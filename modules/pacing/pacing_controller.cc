/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/pacing/pacing_controller.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "api/array_view.h"
#include "api/field_trials_view.h"
#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "modules/pacing/bitrate_prober.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/numerics/safe_conversions.h"
#include "rtc_base/synchronization/mutex.h"
#include "system_wrappers/include/clock.h"

namespace webrtc {
namespace {
constexpr TimeDelta kCongestedPacketInterval = TimeDelta::Millis(500);
// TODO(sprang): Consider dropping this limit.
// The maximum debt level, in terms of time, capped when sending packets.
constexpr TimeDelta kMaxDebtInTime = TimeDelta::Millis(500);
constexpr TimeDelta kMaxElapsedTime = TimeDelta::Seconds(2);

bool IsDisabled(const FieldTrialsView& field_trials, absl::string_view key) {
  return absl::StartsWith(field_trials.Lookup(key), "Disabled");
}

bool IsEnabled(const FieldTrialsView& field_trials, absl::string_view key) {
  return absl::StartsWith(field_trials.Lookup(key), "Enabled");
}

}  // namespace

const TimeDelta PacingController::kPausedProcessInterval =
    kCongestedPacketInterval;
const TimeDelta PacingController::kMinSleepTime = TimeDelta::Millis(1);
const TimeDelta PacingController::kTargetPaddingDuration = TimeDelta::Millis(5);
const TimeDelta PacingController::kMaxPaddingReplayDuration =
    TimeDelta::Millis(50);
const TimeDelta PacingController::kMaxEarlyProbeProcessing =
    TimeDelta::Millis(1);

// Pudica static members
bool PacingController::pudica_probing_enabled_ = false;
int PacingController::pudica_num_probes_ = 4;
std::atomic<double> PacingController::pudica_bur_{1.0};
bool PacingController::pudica_intra_frame_pacing_ = false;
bool PacingController::pudica_mode_ = false;

// L: frame sending interval (30 fps). Shared by the Eq.2 spread span, the
// T_packet probe spacing and the agnostic-period hold.
constexpr double kPudicaFrameIntervalUs = 33333.0;

namespace {
// [A28] §4.3 next delay: send-time ledger of outstanding frames.
//
// A frame every 33 ms, so this is ~17 s of outstanding frames. The list only
// grows while feedback is absent, and the fallback bottoms out (MAX_STEPS)
// long before then; the bound exists so a permanently dead return path cannot
// leak memory.
constexpr size_t kPudicaMaxSentFrames = 512;

struct PudicaSentFrames {
  Mutex lock;
  std::deque<int64_t> frames RTC_GUARDED_BY(lock);
};
// Function-local and intentionally never destroyed: a namespace-scope object
// with a destructor trips -Wexit-time-destructors, and the pacer thread may
// still be running at teardown.
PudicaSentFrames& SentFrames() {
  static PudicaSentFrames* const s = new PudicaSentFrames();
  return *s;
}
}  // namespace

void PacingController::SetPudicaMode(bool enabled) {
  pudica_mode_ = enabled;
}

void PacingController::PudicaRecordFrameSent(int64_t send_us) {
  PudicaSentFrames& s = SentFrames();
  MutexLock lock(&s.lock);
  s.frames.push_back(send_us);
  if (s.frames.size() > kPudicaMaxSentFrames) {
    s.frames.pop_front();
  }
}

int64_t PacingController::PudicaOldestUnackedSendUs(int64_t acked_through_us) {
  PudicaSentFrames& s = SentFrames();
  MutexLock lock(&s.lock);
  while (!s.frames.empty() && s.frames.front() <= acked_through_us) {
    s.frames.pop_front();
  }
  return s.frames.empty() ? 0 : s.frames.front();
}

void PacingController::SetPudicaProbing(bool enabled, int num_probes) {
  pudica_probing_enabled_ = enabled;
  pudica_num_probes_ = num_probes;
  // [A25] Eq.2 rides on the same COORDINATOR_MODE=pudica switch as probing,
  // because ρ is only meaningful once a BUR is being produced. It can be
  // turned off on its own for an A/B against the burst-then-idle behaviour
  // every Pudica run before this one had.
  //
  // [A27] SCOPE, because run 1788956870 was read as a clean baseline and is
  // not one: this flag gates ONLY the intra-frame spread (the frame-start arm,
  // the L deadline guard and its wake-up). The agnostic gap and the Eq.5 probe
  // spacing are gated on pudica_probing_enabled_ and keep using the ADAPTIVE ρ,
  // which is deliberate -- Eq.5's T_packet is defined in terms of ρ, so pinning
  // it here would silently change the probe correction as well. A run that
  // wants no adaptive ρ anywhere needs PUDICA_RHO=<fixed> too.
  static const bool kIntraFrame = []() {
    const char* e = std::getenv("PUDICA_INTRA_FRAME_PACING");
    return !(e && std::atoi(e) == 0);  // default ON
  }();
  pudica_intra_frame_pacing_ = enabled && kIntraFrame;
  if (enabled) {
    RTC_LOG(LS_INFO) << "[PUDICA] Probe injection enabled: num_probes="
                     << num_probes << " intra_frame_pacing="
                     << pudica_intra_frame_pacing_;
  }
}

// [A27] PUDICA_RHO pins ρ, disabling Eq.2's adaptation. Read here, once, at
// the single site that computes ρ.
//
// It used to arrive through SetPudicaRho(), called per frame from the
// coordinator. A26 removed that call (it was the duplicate Eq.2 implementation)
// and left `pudica_rho_override_` with no writer, so the pin silently stopped
// working: run 1788956971 was launched with PUDICA_RHO=2 and logged ρ of 1.25,
// 4.58, 5.16, 8.25, 20.62 — every value except 2. A knob that is read from a
// variable nobody writes fails exactly this way, so the env read now lives
// next to its only reader.
double PacingController::PudicaRhoOverride() {
  static const double kRho = []() {
    const char* e = std::getenv("PUDICA_RHO");
    // An empty string is "explicitly unset" (the runner passes PUDICA_RHO= to
    // mean that), and atof("") is 0, which is already <= 1.0 and so not a pin.
    return e ? std::atof(e) : 0.0;
  }();
  return kRho;
}

void PacingController::SetPudicaBur(double bur) {
  // Guard the value at the boundary so PudicaRho() never has to. NaN compares
  // false against everything, so the >= form rejects it.
  pudica_bur_.store(bur >= 0.0 ? bur : 0.0, std::memory_order_relaxed);
}

// [A25] Pudica NSDI'24 Eq.2:  ρ = γ_ρ / min(R, 1),  γ_ρ = 1.25.
//
// R is the BUR: (D − D_min)/L, the bottleneck queuing delay expressed in frame
// intervals. The frame's send span is L/ρ, so
//
//     L/ρ = L · min(R,1) / γ_ρ
//
// and the two clauses of the paper fall out directly:
//
//   R < 1 : span = (D − D_min)/γ_ρ — "slightly shorter than the queuing
//           delay", which is the point. The frame occupies the wire for less
//           time than the queue takes to drain, so the queue never grows by a
//           whole frame, but the sender is still busy long enough that the
//           NEXT frame's D is a measurement of the link rather than of its own
//           serialization. That is the "sensible period" being extended.
//   R ≥ 1 : the denominator is capped at 1, so span = L/γ_ρ = 0.8 L. "By
//           bounding the denominator up to one, all packets of each frame are
//           sent within the frame interval, to avoid superfluous waiting time
//           at the sender." Without the cap a queued link (R = 8, seen on
//           every dip in this testbed) would give ρ = 0.156, i.e. a span of
//           6.4 L — the sender would sit on a frame for six frame intervals
//           while the encoder produced six more.
//
// The other bound is ours, not the paper's. R → 0 sends ρ → ∞ and the span to
// zero, which is *correct* (an empty queue needs no pacing) but degenerates
// into an unbounded rate. The floor is applied to the SPAN rather than to ρ,
// because a span is the thing with a physical meaning here: below one packet
// serialization there is nothing left to spread. kPudicaMinSpanUs = 1 ms is
// ~3% of L, i.e. still a burst for practical purposes, and it exists only so
// `frame_size / span` cannot produce an infinity.
//
// R is the RAW per-frame BUR, published from the feedback path (the single
// SetPudicaBur() call site). delay_based_bwe.cc notes that 29% of raw samples
// come from 1-packet frames reading a constant 0.030, which arms the minimum
// span; PUDICA_RHO_SMOOTH_BUR=1 switches to the smoothed BUR for an A/B.
double PacingController::PudicaRho() {
  static const double kGammaRho = []() {
    const char* e = std::getenv("PUDICA_GAMMA_RHO");
    return e ? std::atof(e) : 1.25;
  }();
  // The manual pin wins and disables adaptation, for A/B runs against Eq.2.
  const double pin = PudicaRhoOverride();
  if (pin > 1.0) return pin;
  double r = pudica_bur_.load(std::memory_order_relaxed);
  if (!(r > 0.0)) r = 1.0;   // no BUR yet: behave as a saturated link
  double denom = std::min(r, 1.0);
  double rho = kGammaRho / denom;
  // Span floor, expressed as a ρ ceiling so every consumer sees one number.
  constexpr double kPudicaMinSpanUs = 1000.0;
  const double rho_max = kPudicaFrameIntervalUs / kPudicaMinSpanUs;
  if (rho > rho_max) rho = rho_max;
  if (rho < 1.0) rho = 1.0;
  return rho;
}

PacingController::PacingController(Clock* clock,
                                   PacketSender* packet_sender,
                                   const FieldTrialsView& field_trials,
                                   Configuration configuration)
    : clock_(clock),
      packet_sender_(packet_sender),
      field_trials_(field_trials),
      drain_large_queues_(
          configuration.drain_large_queues &&
          !IsDisabled(field_trials_, "WebRTC-Pacer-DrainQueue")),
      send_padding_if_silent_(
          IsEnabled(field_trials_, "WebRTC-Pacer-PadInSilence")),
      pace_audio_(IsEnabled(field_trials_, "WebRTC-Pacer-BlockAudio")),
      ignore_transport_overhead_(
          IsEnabled(field_trials_, "WebRTC-Pacer-IgnoreTransportOverhead")),
      fast_retransmissions_(
          IsEnabled(field_trials_, "WebRTC-Pacer-FastRetransmissions")),
      keyframe_flushing_(
          configuration.keyframe_flushing ||
          IsEnabled(field_trials_, "WebRTC-Pacer-KeyframeFlushing")),
      transport_overhead_per_packet_(DataSize::Zero()),
      send_burst_interval_(configuration.send_burst_interval),
      last_timestamp_(clock_->CurrentTime()),
      paused_(false),
      media_debt_(DataSize::Zero()),
      padding_debt_(DataSize::Zero()),
      pacing_rate_(DataRate::Zero()),
      adjusted_media_rate_(DataRate::Zero()),
      padding_rate_(DataRate::Zero()),
      prober_(field_trials_),
      probing_send_failure_(false),
      last_process_time_(clock->CurrentTime()),
      last_send_time_(last_process_time_),
      seen_first_packet_(false),
      packet_queue_(/*creation_time=*/last_process_time_,
                    configuration.prioritize_audio_retransmission,
                    configuration.packet_queue_ttl),
      congested_(false),
      queue_time_limit_(configuration.queue_time_limit),
      account_for_audio_(false),
      include_overhead_(false),
      circuit_breaker_threshold_(1 << 16) {
  if (!drain_large_queues_) {
    RTC_LOG(LS_WARNING) << "Pacer queues will not be drained,"
                           "pushback experiment must be enabled.";
  }
}

PacingController::~PacingController() = default;

void PacingController::CreateProbeClusters(
    rtc::ArrayView<const ProbeClusterConfig> probe_cluster_configs) {
  for (const ProbeClusterConfig probe_cluster_config : probe_cluster_configs) {
    prober_.CreateProbeCluster(probe_cluster_config);
  }
}

void PacingController::Pause() {
  if (!paused_)
    RTC_LOG(LS_INFO) << "PacedSender paused.";
  paused_ = true;
  packet_queue_.SetPauseState(true, CurrentTime());
}

void PacingController::Resume() {
  if (paused_)
    RTC_LOG(LS_INFO) << "PacedSender resumed.";
  paused_ = false;
  packet_queue_.SetPauseState(false, CurrentTime());
}

bool PacingController::IsPaused() const {
  return paused_;
}

void PacingController::SetCongested(bool congested) {
  if (congested_ && !congested) {
    UpdateBudgetWithElapsedTime(UpdateTimeAndGetElapsed(CurrentTime()));
  }
  congested_ = congested;
}

void PacingController::SetCircuitBreakerThreshold(int num_iterations) {
  circuit_breaker_threshold_ = num_iterations;
}

void PacingController::RemovePacketsForSsrc(uint32_t ssrc) {
  packet_queue_.RemovePacketsForSsrc(ssrc);
}

bool PacingController::IsProbing() const {
  return prober_.is_probing();
}

Timestamp PacingController::CurrentTime() const {
  Timestamp time = clock_->CurrentTime();
  if (time < last_timestamp_) {
    RTC_LOG(LS_WARNING)
        << "Non-monotonic clock behavior observed. Previous timestamp: "
        << last_timestamp_.ms() << ", new timestamp: " << time.ms();
    RTC_DCHECK_GE(time, last_timestamp_);
    time = last_timestamp_;
  }
  last_timestamp_ = time;
  return time;
}

void PacingController::SetProbingEnabled(bool enabled) {
  RTC_CHECK(!seen_first_packet_);
  prober_.SetEnabled(enabled);
}

void PacingController::SetPacingRates(DataRate pacing_rate,
                                      DataRate padding_rate) {
  RTC_CHECK_GT(pacing_rate, DataRate::Zero());
  RTC_CHECK_GE(padding_rate, DataRate::Zero());
  if (padding_rate > pacing_rate) {
    RTC_LOG(LS_WARNING) << "Padding rate " << padding_rate.kbps()
                        << "kbps is higher than the pacing rate "
                        << pacing_rate.kbps() << "kbps, capping.";
    padding_rate = pacing_rate;
  }

  if (pacing_rate > max_rate || padding_rate > max_rate) {
    RTC_LOG(LS_WARNING) << "Very high pacing rates ( > " << max_rate.kbps()
                        << " kbps) configured: pacing = " << pacing_rate.kbps()
                        << " kbps, padding = " << padding_rate.kbps()
                        << " kbps.";
    max_rate = std::max(pacing_rate, padding_rate) * 1.1;
  }
  pacing_rate_ = pacing_rate;
  padding_rate_ = padding_rate;
  MaybeUpdateMediaRateDueToLongQueue(CurrentTime());

  RTC_LOG(LS_VERBOSE) << "bwe:pacer_updated pacing_kbps=" << pacing_rate_.kbps()
                      << " padding_budget_kbps=" << padding_rate.kbps();
}

void PacingController::EnqueuePacket(std::unique_ptr<RtpPacketToSend> packet) {
  RTC_DCHECK(pacing_rate_ > DataRate::Zero())
      << "SetPacingRate must be called before InsertPacket.";
  RTC_CHECK(packet->packet_type());

  if (keyframe_flushing_ &&
      packet->packet_type() == RtpPacketMediaType::kVideo &&
      packet->is_key_frame() && packet->is_first_packet_of_frame() &&
      !packet_queue_.HasKeyframePackets(packet->Ssrc())) {
    // First packet of a keyframe (and no keyframe packets currently in the
    // queue). Flush any pending packets currently in the queue for that stream
    // in order to get the new keyframe out as quickly as possible.
    packet_queue_.RemovePacketsForSsrc(packet->Ssrc());
    std::optional<uint32_t> rtx_ssrc =
        packet_sender_->GetRtxSsrcForMedia(packet->Ssrc());
    if (rtx_ssrc) {
      packet_queue_.RemovePacketsForSsrc(*rtx_ssrc);
    }
  }

  prober_.OnIncomingPacket(DataSize::Bytes(packet->payload_size()));

  const Timestamp now = CurrentTime();
  if (packet_queue_.Empty()) {
    // If queue is empty, we need to "fast-forward" the last process time,
    // so that we don't use passed time as budget for sending the first new
    // packet.
    Timestamp target_process_time = now;
    Timestamp next_send_time = NextSendTime();
    if (next_send_time.IsFinite()) {
      // There was already a valid planned send time, such as a keep-alive.
      // Use that as last process time only if it's prior to now.
      target_process_time = std::min(now, next_send_time);
    }
    UpdateBudgetWithElapsedTime(UpdateTimeAndGetElapsed(target_process_time));
  }
  packet_queue_.Push(now, std::move(packet));
  seen_first_packet_ = true;

  // Queue length has increased, check if we need to change the pacing rate.
  MaybeUpdateMediaRateDueToLongQueue(now);
}

void PacingController::SetAccountForAudioPackets(bool account_for_audio) {
  account_for_audio_ = account_for_audio;
}

void PacingController::SetIncludeOverhead() {
  include_overhead_ = true;
}

void PacingController::SetTransportOverhead(DataSize overhead_per_packet) {
  if (ignore_transport_overhead_)
    return;
  transport_overhead_per_packet_ = overhead_per_packet;
}

void PacingController::SetSendBurstInterval(TimeDelta burst_interval) {
  send_burst_interval_ = burst_interval;
}

void PacingController::SetAllowProbeWithoutMediaPacket(bool allow) {
  prober_.SetAllowProbeWithoutMediaPacket(allow);
}

TimeDelta PacingController::ExpectedQueueTime() const {
  RTC_DCHECK_GT(adjusted_media_rate_, DataRate::Zero());
  return QueueSizeData() / adjusted_media_rate_;
}

size_t PacingController::QueueSizePackets() const {
  return rtc::checked_cast<size_t>(packet_queue_.SizeInPackets());
}

const std::array<int, kNumMediaTypes>&
PacingController::SizeInPacketsPerRtpPacketMediaType() const {
  return packet_queue_.SizeInPacketsPerRtpPacketMediaType();
}

DataSize PacingController::QueueSizeData() const {
  DataSize size = packet_queue_.SizeInPayloadBytes();
  if (include_overhead_) {
    size += static_cast<int64_t>(packet_queue_.SizeInPackets()) *
            transport_overhead_per_packet_;
  }
  return size;
}

DataSize PacingController::CurrentBufferLevel() const {
  return std::max(media_debt_, padding_debt_);
}

std::optional<Timestamp> PacingController::FirstSentPacketTime() const {
  return first_sent_packet_time_;
}

Timestamp PacingController::OldestPacketEnqueueTime() const {
  return packet_queue_.OldestEnqueueTime();
}

TimeDelta PacingController::UpdateTimeAndGetElapsed(Timestamp now) {
  // If no previous processing, or last process was "in the future" because of
  // early probe processing, then there is no elapsed time to add budget for.
  if (last_process_time_.IsMinusInfinity() || now < last_process_time_) {
    return TimeDelta::Zero();
  }
  TimeDelta elapsed_time = now - last_process_time_;
  last_process_time_ = now;
  if (elapsed_time > kMaxElapsedTime) {
    RTC_LOG(LS_WARNING) << "Elapsed time (" << elapsed_time
                        << ") longer than expected, limiting to "
                        << kMaxElapsedTime;
    elapsed_time = kMaxElapsedTime;
  }
  return elapsed_time;
}

bool PacingController::ShouldSendKeepalive(Timestamp now) const {
  if (send_padding_if_silent_ || paused_ || congested_ || !seen_first_packet_) {
    // We send a padding packet every 500 ms to ensure we won't get stuck in
    // congested state due to no feedback being received.
    if (now - last_send_time_ >= kCongestedPacketInterval) {
      return true;
    }
  }
  return false;
}

Timestamp PacingController::NextSendTime() const {
  return NextSendTime(/*include_pudica_wakeups=*/true);
}

Timestamp PacingController::NextSendTime(bool include_pudica_wakeups) const {
  const Timestamp now = CurrentTime();
  Timestamp next_send_time = Timestamp::PlusInfinity();

  if (paused_) {
    return last_send_time_ + kPausedProcessInterval;
  }

  // If probing is active, that always takes priority.
  if (prober_.is_probing() && !probing_send_failure_) {
    Timestamp probe_time = prober_.NextProbeTime(now);
    if (!probe_time.IsPlusInfinity()) {
      return probe_time.IsMinusInfinity() ? now : probe_time;
    }
  }

  // If queue contains a packet which should not be paced, its target send time
  // is the time at which it was enqueued.
  Timestamp unpaced_send_time = NextUnpacedSendTime();
  if (unpaced_send_time.IsFinite()) {
    return unpaced_send_time;
  }

  if (congested_ || !seen_first_packet_) {
    // We need to at least send keep-alive packets with some interval.
    return last_send_time_ + kCongestedPacketInterval;
  }

  if (adjusted_media_rate_ > DataRate::Zero() && !packet_queue_.Empty()) {
    // If packets are allowed to be sent in a burst, the
    // debt is allowed to grow up to one packet more than what can be sent
    // during 'send_burst_period_'.
    TimeDelta drain_time = media_debt_ / adjusted_media_rate_;
    // Ensure that a burst of sent packet is not larger than kMaxBurstSize in
    // order to not risk overfilling socket buffers at high bitrate.
    //
    // [A25] Eq.2: no burst allowance while spreading a frame. The default
    // min(send_burst_interval_, 63KB/rate) lets a whole median frame go out
    // back-to-back, which bypasses pudica_frame_rate_ entirely and leaves the
    // send span at ~0 — i.e. Eq.2 would compute a span and then never apply it.
    TimeDelta send_burst_interval =
        (pudica_frame_rate_ > DataRate::Zero())
            ? TimeDelta::Zero()
            : std::min(send_burst_interval_,
                       kMaxBurstSize / adjusted_media_rate_);
    next_send_time =
        last_process_time_ +
        ((send_burst_interval > drain_time) ? TimeDelta::Zero() : drain_time);
  } else if (padding_rate_ > DataRate::Zero() && packet_queue_.Empty()) {
    // If we _don't_ have pending packets, check how long until we have
    // bandwidth for padding packets. Both media and padding debts must
    // have been drained to do this.
    RTC_DCHECK_GT(adjusted_media_rate_, DataRate::Zero());
    TimeDelta drain_time = std::max(media_debt_ / adjusted_media_rate_,
                                    padding_debt_ / padding_rate_);

    if (drain_time.IsZero() &&
        (!media_debt_.IsZero() || !padding_debt_.IsZero())) {
      // We have a non-zero debt, but drain time is smaller than tick size of
      // TimeDelta, round it up to the smallest possible non-zero delta.
      drain_time = TimeDelta::Micros(1);
    }
    next_send_time = last_process_time_ + drain_time;
  } else {
    // Nothing to do.
    next_send_time = last_process_time_ + kPausedProcessInterval;
  }

  if (send_padding_if_silent_) {
    next_send_time =
        std::min(next_send_time, last_send_time_ + kPausedProcessInterval);
  }

  // Pudica: wake up for deferred probe sending or gap end
  if (pudica_probing_enabled_ && include_pudica_wakeups) {
    if (pudica_probes_remaining_ > 0 && pudica_next_probe_time_.IsFinite()) {
      next_send_time = std::min(next_send_time, pudica_next_probe_time_);
    }
    if (now < pudica_gap_end_time_ && pudica_gap_end_time_.IsFinite()) {
      next_send_time = std::min(next_send_time, pudica_gap_end_time_);
    }
    // [A25] Wake up for the Eq.2 deadline guard at the top of ProcessPackets().
    // Without this the guard's wake-up is scheduled from
    // media_debt_ / adjusted_media_rate_ — i.e. from the very rate it exists to
    // correct, so the lower the bogus rate the later the correction, and an
    // emptied queue falls through to the kPausedProcessInterval (500 ms) branch
    // above. The deadline must not be scheduled by the thing it bounds.
    if (pudica_intra_frame_pacing_ && pudica_frame_send_start_.IsFinite()) {
      next_send_time =
          std::min(next_send_time,
                   pudica_frame_send_start_ +
                       TimeDelta::Micros(
                           static_cast<int64_t>(kPudicaFrameIntervalUs)));
    }
  }

  return next_send_time;
}

void PacingController::ProcessPackets() {
  absl::Cleanup cleanup = [packet_sender = packet_sender_] {
    packet_sender->OnBatchComplete();
  };
  const Timestamp now = CurrentTime();
  Timestamp target_send_time = now;

  // [A25] Eq.2 deadline guard. pudica_frame_rate_ is computed once, from
  // `packet_size + QueueSizeData()` at the instant the frame's FIRST packet is
  // dequeued, on the assumption that a frame is enqueued as one batch. That
  // assumption breaks when the encoder is still delivering the frame — most
  // reliably on a resolution step-up, where the keyframe is large and slow to
  // packetize. The queue then holds a fraction of the frame, the rate is set
  // from that fraction, and nothing corrects it: the rate is only recomputed at
  // the next frame start, and pudica_frame_send_start_ is only cleared by the
  // marker bit, which is the frame's LAST packet. So the frame cannot finish
  // until it drains at the wrong rate, and the rate cannot be fixed until the
  // frame finishes.
  //
  // Eq.2's premise is that a frame leaves within L/ρ, and ρ ≥ 1 by
  // construction, so L/ρ ≤ L. Taking longer than L therefore means the size
  // estimate was wrong. Drop it and let the next video packet re-arm from the
  // queue as it stands by then — which holds the rest of the frame, so the
  // recomputed rate is right. This bounds the damage to one frame interval
  // instead of the whole frame.
  //
  // This guard is why Eq.2 cannot reproduce the unbounded pacer holds measured
  // without it (1.3-1.9 s on runs 1788944778 / 1788946628, against a p90 of
  // 49-74 ms on the sibling branch that has it).
  if (pudica_intra_frame_pacing_ && pudica_frame_send_start_.IsFinite() &&
      now - pudica_frame_send_start_ >
          TimeDelta::Micros(static_cast<int64_t>(kPudicaFrameIntervalUs))) {
    ++pudica_frame_deadline_hits_;
    if (pudica_frame_deadline_hits_ % 100 == 1) {
      fprintf(stderr,
              "[PUDICA-PACE-DEADLINE] frame overran L: held=%.0fms "
              "rate=%.1fMbps queue=%.0fKB rho=%.2f hits=%d\n",
              (now - pudica_frame_send_start_).ms<double>(),
              pudica_frame_rate_.bps() / 1e6,
              QueueSizeData().bytes<double>() / 1024.0, PudicaRho(),
              pudica_frame_deadline_hits_);
    }
    pudica_frame_rate_ = DataRate::Zero();
    adjusted_media_rate_ = pacing_rate_;
    pudica_frame_send_start_ = Timestamp::MinusInfinity();
  }

  if (ShouldSendKeepalive(now)) {
    DataSize keepalive_data_sent = DataSize::Zero();
    // We can not send padding unless a normal packet has first been sent. If
    // we do, timestamps get messed up.
    if (seen_first_packet_) {
      std::vector<std::unique_ptr<RtpPacketToSend>> keepalive_packets =
          packet_sender_->GeneratePadding(DataSize::Bytes(1));
      for (auto& packet : keepalive_packets) {
        keepalive_data_sent +=
            DataSize::Bytes(packet->payload_size() + packet->padding_size());
        packet_sender_->SendPacket(std::move(packet), PacedPacketInfo());
        for (auto& packet : packet_sender_->FetchFec()) {
          EnqueuePacket(std::move(packet));
        }
      }
    }
    OnPacketSent(RtpPacketMediaType::kPadding, keepalive_data_sent, now);
  }

  if (paused_) {
    return;
  }

  TimeDelta early_execute_margin =
      prober_.is_probing() ? kMaxEarlyProbeProcessing : TimeDelta::Zero();

  target_send_time = NextSendTime();
  if (now + early_execute_margin < target_send_time) {
    // We are too early, but if queue is empty still allow draining some debt.
    // Probing is allowed to be sent up to kMinSleepTime early.
    UpdateBudgetWithElapsedTime(UpdateTimeAndGetElapsed(now));
    return;
  }

  TimeDelta elapsed_time = UpdateTimeAndGetElapsed(target_send_time);

  if (elapsed_time > TimeDelta::Zero()) {
    UpdateBudgetWithElapsedTime(elapsed_time);
  }

  PacedPacketInfo pacing_info;
  DataSize recommended_probe_size = DataSize::Zero();
  bool is_probing = prober_.is_probing();
  if (is_probing) {
    // Probe timing is sensitive, and handled explicitly by BitrateProber, so
    // use actual send time rather than target.
    pacing_info = prober_.CurrentCluster(now).value_or(PacedPacketInfo());
    if (pacing_info.probe_cluster_id != PacedPacketInfo::kNotAProbe) {
      recommended_probe_size = prober_.RecommendedMinProbeSize();
      RTC_DCHECK_GT(recommended_probe_size, DataSize::Zero());
    } else {
      // No valid probe cluster returned, probe might have timed out.
      is_probing = false;
    }
  }

  DataSize data_sent = DataSize::Zero();
  int iteration = 0;
  int packets_sent = 0;
  int padding_packets_generated = 0;
  // [A42] A Pudica probe / gap / deadline wake-up gets us here, but it does
  // not make media due. The send loop below must be bounded by the media send
  // time alone: with the probe time in it, a probe that fell due (probes go
  // out only AFTER this loop) pinned target_send_time in the past, the loop
  // never broke, and UpdateTimeAndGetElapsed() drained nothing -- so the
  // whole queue left at once and every byte of it became media debt. Run
  // 1789089163, 63.09 s: ~890 KB out in ~50 ms, debt at the 500 ms cap,
  // then repaid at the post-DRAIN 4.5 Mbps -- 516 ms of silence while 11
  // frames waited in the pacer (pacing_ms 512).
  const bool media_due =
      NextSendTime(/*include_pudica_wakeups=*/false) <=
      now + early_execute_margin;
  for (; media_due && iteration < circuit_breaker_threshold_; ++iteration) {
    // Fetch packet, so long as queue is not empty or budget is not
    // exhausted.
    std::unique_ptr<RtpPacketToSend> rtp_packet =
        GetPendingPacket(pacing_info, target_send_time, now);
    if (rtp_packet == nullptr) {
      // No packet available to send, check if we should send padding.
      if (now - target_send_time > kMaxPaddingReplayDuration) {
        // The target send time is more than `kMaxPaddingReplayDuration` behind
        // the real-time clock. This can happen if the clock is adjusted forward
        // without `ProcessPackets()` having been called at the expected times.
        target_send_time = now - kMaxPaddingReplayDuration;
        last_process_time_ = std::max(last_process_time_, target_send_time);
      }

      DataSize padding_to_add = PaddingToAdd(recommended_probe_size, data_sent);
      if (padding_to_add > DataSize::Zero()) {
        std::vector<std::unique_ptr<RtpPacketToSend>> padding_packets =
            packet_sender_->GeneratePadding(padding_to_add);
        if (!padding_packets.empty()) {
          padding_packets_generated += padding_packets.size();
          for (auto& packet : padding_packets) {
            EnqueuePacket(std::move(packet));
          }
          // Continue loop to send the padding that was just added.
          continue;
        } else {
          // Can't generate padding, still update padding budget for next send
          // time.
          UpdatePaddingBudgetWithSentData(padding_to_add);
        }
      }
      // Can't fetch new packet and no padding to send, exit send loop.
      break;
    } else {
      RTC_DCHECK(rtp_packet);
      RTC_DCHECK(rtp_packet->packet_type().has_value());
      const RtpPacketMediaType packet_type = *rtp_packet->packet_type();
      DataSize packet_size = DataSize::Bytes(rtp_packet->payload_size() +
                                             rtp_packet->padding_size());

      if (include_overhead_) {
        packet_size += DataSize::Bytes(rtp_packet->headers_size()) +
                       transport_overhead_per_packet_;
      }

      // Pudica: track frame send start (first video pkt after gap)
      if (pudica_probing_enabled_ &&
          packet_type == RtpPacketMediaType::kVideo &&
          pudica_frame_send_start_.IsMinusInfinity()) {
        pudica_frame_send_start_ = now;
        // [A25] Eq.2: spread this frame's packets over L/ρ so the last one
        // leaves at L/ρ, instead of bursting the frame at pacing_rate_ and
        // idling for the rest of L. All packets of a frame are enqueued as one
        // batch, so the queue at frame start plus this packet is the frame
        // size.
        //
        // Deliberately NOT clamped to any multiple of the committed rate. An
        // upper bound of B·ρ was tried on the sibling branch and reverted: B is
        // what the BUR controls, so the bound closes a positive feedback loop
        // (B down → bound down → frame takes longer → Eq.1's D counts that →
        // BUR up → B down). The L deadline guard above is the bound instead,
        // and a deadline is independent of B so it cannot do that.
        if (pudica_intra_frame_pacing_) {
          const double rho = PudicaRho();
          DataSize frame_size = packet_size + QueueSizeData();
          TimeDelta span = TimeDelta::Micros(
              static_cast<int64_t>(kPudicaFrameIntervalUs / rho));
          if (span > TimeDelta::Zero() && frame_size > DataSize::Zero()) {
            pudica_frame_rate_ = frame_size / span;
            // MaybeUpdateMediaRateDueToLongQueue() only runs at the END of
            // ProcessPackets(), so apply it here too — otherwise the rest of
            // this send batch still goes out at the old (bursty) rate.
            adjusted_media_rate_ = pudica_frame_rate_;
          }
        }
      }

      // Pudica: detect video frame end (marker bit) before packet is moved.
      // [A28] No longer gated on pudica_probing_enabled_: §4.3 next delay has
      // to time outstanding frames whether or not probes are being injected,
      // and folding the two together made next delay silently dead in any
      // PUDICA_PROBING=0 run. The probe SCHEDULING below keeps the gate.
      const bool pudica_frame_ended =
          packet_type == RtpPacketMediaType::kVideo && rtp_packet->Marker();
      // [A42] This frame was paced by Eq.2 (read before the marker handling
      // below clears the rate).
      const bool pudica_eq2_frame_ended =
          pudica_frame_ended && pudica_frame_rate_ > DataRate::Zero();
      const uint32_t diag_rtp_ts = rtp_packet->Timestamp();  // [DIAG]

      packet_sender_->SendPacket(std::move(rtp_packet), pacing_info);
      for (auto& packet : packet_sender_->FetchFec()) {
        EnqueuePacket(std::move(packet));
      }
      data_sent += packet_size;
      ++packets_sent;

      // [A28] §4.3 next delay: this frame is now fully sent and outstanding.
      // Independent of probing (see above), but still Pudica-only: nothing
      // drains this ledger unless GetPudicaRtpOverride() runs, and that is
      // gated on IsPudicaMode(). Recording in a GCC-arm run would keep a dead
      // 512-entry deque at its bound forever.
      if (pudica_frame_ended && pudica_mode_) {
        PudicaRecordFrameSent(now.us());
      }

      // Pudica: schedule deferred probe packets after frame ends
      if (pudica_frame_ended && pudica_probing_enabled_) {
        static int marker_count = 0;
        marker_count++;
        // Compute T_packet = (1-1/ρ) × L / (N+1)
        // [A25] ρ now comes from Eq.2 instead of a literal 2.0, so the probe
        // spacing, the agnostic gap and the frame spread stay consistent: the
        // agnostic period IS L − L/ρ by definition, and probes are what fills
        // it. A hardcoded 2.0 here against an Eq.2 span elsewhere would place
        // probes inside the frame's own send window on any link with R < 0.625.
        double rho = PudicaRho();
        const double kL_us = kPudicaFrameIntervalUs;
        double T_packet_us = (1.0 - 1.0 / rho) * kL_us /
                             (pudica_num_probes_ + 1);
        T_packet_us = std::max(T_packet_us, 500.0);  // min 0.5ms

        pudica_probes_remaining_ = pudica_num_probes_;
        pudica_probe_interval_ = TimeDelta::Micros(
            static_cast<int64_t>(T_packet_us));
        pudica_frame_end_time_ = now;
        pudica_next_probe_time_ = now + pudica_probe_interval_;

        // ρ-based gap: hold video for remaining time in frame interval.
        // gap = min((1-1/ρ)×L, L - frame_send_time) — never exceeds frame interval.
        double desired_gap_us = (1.0 - 1.0 / rho) * kL_us;
        double frame_send_us = pudica_frame_send_start_.IsFinite()
            ? static_cast<double>((now - pudica_frame_send_start_).us())
            : 0.0;
        double remaining_us = std::max(0.0, kL_us - frame_send_us);
        // [A40] No agnostic period while the next frame is already queued.
        // Eq.2/Eq.5 assume the pacer is empty at frame end -- the next frame
        // arrives one L later, so L - L/ρ is idle time. With video still
        // waiting behind this marker the pacer is behind the encoder, and the
        // gap only idles a link that has work: service drops to one frame per
        // ~L whatever the frame size, and with input at exactly 1/L a backlog
        // never drains. Measured pre-dip on a 200 Mbps link (runs
        // 1789041793/1789041990/1789042110): backlogged frames left every
        // 25-32 ms at 0-100 KB and 300-450 KB alike, pacer wait p50 up to
        // 430 ms while the network delay stayed 5-19 ms. Probes are left as
        // they are (50 B padding; their BUR correction path is unreachable).
        const bool video_waiting =
            packet_queue_.SizeInPacketsPerRtpPacketMediaType()[static_cast<size_t>(
                RtpPacketMediaType::kVideo)] > 0;
        static int gaps_skipped = 0;
        if (video_waiting) ++gaps_skipped;
        double gap_us =
            video_waiting ? 0.0 : std::min(desired_gap_us, remaining_us);
        pudica_gap_end_time_ = now + TimeDelta::Micros(
            static_cast<int64_t>(gap_us));
        // [DIAG] PUDICA_DIAG=1: what A40 saw at this marker. Joined on rtp_ts
        // with [PUDICA-DIAG-ENQ] (task_queue_paced_sender.cc) it tells whether
        // the next frame was already in packet_queue_ or not yet enqueued.
        static const bool kDiag = []() {
          const char* e = std::getenv("PUDICA_DIAG");
          return e && std::atoi(e) == 1;
        }();
        if (kDiag) {
          const Timestamp oldest =
              packet_queue_.LeadingPacketEnqueueTime(RtpPacketMediaType::kVideo);
          fprintf(stderr,
                  "[PUDICA-DIAG-MARK] rtp_ts=%u now_ms=%.3f video_pkts=%d "
                  "queue_kb=%.1f oldest_video_age_ms=%.3f gap_ms=%.3f "
                  "send_ms=%.3f rho=%.2f\n",
                  diag_rtp_ts, now.us() / 1000.0,
                  packet_queue_.SizeInPacketsPerRtpPacketMediaType()
                      [static_cast<size_t>(RtpPacketMediaType::kVideo)],
                  QueueSizeData().bytes<double>() / 1024.0,
                  oldest.IsFinite() ? (now - oldest).us() / 1000.0 : -1.0,
                  gap_us / 1000.0, frame_send_us / 1000.0, rho);
        }
        pudica_frame_send_start_ = Timestamp::MinusInfinity();  // reset for next frame
        // [A25] The frame is done; revert to pacing_rate_ for the gap. The next
        // frame's first packet re-arms Eq.2 from the queue as it stands then.
        pudica_frame_rate_ = DataRate::Zero();

        if (marker_count % 100 == 0) {
          fprintf(stderr,
                  "[PUDICA-MARKER] markers=%d T_pkt=%.1fms gap=%.1f/%.1fms "
                  "rho=%.2f bur=%.3f want_span=%.1fms send=%.1fms "
                  "gaps_skipped=%d\n",
                  marker_count, T_packet_us / 1000.0, gap_us / 1000.0,
                  desired_gap_us / 1000.0, rho,
                  pudica_bur_.load(std::memory_order_relaxed),
                  kL_us / rho / 1000.0, frame_send_us / 1000.0, gaps_skipped);
        }
      }

      // Send done, update send time.
      OnPacketSent(packet_type, packet_size, now);
      // [A42] The frame's bytes were paced by Eq.2 at frame_size / (L/ρ); they
      // are on the wire. Whatever debt is left must not be repaid again at
      // pacing_rate_, which the rate reverts to from here -- after a DRAIN cut
      // that is a few Mbps, and the next frame would wait behind it.
      if (pudica_eq2_frame_ended) {
        media_debt_ = DataSize::Zero();
      }

      if (is_probing) {
        pacing_info.probe_cluster_bytes_sent += packet_size.bytes();
        // If we are currently probing, we need to stop the send loop when we
        // have reached the send target.
        if (data_sent >= recommended_probe_size) {
          break;
        }
      }

      // Update target send time in case that are more packets that we are late
      // in processing. [A42] Media time only -- see `media_due` above.
      target_send_time = NextSendTime(/*include_pudica_wakeups=*/false);
      if (target_send_time > now) {
        // Exit loop if not probing.
        if (!is_probing) {
          break;
        }
        target_send_time = now;
      }
      UpdateBudgetWithElapsedTime(UpdateTimeAndGetElapsed(target_send_time));
    }
  }

  // Pudica: deferred probe sending at T_packet intervals
  if (pudica_probing_enabled_ && pudica_probes_remaining_ > 0) {
    while (pudica_probes_remaining_ > 0 && now >= pudica_next_probe_time_) {
      auto padding = packet_sender_->GeneratePadding(DataSize::Bytes(50));
      if (padding.empty()) break;
      for (auto& p : padding) {
        PacedPacketInfo pudica_info;
        pudica_info.probe_cluster_id = kPudicaProbeClusterId;
        packet_sender_->SendPacket(std::move(p), pudica_info);
      }
      pudica_probes_remaining_--;
      pudica_next_probe_time_ += pudica_probe_interval_;

      static int total_probes = 0;
      total_probes++;
      if (total_probes % 400 == 0) {
        fprintf(stderr, "[PUDICA-PROBE] total=%d interval_us=%lld\n",
                total_probes, (long long)pudica_probe_interval_.us());
      }
    }
  }

  if (iteration >= circuit_breaker_threshold_) {
    // Circuit break activated. Log warning, adjust send time and return.
    // TODO(sprang): Consider completely clearing state.
    RTC_LOG(LS_ERROR)
        << "PacingController exceeded max iterations in "
           "send-loop. Debug info: "
        << " packets sent = " << packets_sent
        << ", padding packets generated = " << padding_packets_generated
        << ", bytes sent = " << data_sent.bytes()
        << ", probing = " << (is_probing ? "true" : "false")
        << ", recommended_probe_size = " << recommended_probe_size.bytes()
        << ", now = " << now.us()
        << ", target_send_time = " << target_send_time.us()
        << ", last_process_time = " << last_process_time_.us()
        << ", last_send_time = " << last_send_time_.us()
        << ", paused = " << (paused_ ? "true" : "false")
        << ", media_debt = " << media_debt_.bytes()
        << ", padding_debt = " << padding_debt_.bytes()
        << ", pacing_rate = " << pacing_rate_.bps()
        << ", adjusted_media_rate = " << adjusted_media_rate_.bps()
        << ", padding_rate = " << padding_rate_.bps()
        << ", queue size (packets) = " << packet_queue_.SizeInPackets()
        << ", queue size (payload bytes) = "
        << packet_queue_.SizeInPayloadBytes();
    last_send_time_ = now;
    last_process_time_ = now;
    return;
  }

  if (is_probing) {
    probing_send_failure_ = data_sent == DataSize::Zero();
    if (!probing_send_failure_) {
      prober_.ProbeSent(CurrentTime(), data_sent);
    }
  }

  // Queue length has probably decreased, check if pacing rate needs to updated.
  // Poll the time again, since we might have enqueued new fec/padding packets
  // with a later timestamp than `now`.
  MaybeUpdateMediaRateDueToLongQueue(CurrentTime());
}

DataSize PacingController::PaddingToAdd(DataSize recommended_probe_size,
                                        DataSize data_sent) const {
  if (!packet_queue_.Empty()) {
    // Actual payload available, no need to add padding.
    return DataSize::Zero();
  }

  if (congested_) {
    // Don't add padding if congested, even if requested for probing.
    return DataSize::Zero();
  }

  if (!recommended_probe_size.IsZero()) {
    if (recommended_probe_size > data_sent) {
      return recommended_probe_size - data_sent;
    }
    return DataSize::Zero();
  }

  if (padding_rate_ > DataRate::Zero() && padding_debt_ == DataSize::Zero()) {
    return kTargetPaddingDuration * padding_rate_;
  }
  return DataSize::Zero();
}

std::unique_ptr<RtpPacketToSend> PacingController::GetPendingPacket(
    const PacedPacketInfo& pacing_info,
    Timestamp target_send_time,
    Timestamp now) {
  const bool is_probe =
      pacing_info.probe_cluster_id != PacedPacketInfo::kNotAProbe;
  // If first packet in probe, insert a small padding packet so we have a
  // more reliable start window for the rate estimation.
  if (is_probe && pacing_info.probe_cluster_bytes_sent == 0) {
    auto padding = packet_sender_->GeneratePadding(DataSize::Bytes(1));
    // If no RTP modules sending media are registered, we may not get a
    // padding packet back.
    if (!padding.empty()) {
      // We should never get more than one padding packets with a requested
      // size of 1 byte.
      RTC_DCHECK_EQ(padding.size(), 1u);
      return std::move(padding[0]);
    }
  }

  if (packet_queue_.Empty()) {
    return nullptr;
  }

  // Pudica gap enforcement: hold video/FEC packets during agnostic period.
  // Audio (prio 0) and retransmissions (prio 1-2) pass through.
  if (pudica_probing_enabled_ && now < pudica_gap_end_time_) {
    // Adapted to baseline PrioritizedPacketQueue API (no
    // TopActivePriorityLevel()): the top active priority level is 3
    // (video/FEC) exactly when no audio or retransmission packets are queued
    // and a video or FEC packet is.
    const std::array<int, kNumMediaTypes>& size_per_type =
        packet_queue_.SizeInPacketsPerRtpPacketMediaType();
    bool higher_prio_pending =
        size_per_type[static_cast<size_t>(RtpPacketMediaType::kAudio)] > 0 ||
        size_per_type[static_cast<size_t>(RtpPacketMediaType::kRetransmission)] >
            0;
    bool video_or_fec_pending =
        size_per_type[static_cast<size_t>(RtpPacketMediaType::kVideo)] > 0 ||
        size_per_type[static_cast<size_t>(
            RtpPacketMediaType::kForwardErrorCorrection)] > 0;
    if (!higher_prio_pending && video_or_fec_pending) {
      // video/FEC — hold during agnostic period
      return nullptr;
    }
  }

  // First, check if there is any reason _not_ to send the next queued packet.
  // Unpaced packets and probes are exempted from send checks.
  if (NextUnpacedSendTime().IsInfinite() && !is_probe) {
    if (congested_) {
      // Don't send anything if congested.
      return nullptr;
    }

    if (now <= target_send_time && send_burst_interval_.IsZero()) {
      // We allow sending slightly early if we think that we would actually
      // had been able to, had we been right on time - i.e. the current debt
      // is not more than would be reduced to zero at the target sent time.
      // If we allow packets to be sent in a burst, packet are allowed to be
      // sent early.
      TimeDelta flush_time = media_debt_ / adjusted_media_rate_;
      if (now + flush_time > target_send_time) {
        return nullptr;
      }
    }
  }

  return packet_queue_.Pop();
}

void PacingController::OnPacketSent(RtpPacketMediaType packet_type,
                                    DataSize packet_size,
                                    Timestamp send_time) {
  if (!first_sent_packet_time_ && packet_type != RtpPacketMediaType::kPadding) {
    first_sent_packet_time_ = send_time;
  }

  bool audio_packet = packet_type == RtpPacketMediaType::kAudio;
  if ((!audio_packet || account_for_audio_) && packet_size > DataSize::Zero()) {
    UpdateBudgetWithSentData(packet_size);
  }

  last_send_time_ = send_time;
}

void PacingController::UpdateBudgetWithElapsedTime(TimeDelta delta) {
  media_debt_ -= std::min(media_debt_, adjusted_media_rate_ * delta);
  padding_debt_ -= std::min(padding_debt_, padding_rate_ * delta);
}

void PacingController::UpdateBudgetWithSentData(DataSize size) {
  media_debt_ += size;
  media_debt_ = std::min(media_debt_, adjusted_media_rate_ * kMaxDebtInTime);
  UpdatePaddingBudgetWithSentData(size);
}

void PacingController::UpdatePaddingBudgetWithSentData(DataSize size) {
  padding_debt_ += size;
  padding_debt_ = std::min(padding_debt_, padding_rate_ * kMaxDebtInTime);
}

void PacingController::SetQueueTimeLimit(TimeDelta limit) {
  queue_time_limit_ = limit;
}

void PacingController::MaybeUpdateMediaRateDueToLongQueue(Timestamp now) {
  // [A42] While Eq.2 spreads a frame, the frame's own rate is the media rate
  // for the whole frame. This runs at the end of every ProcessPackets() and on
  // every EnqueuePacket() / SetPacingRates(), so without this the Eq.2 rate
  // lived for one send batch and the rest of the frame -- and the debt it had
  // run up -- drained at pacing_rate_. (As on bansuk-webrtc.)
  if (pudica_frame_rate_ > DataRate::Zero()) {
    adjusted_media_rate_ = pudica_frame_rate_;
    return;
  }
  adjusted_media_rate_ = pacing_rate_;
  if (!drain_large_queues_) {
    return;
  }

  DataSize queue_size_data = QueueSizeData();
  if (queue_size_data > DataSize::Zero()) {
    // Assuming equal size packets and input/output rate, the average packet
    // has avg_time_left_ms left to get queue_size_bytes out of the queue, if
    // time constraint shall be met. Determine bitrate needed for that.
    packet_queue_.UpdateAverageQueueTime(now);
    TimeDelta avg_time_left =
        std::max(TimeDelta::Millis(1),
                 queue_time_limit_ - packet_queue_.AverageQueueTime());
    DataRate min_rate_needed = queue_size_data / avg_time_left;
    if (min_rate_needed > pacing_rate_) {
      adjusted_media_rate_ = min_rate_needed;
      RTC_LOG(LS_VERBOSE) << "bwe:large_pacing_queue pacing_rate_kbps="
                          << pacing_rate_.kbps();
    }
  }
}

Timestamp PacingController::NextUnpacedSendTime() const {
  if (!pace_audio_) {
    Timestamp leading_audio_send_time =
        packet_queue_.LeadingPacketEnqueueTime(RtpPacketMediaType::kAudio);
    if (leading_audio_send_time.IsFinite()) {
      return leading_audio_send_time;
    }
  }
  if (fast_retransmissions_) {
    Timestamp leading_retransmission_send_time =
        packet_queue_.LeadingPacketEnqueueTimeForRetransmission();
    if (leading_retransmission_send_time.IsFinite()) {
      return leading_retransmission_send_time;
    }
  }
  return Timestamp::MinusInfinity();
}

}  // namespace webrtc
