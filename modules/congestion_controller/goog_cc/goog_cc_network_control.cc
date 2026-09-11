/*
 *  Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/pacing/pacing_controller.h"
#include "modules/congestion_controller/goog_cc/goog_cc_network_control.h"

#include <stdio.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "api/environment/environment.h"
#include "api/field_trials_view.h"
#include "api/transport/bandwidth_usage.h"
#include "api/transport/network_control.h"
#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "logging/rtc_event_log/events/rtc_event_remote_estimate.h"
#include "modules/congestion_controller/goog_cc/acknowledged_bitrate_estimator_interface.h"
#include "modules/congestion_controller/goog_cc/alr_detector.h"
#include "modules/congestion_controller/goog_cc/congestion_window_pushback_controller.h"
#include "modules/congestion_controller/goog_cc/delay_based_bwe.h"
#include "modules/congestion_controller/goog_cc/loss_based_bwe_v2.h"
#include "modules/congestion_controller/goog_cc/probe_bitrate_estimator.h"
#include "modules/congestion_controller/goog_cc/probe_controller.h"
#include "modules/congestion_controller/goog_cc/send_side_bandwidth_estimation.h"
#include "modules/remote_bitrate_estimator/include/bwe_defines.h"
#include "pc/rtp_sctp_coordinator.h"
#include "rtc_base/checks.h"
#include "rtc_base/experiments/field_trial_parser.h"
#include "rtc_base/experiments/rate_control_settings.h"
#include "rtc_base/logging.h"

namespace webrtc {

namespace {
// From RTCPSender video report interval.
constexpr TimeDelta kLossUpdateInterval = TimeDelta::Millis(1000);

// Pacing-rate relative to our target send rate.
// Multiplicative factor that is applied to the target bitrate to calculate
// the number of bytes that can be transmitted per interval.
// Increasing this factor will result in lower delays in cases of bitrate
// overshoots from the encoder.
constexpr float kDefaultPaceMultiplier = 2.5f;

// If the probe result is far below the current throughput estimate
// it's unlikely that the probe is accurate, so we don't want to drop too far.
// However, if we actually are overusing, we want to drop to something slightly
// below the current throughput estimate to drain the network queues.
constexpr double kProbeDropThroughputFraction = 0.85;

// [A31] Remove congestion-window pushback entirely. Unset => byte-identical to
// upstream, which has it ON by default: WebRTC-CongestionWindow is unset in
// this testbed, so rate_control_settings.cc falls back to the built-in
// "QueueSize:350,MinBitrate:30000,DropFrame:true".
//
// This is a SEPARATE knob from KFT_NO_FRAME_DROP on purpose. That flag gates
// five sites (video_stream_encoder.cc 1679/1941/2352/2464 and
// libvpx_vp8_encoder.cc 994); four of them drop frames, but 2352 does
// something else entirely -- it zeroes cwnd_reduce_ratio, i.e. it removes a
// HALVING OF THE ENCODER TARGET, not a frame drop. Measuring the pushback's
// effect through KFT_NO_FRAME_DROP therefore changes five things at once.
//
// Gating construction rather than one consumer disables the mechanism whole:
// with the controller null, UpdatePacingQueue / SetDataWindow /
// UpdateOutstandingData are all skipped, pushback_target_rate stays equal to
// loss_based_target_rate, and cwnd_reduce_ratio stays 0.
bool KftNoCwndPushback() {
  static const bool on = [] {
    const char* e = getenv("KFT_NO_CWND_PUSHBACK");
    return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
  }();
  return on;
}

BandwidthLimitedCause GetBandwidthLimitedCause(LossBasedState loss_based_state,
                                               bool is_rtt_above_limit,
                                               BandwidthUsage bandwidth_usage) {
  if (bandwidth_usage == BandwidthUsage::kBwOverusing ||
      bandwidth_usage == BandwidthUsage::kBwUnderusing) {
    return BandwidthLimitedCause::kDelayBasedLimitedDelayIncreased;
  } else if (is_rtt_above_limit) {
    return BandwidthLimitedCause::kRttBasedBackOffHighRtt;
  }
  switch (loss_based_state) {
    case LossBasedState::kDecreasing:
      // Probes may not be sent in this state.
      return BandwidthLimitedCause::kLossLimitedBwe;
    case webrtc::LossBasedState::kIncreaseUsingPadding:
      // Probes may not be sent in this state.
      return BandwidthLimitedCause::kLossLimitedBwe;
    case LossBasedState::kIncreasing:
      // Probes may be sent in this state.
      return BandwidthLimitedCause::kLossLimitedBweIncreasing;
    case LossBasedState::kDelayBasedEstimate:
      return BandwidthLimitedCause::kDelayBasedLimited;
  }
}

}  // namespace

GoogCcNetworkController::GoogCcNetworkController(NetworkControllerConfig config,
                                                 GoogCcConfig goog_cc_config)
    : env_(config.env),
      packet_feedback_only_(goog_cc_config.feedback_only),
      safe_reset_on_route_change_("Enabled"),
      safe_reset_acknowledged_rate_("ack"),
      use_min_allocatable_as_lower_bound_(
          !env_.field_trials().IsDisabled("WebRTC-Bwe-MinAllocAsLowerBound")),
      ignore_probes_lower_than_network_estimate_(
          !env_.field_trials().IsDisabled(
              "WebRTC-Bwe-IgnoreProbesLowerThanNetworkStateEstimate")),
      limit_probes_lower_than_throughput_estimate_(
          !env_.field_trials().IsDisabled(
              "WebRTC-Bwe-LimitProbesLowerThanThroughputEstimate")),
      rate_control_settings_(env_.field_trials()),
      pace_at_max_of_bwe_and_lower_link_capacity_(env_.field_trials().IsEnabled(
          "WebRTC-Bwe-PaceAtMaxOfBweAndLowerLinkCapacity")),
      limit_pacingfactor_by_upper_link_capacity_estimate_(
          env_.field_trials().IsEnabled(
              "WebRTC-Bwe-LimitPacingFactorByUpperLinkCapacityEstimate")),
      probe_controller_(
          new ProbeController(&env_.field_trials(), &env_.event_log())),
      congestion_window_pushback_controller_(
          (rate_control_settings_.UseCongestionWindowPushback() &&
           !KftNoCwndPushback())
              ? std::make_unique<CongestionWindowPushbackController>(
                    env_.field_trials())
              : nullptr),
      bandwidth_estimation_(
          std::make_unique<SendSideBandwidthEstimation>(&env_.field_trials(),
                                                        &env_.event_log())),
      alr_detector_(std::make_unique<AlrDetector>(&env_.field_trials(),
                                                  &env_.event_log())),
      probe_bitrate_estimator_(new ProbeBitrateEstimator(&env_.event_log())),
      network_estimator_(std::move(goog_cc_config.network_state_estimator)),
      network_state_predictor_(
          std::move(goog_cc_config.network_state_predictor)),
      delay_based_bwe_(new DelayBasedBwe(&env_.field_trials(),
                                         &env_.event_log(),
                                         network_state_predictor_.get())),
      acknowledged_bitrate_estimator_(
          AcknowledgedBitrateEstimatorInterface::Create(&env_.field_trials())),
      initial_config_(config),
      l4s_ce_reaction_enabled_(
          env_.field_trials().IsEnabled("WebRTC-L4S-CeReaction")),
      l4s_ce_g_("g", 0.0625),
      last_loss_based_target_rate_(*config.constraints.starting_rate),
      last_pushback_target_rate_(last_loss_based_target_rate_),
      last_stable_target_rate_(last_loss_based_target_rate_),
      last_loss_base_state_(LossBasedState::kDelayBasedEstimate),
      pacing_factor_(config.stream_based_config.pacing_factor.value_or(
          kDefaultPaceMultiplier)),
      min_total_allocated_bitrate_(
          config.stream_based_config.min_total_allocated_bitrate.value_or(
              DataRate::Zero())),
      max_padding_rate_(config.stream_based_config.max_padding_rate.value_or(
          DataRate::Zero())) {
  RTC_DCHECK(config.constraints.at_time.IsFinite());
  ParseFieldTrial(
      {&safe_reset_on_route_change_, &safe_reset_acknowledged_rate_},
      env_.field_trials().Lookup("WebRTC-Bwe-SafeResetOnRouteChange"));
  // [L4Span L3] Parse CE-brake gain g (default 1/16). Only meaningful when
  // WebRTC-L4S-CeReaction is enabled; otherwise MaybeReactToCe returns early.
  ParseFieldTrial({&l4s_ce_g_},
                  env_.field_trials().Lookup("WebRTC-L4S-CeReaction"));
  // [L4Span L7] Sender-side CE-brake log. The runner sets KFT_L4S_LOG for the
  // sender process only (the receiver's goog_cc would otherwise write a
  // second l4s_sender.csv into the same UNIFIED_CSV_DIR).
  if (l4s_ce_reaction_enabled_) {
    const char* e = std::getenv("KFT_L4S_LOG");
    l4s_log_enabled_ = e != nullptr && *e != '\0' && std::strcmp(e, "0") != 0;
    if (l4s_log_enabled_) {
      const char* dir = std::getenv("UNIFIED_CSV_DIR");
      if (dir && *dir) {
        const std::string path = std::string(dir) + "/l4s_sender.csv";
        l4s_csv_ = std::fopen(path.c_str(), "w");
        if (l4s_csv_) {
          std::fprintf(l4s_csv_,
                       "t_ms,n_ce,n_ect,alpha,rtt_ms,target_mbps,cap_mbps,active\n");
          std::fflush(l4s_csv_);
        }
      }
      std::fprintf(stderr, "[L4S] ON g=%.4f csv=%s\n", l4s_ce_g_.Get(),
                   l4s_csv_ ? "yes" : "no");
    }
  }
  if (delay_based_bwe_)
    delay_based_bwe_->SetMinBitrate(kCongestionControllerMinBitrate);

  // Logging
  logging_folder_ = config.logging_folder;
  delay_based_bwe_->SetLoggingFolder(logging_folder_);
  bandwidth_estimation_->SetLoggingFolder(logging_folder_);
  RTC_LOG(LS_INFO) << "GoogCcNetworkController: Logging to " << logging_folder_.value_or("not set");
}

GoogCcNetworkController::~GoogCcNetworkController() {
  if (l4s_csv_)
    std::fclose(l4s_csv_);
}

NetworkControlUpdate GoogCcNetworkController::OnNetworkAvailability(
    NetworkAvailability msg) {
  NetworkControlUpdate update;
  update.probe_cluster_configs = probe_controller_->OnNetworkAvailability(msg);
  return update;
}

NetworkControlUpdate GoogCcNetworkController::OnNetworkRouteChange(
    NetworkRouteChange msg) {
  if (safe_reset_on_route_change_) {
    std::optional<DataRate> estimated_bitrate;
    if (safe_reset_acknowledged_rate_) {
      estimated_bitrate = acknowledged_bitrate_estimator_->bitrate();
      if (!estimated_bitrate)
        estimated_bitrate = acknowledged_bitrate_estimator_->PeekRate();
    } else {
      estimated_bitrate = bandwidth_estimation_->target_rate();
    }
    if (estimated_bitrate) {
      if (msg.constraints.starting_rate) {
        msg.constraints.starting_rate =
            std::min(*msg.constraints.starting_rate, *estimated_bitrate);
      } else {
        msg.constraints.starting_rate = estimated_bitrate;
      }
    }
  }

  acknowledged_bitrate_estimator_ =
      AcknowledgedBitrateEstimatorInterface::Create(&env_.field_trials());
  probe_bitrate_estimator_.reset(new ProbeBitrateEstimator(&env_.event_log()));
  if (network_estimator_)
    network_estimator_->OnRouteChange(msg);
  delay_based_bwe_.reset(new DelayBasedBwe(
      &env_.field_trials(), &env_.event_log(), network_state_predictor_.get()));
  bandwidth_estimation_->OnRouteChange();
  probe_controller_->Reset(msg.at_time);
  NetworkControlUpdate update;
  update.probe_cluster_configs = ResetConstraints(msg.constraints);
  MaybeTriggerOnNetworkChanged(&update, msg.at_time);
  return update;
}

NetworkControlUpdate GoogCcNetworkController::OnProcessInterval(
    ProcessInterval msg) {
  NetworkControlUpdate update;
  if (initial_config_) {
    update.probe_cluster_configs =
        ResetConstraints(initial_config_->constraints);
    update.pacer_config = GetPacingRates(msg.at_time);

    if (initial_config_->stream_based_config.requests_alr_probing) {
      probe_controller_->EnablePeriodicAlrProbing(
          *initial_config_->stream_based_config.requests_alr_probing);
    }
    if (initial_config_->stream_based_config.enable_repeated_initial_probing) {
      probe_controller_->EnableRepeatedInitialProbing(
          *initial_config_->stream_based_config
               .enable_repeated_initial_probing);
    }
    std::optional<DataRate> total_bitrate =
        initial_config_->stream_based_config.max_total_allocated_bitrate;
    if (total_bitrate) {
      auto probes = probe_controller_->OnMaxTotalAllocatedBitrate(
          *total_bitrate, msg.at_time);
      update.probe_cluster_configs.insert(update.probe_cluster_configs.end(),
                                          probes.begin(), probes.end());
    }
    initial_config_.reset();
  }
  if (congestion_window_pushback_controller_ && msg.pacer_queue) {
    congestion_window_pushback_controller_->UpdatePacingQueue(
        msg.pacer_queue->bytes());
  }
  bandwidth_estimation_->UpdateEstimate(msg.at_time);
  std::optional<int64_t> start_time_ms =
      alr_detector_->GetApplicationLimitedRegionStartTime();
  probe_controller_->SetAlrStartTimeMs(start_time_ms);

  auto probes = probe_controller_->Process(msg.at_time);
  update.probe_cluster_configs.insert(update.probe_cluster_configs.end(),
                                      probes.begin(), probes.end());

  // [A31] KftNoCwndPushback() suppresses the WINDOW too, not just the pushback
  // controller. Leaving it computed would be worse than upstream: with the
  // controller null the branch below routes current_data_window_ into
  // update.congestion_window instead, i.e. a hard transport-side cap on
  // outstanding data replaces a soft encoder-side rate reduction. The knob is
  // meant to remove the mechanism, not relocate it.
  if (rate_control_settings_.UseCongestionWindow() &&
      !feedback_max_rtts_.empty() && !KftNoCwndPushback()) {
    UpdateCongestionWindowSize();
  }
  if (congestion_window_pushback_controller_ && current_data_window_) {
    congestion_window_pushback_controller_->SetDataWindow(
        *current_data_window_);
  } else {
    update.congestion_window = current_data_window_;
  }
  // [A28e] Timer-driven evaluation of Pudica's §4.3 next-delay fallback.
  //
  // The rule is measured entirely on the SENDER's clock precisely so it does
  // not need the failed link to carry anything -- but until now its only call
  // site was inside DelayBasedBwe::MaybeUpdateEstimate(), reachable only from
  // OnTransportPacketsFeedback(). In a full blackout no packet reaches the UE,
  // so no TWCC comes back, so the fallback never runs and the target stays
  // frozen at its pre-dip value. That is the exact opposite of what the signal
  // exists for.
  //
  // Measured on run 1789022666 (q1s_attcampus_5g_122s): next_delay.csv has NO
  // ROWS at all for every one of the 7 zero-capacity seconds -- the function
  // was never called. Feedback stopped for 1.2-2.2 s at t=17.9/23.9/32.9/
  // 52.9/56.9/59.9 s while the target sat at 48.94 and 49.35 Mbps into a link
  // delivering nothing.
  //
  // OnProcessInterval is the periodic path (25 ms) and keeps running with no
  // feedback, so evaluating here makes the fallback reachable. Applied the
  // same way the feedback path applies it, via UpdateDelayBasedEstimate()
  // followed by the MaybeTriggerOnNetworkChanged() already below.
  if (RtpSctpCoordinator::IsPudicaMode()) {
    int64_t pud_fallback = RtpSctpCoordinator::GetPudicaTimerFallback();
    if (pud_fallback > 0) {
      bandwidth_estimation_->UpdateDelayBasedEstimate(
          msg.at_time, DataRate::BitsPerSec(pud_fallback));
    }
  }
  MaybeTriggerOnNetworkChanged(&update, msg.at_time);
  return update;
}

NetworkControlUpdate GoogCcNetworkController::OnRemoteBitrateReport(
    RemoteBitrateReport msg) {
  if (packet_feedback_only_) {
    RTC_LOG(LS_ERROR) << "Received REMB for packet feedback only GoogCC";
    return NetworkControlUpdate();
  }
  bandwidth_estimation_->UpdateReceiverEstimate(msg.receive_time,
                                                msg.bandwidth);
  return NetworkControlUpdate();
}

NetworkControlUpdate GoogCcNetworkController::OnRoundTripTimeUpdate(
    RoundTripTimeUpdate msg) {
  if (packet_feedback_only_ || msg.smoothed)
    return NetworkControlUpdate();
  RTC_DCHECK(!msg.round_trip_time.IsZero());
  if (delay_based_bwe_)
    delay_based_bwe_->OnRttUpdate(msg.round_trip_time);
  bandwidth_estimation_->UpdateRtt(msg.round_trip_time, msg.receive_time);
  return NetworkControlUpdate();
}

NetworkControlUpdate GoogCcNetworkController::OnSentPacket(
    SentPacket sent_packet) {
  alr_detector_->OnBytesSent(sent_packet.size.bytes(),
                             sent_packet.send_time.ms());
  acknowledged_bitrate_estimator_->SetAlr(
      alr_detector_->GetApplicationLimitedRegionStartTime().has_value());

  if (!first_packet_sent_) {
    first_packet_sent_ = true;
    // Initialize feedback time to send time to allow estimation of RTT until
    // first feedback is received.
    bandwidth_estimation_->UpdatePropagationRtt(sent_packet.send_time,
                                                TimeDelta::Zero());
  }
  bandwidth_estimation_->OnSentPacket(sent_packet);

  if (congestion_window_pushback_controller_) {
    congestion_window_pushback_controller_->UpdateOutstandingData(
        sent_packet.data_in_flight.bytes());
    NetworkControlUpdate update;
    MaybeTriggerOnNetworkChanged(&update, sent_packet.send_time);
    return update;
  } else {
    return NetworkControlUpdate();
  }
}

NetworkControlUpdate GoogCcNetworkController::OnReceivedPacket(
    ReceivedPacket /* received_packet */) {
  return NetworkControlUpdate();
}

