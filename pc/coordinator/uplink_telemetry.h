/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_COORDINATOR_UPLINK_TELEMETRY_H_
#define PC_COORDINATOR_UPLINK_TELEMETRY_H_

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <numeric>
#include <vector>

#include "rtc_base/logging.h"
#include "rtc_base/synchronization/mutex.h"

namespace webrtc {
namespace coordinator {

// UplinkTelemetry - Measures incoming SCTP throughput.
// Tracks bytes received via OnDataReceived() callbacks and computes:
// - R_up: EWMA of uplink throughput (bps)
// - sigma_R: Standard deviation of rate samples

class UplinkTelemetry {
 public:
  struct Config {
    int64_t sample_interval_ms = 1000;
    int64_t ewma_window_ms = 5000;
    double ewma_alpha = 0.2;

    bool IsValid() const {
      return sample_interval_ms > 0 && ewma_window_ms > 0 &&
             ewma_alpha > 0.0 && ewma_alpha <= 1.0 &&
             ewma_window_ms >= sample_interval_ms;
    }
  };

  UplinkTelemetry()
      : config_(),
        last_sample_time_ms_(0),
        bytes_in_current_interval_(0),
        initialized_(false),
        r_up_ewma_bps_(0.0) {}

  explicit UplinkTelemetry(const Config& config)
      : config_(config),
        last_sample_time_ms_(0),
        bytes_in_current_interval_(0),
        initialized_(false),
        r_up_ewma_bps_(0.0) {}

  void OnBytesReceived(int stream_id, size_t bytes, int64_t timestamp_ms) {
    webrtc::MutexLock lock(&mutex_);

    per_stream_bytes_[stream_id] += bytes;
    total_bytes_received_ += bytes;
    bytes_in_current_interval_ += bytes;

    if (!initialized_) {
      last_sample_time_ms_ = timestamp_ms;
      initialized_ = true;
      return;
    }

    int64_t elapsed_ms = timestamp_ms - last_sample_time_ms_;
    if (elapsed_ms >= config_.sample_interval_ms) {
      double interval_seconds = elapsed_ms / 1000.0;
      double instant_rate_bps =
          (bytes_in_current_interval_ * 8.0) / interval_seconds;

      if (r_up_ewma_bps_ == 0.0) {
        r_up_ewma_bps_ = instant_rate_bps;
      } else {
        r_up_ewma_bps_ = config_.ewma_alpha * instant_rate_bps +
                         (1.0 - config_.ewma_alpha) * r_up_ewma_bps_;
      }

      rate_samples_.push_back({timestamp_ms, instant_rate_bps});

      int64_t cutoff_ms = timestamp_ms - config_.ewma_window_ms;
      while (!rate_samples_.empty() &&
             rate_samples_.front().timestamp_ms < cutoff_ms) {
        rate_samples_.pop_front();
      }

      bytes_in_current_interval_ = 0;
      last_sample_time_ms_ = timestamp_ms;

      RTC_LOG(LS_VERBOSE) << "[UPLINK-TELEMETRY] Sample: instant="
                          << (instant_rate_bps / 1000000.0) << " Mbps, ewma="
                          << (r_up_ewma_bps_ / 1000000.0) << " Mbps, samples="
                          << rate_samples_.size();
    }
  }

  double GetUplinkRateBps() const {
    webrtc::MutexLock lock(&mutex_);
    return r_up_ewma_bps_;
  }

  double GetSigmaR() const {
    webrtc::MutexLock lock(&mutex_);

    if (rate_samples_.size() < 2) {
      return 0.0;
    }

    double sum = 0.0;
    for (const auto& sample : rate_samples_) {
      sum += sample.rate_bps;
    }
    double mean = sum / rate_samples_.size();

    double variance_sum = 0.0;
    for (const auto& sample : rate_samples_) {
      double diff = sample.rate_bps - mean;
      variance_sum += diff * diff;
    }
    double variance = variance_sum / rate_samples_.size();

    return std::sqrt(variance);
  }

  size_t GetStreamBytesReceived(int stream_id) const {
    webrtc::MutexLock lock(&mutex_);
    auto it = per_stream_bytes_.find(stream_id);
    return (it != per_stream_bytes_.end()) ? it->second : 0;
  }

  size_t GetTotalBytesReceived() const {
    webrtc::MutexLock lock(&mutex_);
    return total_bytes_received_;
  }

  void ResetStreamBytes(int stream_id) {
    webrtc::MutexLock lock(&mutex_);
    per_stream_bytes_[stream_id] = 0;
  }

  size_t GetSampleCount() const {
    webrtc::MutexLock lock(&mutex_);
    return rate_samples_.size();
  }

  bool IsReady() const {
    webrtc::MutexLock lock(&mutex_);
    return rate_samples_.size() >= 2;
  }

  const Config& GetConfig() const { return config_; }

  void Clear() {
    webrtc::MutexLock lock(&mutex_);
    per_stream_bytes_.clear();
    total_bytes_received_ = 0;
    bytes_in_current_interval_ = 0;
    rate_samples_.clear();
    r_up_ewma_bps_ = 0.0;
    last_sample_time_ms_ = 0;
    initialized_ = false;
  }

 private:
  struct RateSample {
    int64_t timestamp_ms;
    double rate_bps;
  };

  Config config_;

  int64_t last_sample_time_ms_;
  size_t bytes_in_current_interval_;
  bool initialized_;

  double r_up_ewma_bps_;

  std::deque<RateSample> rate_samples_;

  std::map<int, size_t> per_stream_bytes_;
  size_t total_bytes_received_ = 0;

  mutable webrtc::Mutex mutex_;
};

}  // namespace coordinator
}  // namespace webrtc

#endif  // PC_COORDINATOR_UPLINK_TELEMETRY_H_
