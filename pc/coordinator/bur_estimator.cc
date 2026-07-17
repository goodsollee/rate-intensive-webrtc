/*
 *  Copyright 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree.
 */

#include "pc/coordinator/bur_estimator.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace webrtc {

// ============================================================================
// BurDminTracker
// ============================================================================

BurDminTracker::BurDminTracker(int64_t window_ms) : window_ms_(window_ms) {}

void BurDminTracker::AddSample(int64_t rtt_ms, int64_t now_ms) {
  RemoveOldSamples(now_ms);
  samples_.emplace_back(now_ms, rtt_ms);

  // Update D_min
  if (samples_.size() == 1 || rtt_ms < current_dmin_) {
    current_dmin_ = rtt_ms;
  } else {
    // Recompute min if we may have removed the min sample
    current_dmin_ = samples_.front().second;
    for (const auto& sample : samples_) {
      current_dmin_ = std::min(current_dmin_, sample.second);
    }
  }
}

int64_t BurDminTracker::GetDmin() const {
  return current_dmin_ > 0 ? current_dmin_ : 1;  // Avoid division by zero
}

void BurDminTracker::Reset() {
  samples_.clear();
  current_dmin_ = 0;
}

void BurDminTracker::RemoveOldSamples(int64_t now_ms) {
  while (!samples_.empty() && samples_.front().first < now_ms - window_ms_) {
    samples_.pop_front();
  }
}

// ============================================================================
// BurSmoothedCalculator
// ============================================================================

BurSmoothedCalculator::BurSmoothedCalculator(int64_t smoothing_window_ms)
    : smoothing_window_ms_(smoothing_window_ms) {}

void BurSmoothedCalculator::UpdateBur(int64_t rtt_ms, int64_t d_min_ms,
                                       int64_t rate_bps, int64_t now_ms) {
  RemoveOldSamples(now_ms);

  // Calculate L: time to transmit one packet at current rate
  // L = packet_size_bytes * 8 / rate_bps (in seconds)
  // Convert to ms: L_ms = packet_size_bytes * 8 * 1000 / rate_bps
  double L_ms = 0.0;
  if (rate_bps > 0) {
    L_ms = static_cast<double>(kDefaultPacketSizeBytes) * 8.0 * 1000.0 /
           static_cast<double>(rate_bps);
  }

  // BUR = (D_t - D_min) / L
  // Clamp to [0, 1] range
  double bur = 0.0;
  if (L_ms > 0 && d_min_ms > 0) {
    double queuing_delay = static_cast<double>(rtt_ms - d_min_ms);
    bur = std::max(0.0, queuing_delay / L_ms);
    bur = std::min(1.0, bur);  // Cap at 1.0
  }

  last_bur_ = bur;
  bur_samples_.emplace_back(now_ms, bur);
}

double BurSmoothedCalculator::GetSmoothedBur() const {
  if (bur_samples_.empty()) {
    return 0.0;
  }

  double sum = 0.0;
  for (const auto& sample : bur_samples_) {
    sum += sample.second;
  }
  return sum / static_cast<double>(bur_samples_.size());
}

void BurSmoothedCalculator::Reset() {
  bur_samples_.clear();
  last_bur_ = 0.0;
}

void BurSmoothedCalculator::RemoveOldSamples(int64_t now_ms) {
  while (!bur_samples_.empty() &&
         bur_samples_.front().first < now_ms - smoothing_window_ms_) {
    bur_samples_.pop_front();
  }
}

// ============================================================================
// SctpRttCollector
// ============================================================================

SctpRttCollector::SctpRttCollector(double spike_ratio, int64_t history_ms)
    : spike_ratio_(spike_ratio), history_ms_(history_ms) {}

void SctpRttCollector::AddSample(int64_t rtt_ms, int64_t now_ms) {
  // Remove old samples
  while (!samples_.empty() &&
         samples_.front().first < now_ms - history_ms_) {
    samples_.pop_front();
  }

  // Detect spike: current RTT > spike_ratio * average
  int64_t avg = GetAverageRtt();
  if (avg > 0 && rtt_ms > static_cast<int64_t>(spike_ratio_ * avg)) {
    last_spike_time_ms_ = now_ms;
  }

  samples_.emplace_back(now_ms, rtt_ms);
  latest_rtt_ms_ = rtt_ms;
}

bool SctpRttCollector::HasRecentSpike(int64_t now_ms) const {
  return (now_ms - last_spike_time_ms_) < kSpikeCooldownMs;
}

int64_t SctpRttCollector::GetAverageRtt() const {
  if (samples_.empty()) {
    return 0;
  }

  int64_t sum = 0;
  for (const auto& sample : samples_) {
    sum += sample.second;
  }
  return sum / static_cast<int64_t>(samples_.size());
}