NetworkControlUpdate GoogCcNetworkController::OnStreamsConfig(
    StreamsConfig msg) {
  NetworkControlUpdate update;
  if (msg.requests_alr_probing) {
    probe_controller_->EnablePeriodicAlrProbing(*msg.requests_alr_probing);
  }
  if (msg.max_total_allocated_bitrate) {
    update.probe_cluster_configs =
        probe_controller_->OnMaxTotalAllocatedBitrate(
            *msg.max_total_allocated_bitrate, msg.at_time);
  }

  bool pacing_changed = false;
  if (msg.pacing_factor && *msg.pacing_factor != pacing_factor_) {
    pacing_factor_ = *msg.pacing_factor;
    pacing_changed = true;
  }
  if (msg.min_total_allocated_bitrate &&
      *msg.min_total_allocated_bitrate != min_total_allocated_bitrate_) {
    min_total_allocated_bitrate_ = *msg.min_total_allocated_bitrate;
    pacing_changed = true;

    if (use_min_allocatable_as_lower_bound_) {
      ClampConstraints();
      delay_based_bwe_->SetMinBitrate(min_data_rate_);
      bandwidth_estimation_->SetMinMaxBitrate(min_data_rate_, max_data_rate_);
    }
  }
  if (msg.max_padding_rate && *msg.max_padding_rate != max_padding_rate_) {
    max_padding_rate_ = *msg.max_padding_rate;
    pacing_changed = true;
  }

  if (pacing_changed)
    update.pacer_config = GetPacingRates(msg.at_time);
  return update;
}

