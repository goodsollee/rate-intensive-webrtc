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
#include <cstdio>
#include <cstdlib>
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

bool PreserveQueuedFramesOnKeyframe() {
  const char* value = std::getenv("KFT_NO_FRAME_DROP");
  // Match the existing VideoStreamEncoder and VP8 no-frame-drop helpers.
  return value && value[0] != '\0' &&
         !(value[0] == '0' && value[1] == '\0');
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
Mutex& PacingController::PudicaConfigMutex() {
  static Mutex& mutex = *new Mutex();
  return mutex;
}

PacingController::PudicaProbeConfig&
PacingController::MutablePudicaProbeConfig() {
  static PudicaProbeConfig& config = *new PudicaProbeConfig();
  return config;
}

void PacingController::SetPudicaProbing(bool enabled,
                                        int num_probes,
                                        TimeDelta frame_interval) {
  const bool valid = !enabled ||
      (num_probes > 0 && num_probes <= 64 && frame_interval.IsFinite() &&
       frame_interval >= TimeDelta::Millis(1) &&
       frame_interval <= TimeDelta::Seconds(1));
  if (!valid) {
    MutexLock lock(&PudicaConfigMutex());
    PudicaProbeConfig& config = MutablePudicaProbeConfig();
    config.enabled = false;
    config.frame_interval_us = 0;
    ++config.revision;
    RTC_LOG(LS_ERROR) << "[PUDICA] Probe injection disabled: invalid N/L, N="
                      << num_probes << " L_us="
                      << (frame_interval.IsFinite() ? frame_interval.us() : -1);
    return;
  }
  {
    MutexLock lock(&PudicaConfigMutex());
    PudicaProbeConfig& config = MutablePudicaProbeConfig();
    config.enabled = enabled;
    config.num_probes = num_probes;
    config.frame_interval_us = enabled ? frame_interval.us() : 0;
    ++config.revision;
  }
  if (enabled) {
    RTC_LOG(LS_INFO) << "[PUDICA] Probe injection enabled: num_probes="
                     << num_probes << " L_us=" << frame_interval.us();
  }
}

void PacingController::SetPudicaRho(double rho) {
  const bool valid = std::isfinite(rho) && rho > 1.0;
  const double resolved_rho = valid ? rho : 2.0;
  MutexLock lock(&PudicaConfigMutex());
  MutablePudicaProbeConfig().rho = resolved_rho;
  if (!valid) {
    RTC_LOG(LS_WARNING) << "[PUDICA] Invalid rho=" << rho
                        << "; using rho=2";
  }
}

PacingController::PudicaProbeConfig
PacingController::ReadPudicaProbeConfig() {
  MutexLock lock(&PudicaConfigMutex());
  return MutablePudicaProbeConfig();
}

void PacingController::RefreshPudicaProbeConfig() {
  PudicaProbeConfig next = ReadPudicaProbeConfig();
  if (next.revision != pudica_config_snapshot_.revision) {
    pudica_probes_remaining_ = 0;
    pudica_next_probe_time_ = Timestamp::MinusInfinity();
    pudica_probe_interval_ = TimeDelta::Zero();
    pudica_gap_end_time_ = Timestamp::MinusInfinity();
    pudica_frame_send_start_ = Timestamp::MinusInfinity();
  }
  pudica_config_snapshot_ = next;
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
      preserve_queued_frames_on_keyframe_(PreserveQueuedFramesOnKeyframe()),
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
    if (preserve_queued_frames_on_keyframe_) {
      // Keep ordinary queue priority/FIFO and debt; enqueue the new packet
      // exactly once below. These are whole-queue counts before enqueue, not
      // per-SSRC victims or successful-send receipts. Bound logging per pacer.
      constexpr unsigned kReceiptLimit = 16;
      if (keyframe_preserve_receipts_ < kReceiptLimit) {
        ++keyframe_preserve_receipts_;
        std::fprintf(stderr,
                     "KFTF KF_FLUSH_BYPASS t_ms=%lld ssrc=%u rtp_ts=%u "
                     "seq=%u queue_pkts_before=%d queue_payload_bytes_before=%lld "
                     "receipt_n=%u receipt_limit=%u\n",
                     static_cast<long long>(clock_->TimeInMilliseconds()),
                     packet->Ssrc(), packet->Timestamp(),
                     static_cast<unsigned>(packet->SequenceNumber()),
                     packet_queue_.SizeInPackets(),
                     static_cast<long long>(
                         packet_queue_.SizeInPayloadBytes().bytes()),
                     keyframe_preserve_receipts_, kReceiptLimit);
      }
    } else {
      packet_queue_.RemovePacketsForSsrc(packet->Ssrc());
      std::optional<uint32_t> rtx_ssrc =
          packet_sender_->GetRtxSsrcForMedia(packet->Ssrc());
      if (rtx_ssrc) {
        packet_queue_.RemovePacketsForSsrc(*rtx_ssrc);
      }
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
    TimeDelta send_burst_interval =
        std::min(send_burst_interval_, kMaxBurstSize / adjusted_media_rate_);
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
  const PudicaProbeConfig pudica_config = ReadPudicaProbeConfig();
  if (pudica_config.revision == pudica_config_snapshot_.revision &&
      pudica_config.enabled) {
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
  RefreshPudicaProbeConfig();
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
      if (pudica_config_snapshot_.enabled &&
          packet_type == RtpPacketMediaType::kVideo &&
          pudica_frame_send_start_.IsMinusInfinity()) {
        pudica_frame_send_start_ = now;
      }

      // Pudica: detect video frame end (marker bit) before packet is moved
      bool pudica_frame_ended = false;
      if (pudica_config_snapshot_.enabled &&
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

      // Pudica: schedule deferred probe packets after frame ends
      if (pudica_frame_ended) {
        ++pudica_marker_count_;
        // Compute T_packet = (1-1/ρ) × L / (N+1)
        const double rho_configured = pudica_config_snapshot_.rho;
        const double rho = rho_configured > 1.0 ? rho_configured : 2.0;
        const int num_probes = pudica_config_snapshot_.num_probes;
        const double kL_us = static_cast<double>(
            pudica_config_snapshot_.frame_interval_us);
        double T_packet_us = (1.0 - 1.0 / rho) * kL_us /
                             (num_probes + 1);
        T_packet_us = std::max(T_packet_us, 500.0);  // min 0.5ms

        pudica_probe_interval_ = TimeDelta::Micros(
            static_cast<int64_t>(T_packet_us));
        pudica_frame_end_time_ = now;

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
        pudica_probes_remaining_ = std::min(
            num_probes,
            static_cast<int>(gap_us / pudica_probe_interval_.us()));
        pudica_next_probe_time_ = pudica_probes_remaining_ > 0
            ? now + pudica_probe_interval_
            : Timestamp::MinusInfinity();
        pudica_frame_send_start_ = Timestamp::MinusInfinity();  // reset for next frame

        if (pudica_marker_count_ % 100 == 0) {
          fprintf(stderr, "[PUDICA-MARKER] markers=%d L=%.3fms T_pkt=%.3fms gap=%.3f/%.3fms probes=%d rho=%.3f send=%.3fms\n",
                  pudica_marker_count_, kL_us / 1000.0,
                  T_packet_us / 1000.0, gap_us / 1000.0,
                  desired_gap_us / 1000.0, pudica_probes_remaining_, rho,
                  frame_send_us / 1000.0);
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
  if (pudica_config_snapshot_.enabled &&
      pudica_probes_remaining_ > 0 && now >= pudica_gap_end_time_) {
    pudica_probes_remaining_ = 0;
    pudica_next_probe_time_ = Timestamp::MinusInfinity();
  }
  if (pudica_config_snapshot_.enabled &&
      pudica_probes_remaining_ > 0 && now >= pudica_next_probe_time_) {
    const Timestamp scheduled_probe_time = pudica_next_probe_time_;
    const int64_t late_us =
        std::max<int64_t>(0, (now - scheduled_probe_time).us());
    auto padding = packet_sender_->GeneratePadding(DataSize::Bytes(50));
    DataSize probe_data = DataSize::Zero();
    int probe_packets = 0;
    for (auto& p : padding) {
      DataSize packet_size = DataSize::Bytes(
          p->payload_size() + p->padding_size());
      if (include_overhead_) {
        packet_size += DataSize::Bytes(p->headers_size()) +
                       transport_overhead_per_packet_;
      }
      PacedPacketInfo pudica_info;
      pudica_info.probe_cluster_id = kPudicaProbeClusterId;
      packet_sender_->SendPacket(std::move(p), pudica_info);
      OnPacketSent(RtpPacketMediaType::kPadding, packet_size, now);
      probe_data += packet_size;
      ++probe_packets;
    }
    // A late wake never collapses several scheduled probes into one burst.
    // A failed padding generation is also retried on a future interval rather
    // than busy-looping at an already-expired timestamp.
    if (probe_packets > 0) {
      --pudica_probes_remaining_;
    }
    pudica_next_probe_time_ = now + pudica_probe_interval_;

    pudica_total_probe_packets_ += probe_packets;
    if (probe_packets > 0 && pudica_total_probe_packets_ % 400 == 0) {
      fprintf(stderr,
              "[PUDICA-PROBE] total=%d interval_us=%lld scheduled_us=%lld actual_us=%lld late_us=%lld packets=%d bytes=%lld\n",
              pudica_total_probe_packets_,
              (long long)pudica_probe_interval_.us(),
              (long long)scheduled_probe_time.us(), (long long)now.us(),
              (long long)late_us, probe_packets,
              (long long)probe_data.bytes());
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
  if (pudica_config_snapshot_.enabled &&
      now < pudica_gap_end_time_) {
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
