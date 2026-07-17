/*
 *  Copyright 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree.
 */

#ifndef PC_COORDINATOR_BUR_ESTIMATOR_H_
#define PC_COORDINATOR_BUR_ESTIMATOR_H_

#include <cstdint>
#include <deque>
#include <optional>

namespace webrtc {

// BUR (Buffer Utilization Ratio) based rate control for SCTP
// Based on Pudica NSDI'24: BUR = (D_t - D_min) / L
//
// Where:
//   D_t   = Current measured delay (RTT or OWD)
//   D_min = Minimum observed delay (propagation delay estimate)
//   L     = Time to transmit one packet at current rate
//
// BUR ≈ 0: Buffer empty, can increase rate
// BUR ≈ 1: Buffer full, should decrease rate

// Tracks minimum delay (D_min) over a sliding window
class BurDminTracker {
 public:
  explicit BurDminTracker(int64_t window_ms = 10000);

  // Add a new RTT sample
  void AddSample(int64_t rtt_ms, int64_t now_ms);

  // Get current D_min estimate
  int64_t GetDmin() const;

  // Reset tracker
  void Reset();

 private:
  void RemoveOldSamples(int64_t now_ms);

  int64_t window_ms_;
  std::deque<std::pair<int64_t, int64_t>> samples_;  // (time_ms, rtt_ms)
  int64_t current_dmin_ = 0;
};

// Calculates and smooths BUR values
class BurSmoothedCalculator {
 public:
  explicit BurSmoothedCalculator(int64_t smoothing_window_ms = 200);

  // Update BUR with new RTT sample
  // L = packet_size * 8 / rate_bps (time to send one packet)
  void UpdateBur(int64_t rtt_ms, int64_t d_min_ms, int64_t rate_bps,
                 int64_t now_ms);

  // Get smoothed BUR (average over window)
  double GetSmoothedBur() const;

  // Get latest instantaneous BUR
  double GetInstantBur() const { return last_bur_; }

  void Reset();

 private:
  void RemoveOldSamples(int64_t now_ms);

  int64_t smoothing_window_ms_;
  std::deque<std::pair<int64_t, double>> bur_samples_;  // (time_ms, bur)
  double last_bur_ = 0.0;
  static constexpr int64_t kDefaultPacketSizeBytes = 1200;
};

// Collects SCTP RTT samples and detects spikes
class SctpRttCollector {
 public:
  explicit SctpRttCollector(double spike_ratio = 2.0, int64_t history_ms = 500);

  // Add new RTT sample
  void AddSample(int64_t rtt_ms, int64_t now_ms);

  // Check if there was a recent RTT spike
  bool HasRecentSpike(int64_t now_ms) const;

  // Get latest RTT
  int64_t GetLatestRtt() const { return latest_rtt_ms_; }

  // Get average RTT over history window
  int64_t GetAverageRtt() const;

  void Reset();

 private:
  double spike_ratio_;
  int64_t history_ms_;
  std::deque<std::pair<int64_t, int64_t>> samples_;  // (time_ms, rtt_ms)
  int64_t latest_rtt_ms_ = 0;
  int64_t last_spike_time_ms_ = 0;
  static constexpr int64_t kSpikeCooldownMs = 100;
};

// BUR-based rate controller state machine
// Phases: MI (Multiplicative Increase) -> AI (Additive Increase)
//         -> MD (Multiplicative Decrease) -> Draining
class BurRateController {
 public:
  enum class Phase {
    kMultiplicativeIncrease,  // BUR low, buffer empty -> fast increase
    kAdditiveIncrease,        // BUR moderate -> slow increase
    kMultiplicativeDecrease,  // BUR high -> fast decrease
    kSpike,                   // RTT spike detected -> immediate decrease
    kDraining,                // Recovering from congestion
  };

  struct Config {
    int64_t initial_rate_bps;
    int64_t min_rate_bps;
    int64_t max_rate_bps;
    double bur_low_threshold;
    double bur_high_threshold;
    double mi_factor;
    double md_factor;
    int64_t ai_step_bps;
    int64_t spike_duration_ms;

    Config()
        : initial_rate_bps(100'000'000),   // 100 Mbps
          min_rate_bps(1'000'000),         // 1 Mbps
          max_rate_bps(1'000'000'000),     // 1 Gbps
          bur_low_threshold(0.1),
          bur_high_threshold(0.8),
          mi_factor(1.1),
          md_factor(0.8),
          ai_step_bps(10'000'000),         // 10 Mbps
          spike_duration_ms(100) {}
  };

  explicit BurRateController(const Config& config = Config());

  // Update rate based on BUR and spike status
  int64_t UpdateRate(double smoothed_bur, bool rtt_spike, int64_t now_ms);

  // Get current rate
  int64_t GetRate() const { return current_rate_bps_; }

  // Get current phase
  Phase GetPhase() const { return phase_; }

  // Get phase as string (for logging)
  static const char* PhaseToString(Phase phase);

  void Reset();

 private:
  Config config_;
  Phase phase_ = Phase::kMultiplicativeIncrease;
  int64_t current_rate_bps_;
  int64_t spike_end_time_ms_ = 0;
};

// Combined BUR estimator integrating all components
class BurEstimator {
 public:
  struct Config {
    int64_t dmin_window_ms;
    int64_t bur_smoothing_ms;
    double spike_ratio;
    BurRateController::Config rate_ctrl_config;

    Config()
        : dmin_window_ms(10000),
          bur_smoothing_ms(200),
          spike_ratio(2.0),
          rate_ctrl_config() {}
  };

  explicit BurEstimator(const Config& config = Config());

  // Process SCTP RTT sample
  void OnSctpRttSample(int64_t rtt_ms, int64_t now_ms);

  // Get current pacing rate
  int64_t GetPacingRate() const { return rate_controller_.GetRate(); }

  // Get current BUR value (for logging)
  double GetSmoothedBur() const { return bur_calculator_.GetSmoothedBur(); }

  // Get D_min (for logging)
  int64_t GetDmin() const { return dmin_tracker_.GetDmin(); }

  // Get current phase (for logging)
  BurRateController::Phase GetPhase() const {
    return rate_controller_.GetPhase();
  }

  void Reset();

 private:
  BurDminTracker dmin_tracker_;
  BurSmoothedCalculator bur_calculator_;
  SctpRttCollector rtt_collector_;
  BurRateController rate_controller_;
  int64_t last_update_ms_ = 0;
};

}  // namespace webrtc

#endif  // PC_COORDINATOR_BUR_ESTIMATOR_H_