NetworkControlUpdate GoogCcNetworkController::OnTargetRateConstraints(
    TargetRateConstraints constraints) {
  NetworkControlUpdate update;
  update.probe_cluster_configs = ResetConstraints(constraints);
  MaybeTriggerOnNetworkChanged(&update, constraints.at_time);
  return update;
}

void GoogCcNetworkController::ClampConstraints() {
  // TODO(holmer): We should make sure the default bitrates are set to 10 kbps,
  // and that we don't try to set the min bitrate to 0 from any applications.
  // The congestion controller should allow a min bitrate of 0.
  min_data_rate_ = std::max(min_target_rate_, kCongestionControllerMinBitrate);
  if (use_min_allocatable_as_lower_bound_) {
    min_data_rate_ = std::max(min_data_rate_, min_total_allocated_bitrate_);
  }
  if (max_data_rate_ < min_data_rate_) {
    RTC_LOG(LS_WARNING) << "max bitrate smaller than min bitrate";
    max_data_rate_ = min_data_rate_;
  }
  if (starting_rate_ && starting_rate_ < min_data_rate_) {
    RTC_LOG(LS_WARNING) << "start bitrate smaller than min bitrate";
    starting_rate_ = min_data_rate_;
  }
}

std::vector<ProbeClusterConfig> GoogCcNetworkController::ResetConstraints(
    TargetRateConstraints new_constraints) {
  min_target_rate_ = new_constraints.min_data_rate.value_or(DataRate::Zero());
  max_data_rate_ =
      new_constraints.max_data_rate.value_or(DataRate::PlusInfinity());
  starting_rate_ = new_constraints.starting_rate;
  ClampConstraints();

  bandwidth_estimation_->SetBitrates(starting_rate_, min_data_rate_,
                                     max_data_rate_, new_constraints.at_time);

  if (starting_rate_)
    delay_based_bwe_->SetStartBitrate(*starting_rate_);
  delay_based_bwe_->SetMinBitrate(min_data_rate_);

  return probe_controller_->SetBitrates(
      min_data_rate_, starting_rate_.value_or(DataRate::Zero()), max_data_rate_,
      new_constraints.at_time);
}