void SctpRttCollector::Reset() {
  samples_.clear();
  latest_rtt_ms_ = 0;
  last_spike_time_ms_ = 0;
}

// ============================================================================
// BurRateController
// ============================================================================

BurRateController::BurRateController(const Config& config)
    : config_(config), current_rate_bps_(config.initial_rate_bps) {}

int64_t BurRateController::UpdateRate(double bur, bool rtt_spike,
                                       int64_t now_ms) {
  // RTT spike -> immediate transition to Spike phase
  if (rtt_spike) {
    phase_ = Phase::kSpike;
    current_rate_bps_ = static_cast<int64_t>(current_rate_bps_ * 0.5);  // 50% drop
    current_rate_bps_ = std::max(current_rate_bps_, config_.min_rate_bps);
    spike_end_time_ms_ = now_ms + config_.spike_duration_ms;
    return current_rate_bps_;
  }

  // In Spike phase, wait for cooldown
  if (phase_ == Phase::kSpike) {
    if (now_ms < spike_end_time_ms_) {
      return current_rate_bps_;  // Hold rate
    }
    phase_ = Phase::kDraining;
  }

  // State machine based on BUR
  switch (phase_) {
    case Phase::kMultiplicativeIncrease:
      if (bur > config_.bur_low_threshold) {
        phase_ = Phase::kAdditiveIncrease;
      } else {
        // Fast increase: rate *= MI factor
        current_rate_bps_ = static_cast<int64_t>(
            current_rate_bps_ * config_.mi_factor);
      }
      break;

    case Phase::kAdditiveIncrease:
      if (bur > config_.bur_high_threshold) {
        phase_ = Phase::kMultiplicativeDecrease;
      } else if (bur < config_.bur_low_threshold) {
        phase_ = Phase::kMultiplicativeIncrease;
      } else {
        // Slow increase: rate += AI step
        current_rate_bps_ += config_.ai_step_bps;
      }
      break;

    case Phase::kMultiplicativeDecrease:
      // Fast decrease: rate *= MD factor
      current_rate_bps_ = static_cast<int64_t>(
          current_rate_bps_ * config_.md_factor);
      if (bur < config_.bur_high_threshold) {
        phase_ = Phase::kDraining;
      }
      break;

    case Phase::kDraining:
      // Wait for stabilization
      if (bur < config_.bur_low_threshold) {
        phase_ = Phase::kMultiplicativeIncrease;
      }
      break;

    case Phase::kSpike:
      // Handled above
      break;
  }

  // Apply rate limits
  current_rate_bps_ = std::max(current_rate_bps_, config_.min_rate_bps);
  current_rate_bps_ = std::min(current_rate_bps_, config_.max_rate_bps);

  return current_rate_bps_;
}

const char* BurRateController::PhaseToString(Phase phase) {
  switch (phase) {
    case Phase::kMultiplicativeIncrease:
      return "MI";
    case Phase::kAdditiveIncrease:
      return "AI";
    case Phase::kMultiplicativeDecrease:
      return "MD";
    case Phase::kSpike:
      return "SPIKE";
    case Phase::kDraining:
      return "DRAIN";
  }
  return "UNKNOWN";
}

void BurRateController::Reset() {
  phase_ = Phase::kMultiplicativeIncrease;
  current_rate_bps_ = config_.initial_rate_bps;
  spike_end_time_ms_ = 0;
}

// ============================================================================
// BurEstimator
// ============================================================================

BurEstimator::BurEstimator(const Config& config)
    : dmin_tracker_(config.dmin_window_ms),
      bur_calculator_(config.bur_smoothing_ms),
      rtt_collector_(config.spike_ratio),
      rate_controller_(config.rate_ctrl_config) {}

void BurEstimator::OnSctpRttSample(int64_t rtt_ms, int64_t now_ms) {
  // 1. Update D_min tracker
  dmin_tracker_.AddSample(rtt_ms, now_ms);

  // 2. Update RTT collector (for spike detection)
  rtt_collector_.AddSample(rtt_ms, now_ms);

  // 3. Calculate BUR
  int64_t current_rate = rate_controller_.GetRate();
  int64_t d_min = dmin_tracker_.GetDmin();
  bur_calculator_.UpdateBur(rtt_ms, d_min, current_rate, now_ms);

  // 4. Update rate controller
  double smoothed_bur = bur_calculator_.GetSmoothedBur();
  bool spike = rtt_collector_.HasRecentSpike(now_ms);
  rate_controller_.UpdateRate(smoothed_bur, spike, now_ms);

  last_update_ms_ = now_ms;
}

void BurEstimator::Reset() {
  dmin_tracker_.Reset();
  bur_calculator_.Reset();
  rtt_collector_.Reset();
  rate_controller_.Reset();
  last_update_ms_ = 0;
}

}  // namespace webrtc
