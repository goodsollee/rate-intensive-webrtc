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

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
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
double PacingController::pudica_rho_override_ = 0;
bool PacingController::pudica_intra_frame_pacing_ = false;
namespace {
// A frame every 33 ms, so this is ~17 s of outstanding frames. The list only
// grows while feedback is absent, and Pudica's own fallback bottoms out long
// before then; the bound exists so a permanently dead return path cannot leak.
constexpr size_t kPudicaMaxSentFrames = 512;

struct PudicaSentFrames {
  Mutex lock;
  std::deque<int64_t> frames RTC_GUARDED_BY(lock);
};
// Function-local, intentionally never destroyed: a namespace-scope object with
// a destructor trips -Wexit-time-destructors, and the pacer thread may still be
// running at teardown.
PudicaSentFrames& SentFrames() {
  static PudicaSentFrames* const s = new PudicaSentFrames();
  return *s;
}
}  // namespace

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

// L: frame sending interval (30 fps). Shared by the Eq.2 intra-frame pacing
// span, the T_packet probe spacing and the agnostic-period hold below.
constexpr double kPudicaFrameIntervalUs = 33333.0;

void PacingController::SetPudicaProbing(bool enabled, int num_probes) {
  pudica_probing_enabled_ = enabled;
  pudica_num_probes_ = num_probes;
  if (enabled) {
    RTC_LOG(LS_INFO) << "[PUDICA] Probe injection enabled: num_probes="
                     << num_probes;
  }
}

void PacingController::SetPudicaIntraFramePacing(bool enabled) {
  pudica_intra_frame_pacing_ = enabled;
  RTC_LOG(LS_INFO) << "[PUDICA] Intra-frame pacing (Eq.2) "
                   << (enabled ? "enabled" : "disabled");
}