NetworkControlUpdate GoogCcNetworkController::OnTransportLossReport(
    TransportLossReport msg) {
  if (packet_feedback_only_)
    return NetworkControlUpdate();
  int64_t total_packets_delta =
      msg.packets_received_delta + msg.packets_lost_delta;
  bandwidth_estimation_->UpdatePacketsLost(
      msg.packets_lost_delta, total_packets_delta, msg.receive_time);
  return NetworkControlUpdate();
}

void GoogCcNetworkController::UpdateCongestionWindowSize() {
  TimeDelta min_feedback_max_rtt = TimeDelta::Millis(
      *std::min_element(feedback_max_rtts_.begin(), feedback_max_rtts_.end()));

  const DataSize kMinCwnd = DataSize::Bytes(2 * 1500);
  TimeDelta time_window =
      min_feedback_max_rtt +
      TimeDelta::Millis(
          rate_control_settings_.GetCongestionWindowAdditionalTimeMs());

  DataSize data_window = last_loss_based_target_rate_ * time_window;
  if (current_data_window_) {
    data_window =
        std::max(kMinCwnd, (data_window + current_data_window_.value()) / 2);
  } else {
    data_window = std::max(kMinCwnd, data_window);
  }
  current_data_window_ = data_window;
}

NetworkControlUpdate GoogCcNetworkController::OnTransportPacketsFeedback(
    TransportPacketsFeedback report) {
  if (report.packet_feedbacks.empty()) {
    // TODO(bugs.webrtc.org/10125): Design a better mechanism to safe-guard
    // against building very large network queues.
    return NetworkControlUpdate();
  }

  if (congestion_window_pushback_controller_) {
    congestion_window_pushback_controller_->UpdateOutstandingData(
        report.data_in_flight.bytes());
  }
  TimeDelta max_feedback_rtt = TimeDelta::MinusInfinity();
  TimeDelta min_propagation_rtt = TimeDelta::PlusInfinity();
  Timestamp max_recv_time = Timestamp::MinusInfinity();

  std::vector<PacketResult> feedbacks = report.ReceivedWithSendInfo();
  for (const auto& feedback : feedbacks)
    max_recv_time = std::max(max_recv_time, feedback.receive_time);

  for (const auto& feedback : feedbacks) {
    TimeDelta feedback_rtt =
        report.feedback_time - feedback.sent_packet.send_time;
    TimeDelta min_pending_time = max_recv_time - feedback.receive_time;
    TimeDelta propagation_rtt = feedback_rtt - min_pending_time;
    max_feedback_rtt = std::max(max_feedback_rtt, feedback_rtt);
    min_propagation_rtt = std::min(min_propagation_rtt, propagation_rtt);
  }

  if (max_feedback_rtt.IsFinite()) {
    feedback_max_rtts_.push_back(max_feedback_rtt.ms());
    const size_t kMaxFeedbackRttWindow = 32;
    if (feedback_max_rtts_.size() > kMaxFeedbackRttWindow)
      feedback_max_rtts_.pop_front();
    // TODO(srte): Use time since last unacknowledged packet.
    bandwidth_estimation_->UpdatePropagationRtt(report.feedback_time,
                                                min_propagation_rtt);
  }
  // [L4Span L3, J-137/J-138] React to CE marks before the delay/loss BWE
  // updates below, so the CE cap (merged into GetUpperLimit) clamps this
  // report's target update. No-op unless WebRTC-L4S-CeReaction is enabled.
  MaybeReactToCe(feedbacks, max_feedback_rtt, report.feedback_time);
  if (packet_feedback_only_) {
    if (!feedback_max_rtts_.empty()) {
      int64_t sum_rtt_ms =
          std::accumulate(feedback_max_rtts_.begin(), feedback_max_rtts_.end(),
                          static_cast<int64_t>(0));
      int64_t mean_rtt_ms = sum_rtt_ms / feedback_max_rtts_.size();
      if (delay_based_bwe_)
        delay_based_bwe_->OnRttUpdate(TimeDelta::Millis(mean_rtt_ms));
    }

    TimeDelta feedback_min_rtt = TimeDelta::PlusInfinity();
    for (const auto& packet_feedback : feedbacks) {
      TimeDelta pending_time = max_recv_time - packet_feedback.receive_time;
      TimeDelta rtt = report.feedback_time -
                      packet_feedback.sent_packet.send_time - pending_time;
      // Value used for predicting NACK round trip time in FEC controller.
      feedback_min_rtt = std::min(rtt, feedback_min_rtt);
    }
    if (feedback_min_rtt.IsFinite()) {
      bandwidth_estimation_->UpdateRtt(feedback_min_rtt, report.feedback_time);
    }

    expected_packets_since_last_loss_update_ +=
        report.PacketsWithFeedback().size();
    for (const auto& packet_feedback : report.PacketsWithFeedback()) {
      if (!packet_feedback.IsReceived())
        lost_packets_since_last_loss_update_ += 1;
    }
    if (report.feedback_time > next_loss_update_) {
      next_loss_update_ = report.feedback_time + kLossUpdateInterval;
      bandwidth_estimation_->UpdatePacketsLost(
          lost_packets_since_last_loss_update_,
          expected_packets_since_last_loss_update_, report.feedback_time);
      expected_packets_since_last_loss_update_ = 0;
      lost_packets_since_last_loss_update_ = 0;
    }
  }
  std::optional<int64_t> alr_start_time =
      alr_detector_->GetApplicationLimitedRegionStartTime();

  if (previously_in_alr_ && !alr_start_time.has_value()) {
    int64_t now_ms = report.feedback_time.ms();
    acknowledged_bitrate_estimator_->SetAlrEndedTime(report.feedback_time);
    probe_controller_->SetAlrEndedTimeMs(now_ms);
  }
  previously_in_alr_ = alr_start_time.has_value();
  acknowledged_bitrate_estimator_->IncomingPacketFeedbackVector(
      report.SortedByReceiveTime());
  auto acknowledged_bitrate = acknowledged_bitrate_estimator_->bitrate();
  bandwidth_estimation_->SetAcknowledgedRate(acknowledged_bitrate,
                                             report.feedback_time);
  for (const auto& feedback : report.SortedByReceiveTime()) {
    // [A22] Pudica's Eq.5 probes carry kPudicaProbeClusterId (-100) purely as a
    // TAG so delay_based_bwe.cc can route them to the coordinator; they are NOT
    // GoogCC bandwidth probes. pacing_controller.h claimed "GCC ProbeController
    // only processes probe_cluster_id >= 0, so -100 is ignored" -- but the test
    // here is != kNotAProbe (-1), so -100 fell straight through into
    // ProbeBitrateEstimator, which immediately fails
    //   RTC_CHECK(pacing_info.probe_cluster_min_probes > 0)   (-1 vs 0)
    // because pudica_info sets only the cluster id. PUDICA_PROBING=1 therefore
    // aborted the sender ~2.7 s in, on every build, which is why probe_count
    // was 0 in every run of this series.
    //
    // Excluded rather than given a min_probes value on purpose: these are
    // 50-byte padding bursts placed in the inter-frame gap to measure LINK
    // OCCUPANCY, and letting ProbeBitrateEstimator read them as a probe cluster
    // would feed GoogCC a bandwidth estimate derived from 200 bytes.
    if (feedback.sent_packet.pacing_info.probe_cluster_id !=
            PacedPacketInfo::kNotAProbe &&
        feedback.sent_packet.pacing_info.probe_cluster_id !=
            PacingController::kPudicaProbeClusterId) {
      probe_bitrate_estimator_->HandleProbeAndEstimateBitrate(feedback);
    }
  }

  if (network_estimator_) {
    network_estimator_->OnTransportPacketsFeedback(report);
    SetNetworkStateEstimate(network_estimator_->GetCurrentEstimate());
  }
  std::optional<DataRate> probe_bitrate =
      probe_bitrate_estimator_->FetchAndResetLastEstimatedBitrate();
  if (ignore_probes_lower_than_network_estimate_ && probe_bitrate &&
      estimate_ && *probe_bitrate < delay_based_bwe_->last_estimate() &&
      *probe_bitrate < estimate_->link_capacity_lower) {
    probe_bitrate.reset();
  }
  if (limit_probes_lower_than_throughput_estimate_ && probe_bitrate &&
      acknowledged_bitrate) {
    // Limit the backoff to something slightly below the acknowledged
    // bitrate. ("Slightly below" because we want to drain the queues
    // if we are actually overusing.)
    // The acknowledged bitrate shouldn't normally be higher than the delay
    // based estimate, but it could happen e.g. due to packet bursts or
    // encoder overshoot. We use std::min to ensure that a probe result
    // below the current BWE never causes an increase.
    DataRate limit =
        std::min(delay_based_bwe_->last_estimate(),
                 *acknowledged_bitrate * kProbeDropThroughputFraction);
    probe_bitrate = std::max(*probe_bitrate, limit);
  }

  NetworkControlUpdate update;
  bool recovered_from_overuse = false;

  DelayBasedBwe::Result result;
  result = delay_based_bwe_->IncomingPacketFeedbackVector(
      report, acknowledged_bitrate, probe_bitrate, estimate_,
      alr_start_time.has_value());

  if (result.updated) {
    if (result.probe) {
      bandwidth_estimation_->SetSendBitrate(result.target_bitrate,
                                            report.feedback_time);
    }
    // Since SetSendBitrate now resets the delay-based estimate, we have to
    // call UpdateDelayBasedEstimate after SetSendBitrate.
    bandwidth_estimation_->UpdateDelayBasedEstimate(report.feedback_time,
                                                    result.target_bitrate);
  }
  bandwidth_estimation_->UpdateLossBasedEstimator(
      report, result.delay_detector_state, probe_bitrate,
      alr_start_time.has_value());
  if (result.updated) {
    // Update the estimate in the ProbeController, in case we want to probe.
    MaybeTriggerOnNetworkChanged(&update, report.feedback_time);
  }

  recovered_from_overuse = result.recovered_from_overuse;

  if (recovered_from_overuse) {
    probe_controller_->SetAlrStartTimeMs(alr_start_time);
    auto probes = probe_controller_->RequestProbe(report.feedback_time);
    update.probe_cluster_configs.insert(update.probe_cluster_configs.end(),
                                        probes.begin(), probes.end());
  }

  // No valid RTT could be because send-side BWE isn't used, in which case
  // we don't try to limit the outstanding packets.
  if (rate_control_settings_.UseCongestionWindow() &&
      max_feedback_rtt.IsFinite() && !KftNoCwndPushback()) {   // [A31]
    UpdateCongestionWindowSize();
  }
  if (congestion_window_pushback_controller_ && current_data_window_) {
    congestion_window_pushback_controller_->SetDataWindow(
        *current_data_window_);
  } else {
    update.congestion_window = current_data_window_;
  }

  return update;
}

NetworkControlUpdate GoogCcNetworkController::OnNetworkStateEstimate(
    NetworkStateEstimate msg) {
  if (!network_estimator_) {
    SetNetworkStateEstimate(msg);
  }
  return NetworkControlUpdate();
}

void GoogCcNetworkController::SetNetworkStateEstimate(
    std::optional<NetworkStateEstimate> estimate) {
  auto prev_estimate = estimate_;
  estimate_ = estimate;
  if (estimate_ && (!prev_estimate ||
                    estimate_->update_time != prev_estimate->update_time)) {
    env_.event_log().Log(std::make_unique<RtcEventRemoteEstimate>(
        estimate_->link_capacity_lower, estimate_->link_capacity_upper));
    probe_controller_->SetNetworkStateEstimate(*estimate_);
  }
}

NetworkControlUpdate GoogCcNetworkController::GetNetworkState(
    Timestamp at_time) const {
  NetworkControlUpdate update;
  update.target_rate = TargetTransferRate();
  update.target_rate->network_estimate.at_time = at_time;
  update.target_rate->network_estimate.loss_rate_ratio =
      last_estimated_fraction_loss_.value_or(0) / 255.0;
  update.target_rate->network_estimate.round_trip_time =
      last_estimated_round_trip_time_;
  update.target_rate->network_estimate.bwe_period =
      delay_based_bwe_->GetExpectedBwePeriod();

  update.target_rate->at_time = at_time;
  update.target_rate->target_rate = last_pushback_target_rate_;
  update.target_rate->stable_target_rate =
      bandwidth_estimation_->GetEstimatedLinkCapacity();
  update.pacer_config = GetPacingRates(at_time);
  update.congestion_window = current_data_window_;
  return update;
}