void PacingController::SetPudicaRho(double rho) {
  pudica_rho_override_ = rho;
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
    // Pudica Eq.2: no burst allowance while spreading a frame. The default
    // min(40ms, 63KB/rate) lets the whole median frame (55 pkts = 66KB, i.e.
    // ~kMaxBurstSize) go out back-to-back, which bypasses pudica_frame_rate_
    // entirely and leaves the send span at ~0 (observed: PUDICA-MARKER
    // send=0.0ms, DU backlog p90 = 55 packets = exactly one burst).
    TimeDelta send_burst_interval =
        (pudica_frame_rate_ > DataRate::Zero())
            ? TimeDelta::Zero()
            : std::min(send_burst_interval_, kMaxBurstSize / adjusted_media_rate_);
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
  if (pudica_probing_enabled_) {
    if (pudica_probes_remaining_ > 0 && pudica_next_probe_time_.IsFinite()) {
      next_send_time = std::min(next_send_time, pudica_next_probe_time_);
    }
    if (now < pudica_gap_end_time_ && pudica_gap_end_time_.IsFinite()) {
      next_send_time = std::min(next_send_time, pudica_gap_end_time_);
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

  // Pudica Eq.2 deadline guard. pudica_frame_rate_ below is computed once, from
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
  // Measured (run 1785486585, trace t=45.0s, 540p->720p->1080p ramp after the
  // t=20s dip): ~9 packets seen where the keyframe was 224, giving 3.2 Mbps on
  // an idle 40 Mbps link. The keyframe sat 637 ms in the pacer (its own
  // pacing_ms), the RAN queue stayed under 5 packets, GCC saw no overuse, and
  // Pudica's own target was 35.9 Mbps the whole time. The receiver got zero
  // frames for 810 ms and then 33 in 400 ms.
  //
  // Eq.2's premise is that a frame leaves within L/rho, which is strictly less
  // than L. Taking longer than L therefore means the size estimate was wrong.
  // Drop it and let the next video packet re-arm from the queue as it stands by
  // then — which holds the rest of the frame, so the recomputed rate is right.
  // This bounds the damage to one frame interval instead of the whole frame.
  //
  // Note this is a DEADLINE, not a rate bound. An upper bound of B*rho was
  // tried here before and reverted: anchored to B, it closed a positive
  // feedback loop (B down -> bound down -> frame slower -> Eq.1's D counts it
  // -> BUR up -> B down). A deadline is independent of B and cannot do that.
  if (pudica_probing_enabled_ && pudica_intra_frame_pacing_ &&
      pudica_frame_send_start_.IsFinite() &&
      now - pudica_frame_send_start_ >
          TimeDelta::Micros(static_cast<int64_t>(kPudicaFrameIntervalUs))) {
    ++pudica_frame_deadline_hits_;
    fprintf(stderr,
            "[PUDICA-PACE-DEADLINE] frame overran L: held=%.0fms "
            "rate=%.1fMbps queue=%.0fKB hits=%d\n",
            (now - pudica_frame_send_start_).ms<double>(),
            pudica_frame_rate_.bps() / 1e6, QueueSizeData().bytes<double>() / 1024.0,
            pudica_frame_deadline_hits_);
    pudica_frame_rate_ = DataRate::Zero();
    adjusted_media_rate_ = pacing_rate_;
    pudica_frame_send_start_ = Timestamp::MinusInfinity();
    pudica_frame_hit_deadline_ = true;
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
  for (; iteration < circuit_breaker_threshold_; ++iteration) {
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
        // Eq.2: spread this frame's packets evenly over L/rho so the last one
        // leaves at L/rho. All packets of a frame are enqueued as one batch, so
        // the queue at frame start plus this packet is the frame size. Without
        // this the frame goes out as a burst at pacing_rate_ and the send span
        // becomes proportional to the frame size, which contaminates the Eq.1
        // BUR with sender-side serialization (measured: corr(frame_packets,
        // base_bur) = +0.73, BUR > 1 on an empty 200 Mbps link).
        if (pudica_intra_frame_pacing_) {
          double rho = pudica_rho_override_ > 1.0 ? pudica_rho_override_ : 2.0;
          DataSize frame_size = packet_size + QueueSizeData();
          TimeDelta span = TimeDelta::Micros(
              static_cast<int64_t>(kPudicaFrameIntervalUs / rho));
          if (span > TimeDelta::Zero() && frame_size > DataSize::Zero()) {
            // No rate clamp: an above-nominal frame is paced proportionally
            // faster, which does burst past the link. Bounding it at B*rho was
            // tried and reverted — B is what BUR controls, so the bound closed a
            // positive feedback loop (B down -> bound down -> frame takes longer
            // -> Eq.1's D counts that -> BUR up -> B down). Measured: 148 KB
            // frames taking 245 ms, then a 4 s collapse to 2.8 Mbps on a 40 Mbps
            // link. Any bound here must be anchored to something independent of
            // B, and Eq.1 cannot tell a slow pacer from a full queue anyway.
            pudica_frame_rate_ = frame_size / span;
            // MaybeUpdateMediaRateDueToLongQueue() only runs at the end of
            // ProcessPackets(), so apply it here too — otherwise the rest of
            // this send batch still goes out at the old (bursty) rate.
            adjusted_media_rate_ = pudica_frame_rate_;
          }
        }
        // Trace: what Eq.2 armed from, so it can be compared with what the
        // frame turned out to be. See the member declarations.
        pudica_arm_size_ = packet_size + QueueSizeData();
        pudica_arm_queue_ = QueueSizeData();
        pudica_arm_rate_ = pudica_frame_rate_;
        pudica_frame_bytes_sent_ = DataSize::Zero();
        pudica_frame_pkts_sent_ = 0;
        pudica_frame_hit_deadline_ = false;
      }

      // Pudica: detect video frame end (marker bit) before packet is moved
      bool pudica_frame_ended = false;
      if (pudica_probing_enabled_ &&
          packet_type == RtpPacketMediaType::kVideo &&
          rtp_packet->Marker()) {
        pudica_frame_ended = true;
      }

      packet_sender_->SendPacket(std::move(rtp_packet), pacing_info);
      for (auto& packet : packet_sender_->FetchFec()) {
        EnqueuePacket(std::move(packet));
      }
      data_sent += packet_size;
      ++packets_sent;
      if (packet_type == RtpPacketMediaType::kVideo) {
        pudica_frame_bytes_sent_ += packet_size;
        ++pudica_frame_pkts_sent_;
      }

      // Pudica: schedule deferred probe packets after frame ends
      if (pudica_frame_ended) {
        static int marker_count = 0;
        marker_count++;
        // §4.3 next delay: this frame is now fully sent and outstanding.
        PudicaRecordFrameSent(now.us());
        // Compute T_packet = (1-1/ρ) × L / (N+1)
        double rho = pudica_rho_override_ > 1.0 ? pudica_rho_override_ : 2.0;
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
        double gap_us = std::min(desired_gap_us, remaining_us);
        pudica_gap_end_time_ = now + TimeDelta::Micros(
            static_cast<int64_t>(gap_us));
        pudica_frame_send_start_ = Timestamp::MinusInfinity();  // reset for next frame
        double used_mbps = pudica_frame_rate_.bps() / 1e6;
        pudica_frame_rate_ = DataRate::Zero();  // revert to pacing_rate_ in the gap

        // Per-frame pacer trace. armed_* is what Eq.2 sized the rate from;
        // sent_* is what the frame actually was. span_ms against
        // L/rho = the requested span says whether the pacer met the rate it was
        // given, and armed vs sent says whether the rate was right to begin with.
        static const bool kPaceTrace = []() {
          const char* e = std::getenv("PUDICA_PACE_TRACE");
          return e && std::atoi(e) != 0;
        }();
        if (kPaceTrace) {
          if (!pudica_pace_initialized_) {
            const char* dir = std::getenv("UNIFIED_CSV_DIR");
            if (dir && dir[0]) {
              std::string path = std::string(dir) + "/pudica_pace.csv";
              pudica_pace_file_.open(path, std::ios::out | std::ios::trunc);
              if (pudica_pace_file_.is_open()) {
                chmod(path.c_str(), 0666);
                pudica_pace_file_
                    << "time_ms,armed_bytes,armed_queue_bytes,armed_rate_mbps,"
                       "sent_bytes,sent_pkts,span_ms,want_span_ms,"
                       "achieved_mbps,queue_end_bytes,pacing_rate_mbps,"
                       "hit_deadline\n";
              }
              pudica_pace_start_ = now;
              pudica_pace_initialized_ = true;
            }
          }
          if (pudica_pace_file_.is_open()) {
            double span_ms = frame_send_us / 1000.0;
            double achieved = span_ms > 0.0
                ? pudica_frame_bytes_sent_.bytes<double>() * 8.0 /
                      (span_ms / 1000.0) / 1e6
                : 0.0;
            pudica_pace_file_
                << (now - pudica_pace_start_).ms() << ','
                << pudica_arm_size_.bytes() << ','
                << pudica_arm_queue_.bytes() << ','
                << pudica_arm_rate_.bps() / 1e6 << ','
                << pudica_frame_bytes_sent_.bytes() << ','
                << pudica_frame_pkts_sent_ << ','
                << span_ms << ',' << (kL_us / rho / 1000.0) << ','
                << achieved << ',' << QueueSizeData().bytes() << ','
                << pacing_rate_.bps() / 1e6 << ','
                << (pudica_frame_hit_deadline_ ? 1 : 0) << '\n';
          }
        }

        if (marker_count % 100 == 0) {
          fprintf(stderr, "[PUDICA-MARKER] markers=%d T_pkt=%.1fms gap=%.1f/%.1fms rho=%.1f send=%.1fms "
                  "paced=%.1fMbps span=%.1fms\n",
                  marker_count, T_packet_us / 1000.0, gap_us / 1000.0,
                  desired_gap_us / 1000.0, rho, frame_send_us / 1000.0,
                  used_mbps, kL_us / rho / 1000.0);
        }
      }

      // Send done, update send time.
      OnPacketSent(packet_type, packet_size, now);

      if (is_probing) {
        pacing_info.probe_cluster_bytes_sent += packet_size.bytes();
        // If we are currently probing, we need to stop the send loop when we
        // have reached the send target.
        if (data_sent >= recommended_probe_size) {
          break;
        }
      }

      // Update target send time in case that are more packets that we are late
      // in processing.
      target_send_time = NextSendTime();
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

    // Pudica Eq.2: while spreading a frame, apply the strict debt check too —
    // otherwise packets are still allowed out early as a burst.
    if (now <= target_send_time && (send_burst_interval_.IsZero() ||
                                    pudica_frame_rate_ > DataRate::Zero())) {
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
  adjusted_media_rate_ = pacing_rate_;
  // Queue-depth watch: Pudica keeps pudica_frame_rate_ set almost continuously,
  // so the drain_large_queues bump below is effectively never reached. Report
  // when the queue crosses queue_time_limit_ in either direction, to show
  // whether the pacer queue is what starves the encoder.
  if (adjusted_media_rate_ > DataRate::Zero()) {
    TimeDelta qt = QueueSizeData() / adjusted_media_rate_;
    bool over = qt > queue_time_limit_;
    if (over != queue_over_limit_) {
      queue_over_limit_ = over;
      fprintf(stderr,
              "[PACER-QUEUE] %s qtime=%.0fms limit=%.0fms pkts=%d bytes=%.0fKB "
              "rate=%.1fMbps pudica_frame=%d\n",
              over ? "OVER" : "under", qt.ms<double>(),
              queue_time_limit_.ms<double>(),
              static_cast<int>(packet_queue_.SizeInPackets()),
              QueueSizeData().bytes() / 1000.0,
              adjusted_media_rate_.bps() / 1e6,
              pudica_frame_rate_ > DataRate::Zero() ? 1 : 0);
    }
  }
  // Pudica Eq.2: while a frame is being sent, its own span is the control
  // variable. Skip the long-queue bump — it would speed the frame back up to
  // drain the queue and undo the spreading.
  if (pudica_frame_rate_ > DataRate::Zero()) {
    adjusted_media_rate_ = pudica_frame_rate_;
    return;
  }
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