void GoogCcNetworkController::MaybeTriggerOnNetworkChanged(
    NetworkControlUpdate* update,
    Timestamp at_time) {
  uint8_t fraction_loss = bandwidth_estimation_->fraction_loss();
  TimeDelta round_trip_time = bandwidth_estimation_->round_trip_time();
  DataRate loss_based_target_rate = bandwidth_estimation_->target_rate();
  LossBasedState loss_based_state = bandwidth_estimation_->loss_based_state();
  DataRate pushback_target_rate = loss_based_target_rate;

  double cwnd_reduce_ratio = 0.0;
  if (congestion_window_pushback_controller_) {
    int64_t pushback_rate =
        congestion_window_pushback_controller_->UpdateTargetBitrate(
            loss_based_target_rate.bps());
    pushback_rate = std::max<int64_t>(bandwidth_estimation_->GetMinBitrate(),
                                      pushback_rate);
    pushback_target_rate = DataRate::BitsPerSec(pushback_rate);
    if (rate_control_settings_.UseCongestionWindowDropFrameOnly()) {
      cwnd_reduce_ratio = static_cast<double>(loss_based_target_rate.bps() -
                                              pushback_target_rate.bps()) /
                          loss_based_target_rate.bps();
    }
  }
  DataRate stable_target_rate =
      bandwidth_estimation_->GetEstimatedLinkCapacity();
  stable_target_rate = std::min(stable_target_rate, pushback_target_rate);

  if ((loss_based_target_rate != last_loss_based_target_rate_) ||
      (loss_based_state != last_loss_base_state_) ||
      (fraction_loss != last_estimated_fraction_loss_) ||
      (round_trip_time != last_estimated_round_trip_time_) ||
      (pushback_target_rate != last_pushback_target_rate_) ||
      (stable_target_rate != last_stable_target_rate_)) {
    last_loss_based_target_rate_ = loss_based_target_rate;
    last_pushback_target_rate_ = pushback_target_rate;
    last_estimated_fraction_loss_ = fraction_loss;
    last_estimated_round_trip_time_ = round_trip_time;
    last_stable_target_rate_ = stable_target_rate;
    last_loss_base_state_ = loss_based_state;

    alr_detector_->SetEstimatedBitrate(loss_based_target_rate.bps());

    TimeDelta bwe_period = delay_based_bwe_->GetExpectedBwePeriod();

    TargetTransferRate target_rate_msg;
    target_rate_msg.at_time = at_time;
    if (rate_control_settings_.UseCongestionWindowDropFrameOnly()) {
      target_rate_msg.target_rate = loss_based_target_rate;
      target_rate_msg.cwnd_reduce_ratio = cwnd_reduce_ratio;
    } else {
      target_rate_msg.target_rate = pushback_target_rate;
    }
    target_rate_msg.stable_target_rate = stable_target_rate;
    target_rate_msg.network_estimate.at_time = at_time;
    target_rate_msg.network_estimate.round_trip_time = round_trip_time;
    target_rate_msg.network_estimate.loss_rate_ratio = fraction_loss / 255.0f;
    target_rate_msg.network_estimate.bwe_period = bwe_period;
    // [MAE] Ride the same message the encoder already receives. Set
    // unconditionally: it costs one double, and gating it here would make the
    // A/B depend on two things at once. The encoder decides whether to use it.
    target_rate_msg.is_overused_for_encoder = delay_based_bwe_->aggressive_state();

    update->target_rate = target_rate_msg;

    auto probes = probe_controller_->SetEstimatedBitrate(
        loss_based_target_rate,
        GetBandwidthLimitedCause(bandwidth_estimation_->loss_based_state(),
                                 bandwidth_estimation_->IsRttAboveLimit(),
                                 delay_based_bwe_->last_state()),
        at_time);
    update->probe_cluster_configs.insert(update->probe_cluster_configs.end(),
                                         probes.begin(), probes.end());
    update->pacer_config = GetPacingRates(at_time);
    RTC_LOG(LS_VERBOSE) << "bwe " << at_time.ms() << " pushback_target_bps="
                        << last_pushback_target_rate_.bps()
                        << " estimate_bps=" << loss_based_target_rate.bps();
  }
}

void GoogCcNetworkController::MaybeReactToCe(
    const std::vector<PacketResult>& feedbacks,
    TimeDelta max_feedback_rtt,
    Timestamp at_time) {
  if (!l4s_ce_reaction_enabled_)
    return;  // Gate: byte-identical target series when the trial is absent.

  // Count CE among ECN-Capable-Transport packets only. TWCC-derived feedback
  // carries ecn == kNotEct, so a pure-TWCC report contributes 0 to both
  // counters and can never move the brake (INVARIANT).
  int n_ce = 0;
  int n_ect = 0;
  for (const auto& fb : feedbacks) {
    if (fb.ecn == EcnMarking::kCe) {
      ++n_ce;
      ++n_ect;
    } else if (fb.ecn == EcnMarking::kEct1 || fb.ecn == EcnMarking::kEct0) {
      ++n_ect;
    }
  }

  // Per-report EWMA of the CE fraction (Prague/DCTCP alpha, g = 1/16 default).
  if (n_ect > 0) {
    double batch_ce_frac = static_cast<double>(n_ce) / n_ect;
    double g = l4s_ce_g_.Get();
    ce_alpha_ = (1.0 - g) * ce_alpha_ + g * batch_ce_frac;
  }

  // RTT window length = EWMA of max_feedback_rtt (J-137).
  if (max_feedback_rtt.IsFinite() && max_feedback_rtt > TimeDelta::Zero()) {
    double rtt_ms = max_feedback_rtt.ms<double>();
    ce_rtt_ewma_ms_ =
        ce_rtt_ewma_ms_ < 0.0 ? rtt_ms : 0.9 * ce_rtt_ewma_ms_ + 0.1 * rtt_ms;
  }

  ce_window_ce_count_ += n_ce;
  ce_window_ect_count_ += n_ect;
  l4s_tot_ect_ += n_ect;
  l4s_tot_ce_ += n_ce;
  if (!ce_window_start_.IsFinite()) {
    ce_window_start_ = at_time;
    l4s_log_t0_ = at_time;
    l4s_stat_at_ = at_time;
    return;
  }
  // [L4Span L7] 1 s heartbeat: proves the ECN echo path (n_ect > 0 iff the
  // receiver reads ECN and CCFB carries it) even while nothing is marked.
  if (l4s_log_enabled_ && at_time - l4s_stat_at_ >= TimeDelta::Seconds(1)) {
    l4s_stat_at_ = at_time;
    std::fprintf(stderr,
                 "[L4S] stat t=%.0f ect_total=%lld ce_total=%lld alpha=%.4f "
                 "rtt_ms=%.1f active=%d\n",
                 (at_time - l4s_log_t0_).ms<double>(),
                 static_cast<long long>(l4s_tot_ect_),
                 static_cast<long long>(l4s_tot_ce_), ce_alpha_,
                 ce_rtt_ewma_ms_, ce_cap_active_ ? 1 : 0);
  }
  if (ce_rtt_ewma_ms_ <= 0.0)
    return;  // No usable RTT yet; wait for a finite feedback RTT.
  if ((at_time - ce_window_start_).ms<double>() < ce_rtt_ewma_ms_)
    return;  // Still inside the current RTT window: at most one reaction/RTT.

  // Window boundary crossed.
  const DataRate target = bandwidth_estimation_->target_rate();
  DataRate cap = DataRate::PlusInfinity();
  const bool was_active = ce_cap_active_;
  if (ce_window_ce_count_ > 0) {
    // Multiplicative decrease once per RTT that saw CE (paper §2).
    cap = target * (1.0 - ce_alpha_ / 2.0);
    bandwidth_estimation_->SetCeLimit(cap, at_time);
    ce_cap_active_ = true;
  } else if (ce_cap_active_) {
    // A CE-free window: release the cap so additive increase resumes.
    bandwidth_estimation_->SetCeLimit(DataRate::PlusInfinity(), at_time);
    ce_cap_active_ = false;
  }
  // [L4Span L7] One row per window that saw CE, applied a cap, or released
  // one -- quiet (no row) while the link is uncongested.
  if (l4s_log_enabled_ && (ce_window_ce_count_ > 0 || was_active)) {
    const double t_ms = (at_time - l4s_log_t0_).ms<double>();
    const double cap_mbps = cap.IsFinite() ? cap.bps<double>() / 1e6 : -1.0;
    std::fprintf(stderr,
                 "[L4S] t=%.0f n_ce=%d n_ect=%d alpha=%.4f rtt_ms=%.1f "
                 "target_mbps=%.3f cap_mbps=%.3f active=%d\n",
                 t_ms, ce_window_ce_count_, ce_window_ect_count_, ce_alpha_,
                 ce_rtt_ewma_ms_, target.bps<double>() / 1e6, cap_mbps,
                 ce_cap_active_ ? 1 : 0);
    if (l4s_csv_) {
      std::fprintf(l4s_csv_, "%.0f,%d,%d,%.5f,%.2f,%.4f,%.4f,%d\n", t_ms,
                   ce_window_ce_count_, ce_window_ect_count_, ce_alpha_,
                   ce_rtt_ewma_ms_, target.bps<double>() / 1e6, cap_mbps,
                   ce_cap_active_ ? 1 : 0);
      std::fflush(l4s_csv_);
    }
  }
  ce_window_start_ = at_time;
  ce_window_ce_count_ = 0;
  ce_window_ect_count_ = 0;
}

PacerConfig GoogCcNetworkController::GetPacingRates(Timestamp at_time) const {
  // Pacing rate is based on target rate before congestion window pushback,
  // because we don't want to build queues in the pacer when pushback occurs.
  DataRate pacing_rate = DataRate::Zero();
  if (pace_at_max_of_bwe_and_lower_link_capacity_ && estimate_ &&
      !bandwidth_estimation_->PaceAtLossBasedEstimate()) {
    pacing_rate =
        std::max({min_total_allocated_bitrate_, estimate_->link_capacity_lower,
                  last_loss_based_target_rate_}) *
        pacing_factor_;
  } else {
    pacing_rate =
        std::max(min_total_allocated_bitrate_, last_loss_based_target_rate_) *
        pacing_factor_;
  }
  if (limit_pacingfactor_by_upper_link_capacity_estimate_ && estimate_ &&
      estimate_->link_capacity_upper.IsFinite() &&
      pacing_rate > estimate_->link_capacity_upper) {
    pacing_rate =
        std::max({estimate_->link_capacity_upper, min_total_allocated_bitrate_,
                  last_loss_based_target_rate_});
  }

  DataRate padding_rate =
      (last_loss_base_state_ == LossBasedState::kIncreaseUsingPadding)
          ? std::max(max_padding_rate_, last_loss_based_target_rate_)
          : max_padding_rate_;
  padding_rate = std::min(padding_rate, last_pushback_target_rate_);
  PacerConfig msg;
  msg.at_time = at_time;
  msg.time_window = TimeDelta::Seconds(1);
  msg.data_window = pacing_rate * msg.time_window;
  msg.pad_window = padding_rate * msg.time_window;
  return msg;
}

}  // namespace webrtc
