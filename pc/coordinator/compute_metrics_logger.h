/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_COORDINATOR_COMPUTE_METRICS_LOGGER_H_
#define PC_COORDINATOR_COMPUTE_METRICS_LOGGER_H_

#include <chrono>
#include <climits>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>

#include "pc/coordinator/query_tracker.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace coordinator {

// ============================================================================
// ComputeMetricsLogger - CSV logging for query response times
//
// Logs QueryMetrics to CSV file for analysis:
// - Response time (end-to-end latency)
// - Compute time (GPU time)
// - Network time
// - Utilization (compute/response)
//
// CSV columns:
// query_id,start_ms,sent_ms,first_flow_ms,last_flow_ms,
// final_decode_start_ms,complete_ms,response_time_ms,network_time_ms,
// compute_time_ms,idle_time_ms,utilization,num_flows,final_decode_tokens
// ============================================================================

// ============================================================================
// FlowTimingLogger - CSV logging for per-flow timing breakdown
//
// Logs per-flow timing for detailed analysis:
// query_id, flow_id, label, context_tokens,
// net_start_ms, net_end_ms, net_duration_ms,
// com_start_ms, com_end_ms, com_duration_ms,
// idle_before_com_ms (wait time between net_end and com_start)
// ============================================================================

class FlowTimingLogger {
 public:
  explicit FlowTimingLogger(const std::string& filepath)
      : filepath_(filepath), initialized_(false), entry_count_(0) {
    Initialize();
  }

  ~FlowTimingLogger() {
    if (file_.is_open()) {
      file_.close();
      RTC_LOG(LS_INFO) << "[FLOW-TIMING-LOGGER] Closed " << filepath_
                       << ", logged " << entry_count_ << " flows";
    }
  }

  FlowTimingLogger(const FlowTimingLogger&) = delete;
  FlowTimingLogger& operator=(const FlowTimingLogger&) = delete;

  // Log all flows from a completed query
  void LogQueryFlows(uint64_t query_id, int64_t query_start_ms,
                     const std::map<int, QueryFlowState>& flows) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!file_.is_open()) {
      RTC_LOG(LS_WARNING) << "[FLOW-TIMING-LOGGER] File not open";
      return;
    }

    // Find the earliest receive_start_ms to use as the baseline (time 0)
    // This ensures all flows are relative to when traffic actually started
    int64_t earliest_start_ms = INT64_MAX;
    for (const auto& [stream_id, flow] : flows) {
      if (flow.receive_start_ms > 0 && flow.receive_start_ms < earliest_start_ms) {
        earliest_start_ms = flow.receive_start_ms;
      }
    }
    // Fallback to query_start_ms if no valid receive times
    if (earliest_start_ms == INT64_MAX) {
      earliest_start_ms = query_start_ms;
    }

    for (const auto& [stream_id, flow] : flows) {
      // Calculate relative times from earliest flow start (so first flow starts at 0)
      int64_t net_start = flow.receive_start_ms > 0
          ? flow.receive_start_ms - earliest_start_ms : 0;
      int64_t net_end = flow.receive_done_ms > 0
          ? flow.receive_done_ms - earliest_start_ms : 0;
      int64_t net_duration = (net_end > net_start) ? net_end - net_start : 0;

      int64_t com_start = flow.compute_start_ms > 0
          ? flow.compute_start_ms - earliest_start_ms : 0;
      int64_t com_end = flow.compute_done_ms > 0
          ? flow.compute_done_ms - earliest_start_ms : 0;
      int64_t com_duration = (com_end > com_start) ? com_end - com_start : 0;

      // Idle time: waiting between network done and compute start
      int64_t idle_before_com = 0;
      if (flow.compute_start_ms > 0 && flow.receive_done_ms > 0) {
        idle_before_com = flow.compute_start_ms - flow.receive_done_ms;
        if (idle_before_com < 0) idle_before_com = 0;
      }

      file_ << query_id << ","
            << stream_id << ","
            << flow.spec.label << ","
            << flow.spec.context_tokens << ","
            << net_start << ","
            << net_end << ","
            << net_duration << ","
            << com_start << ","
            << com_end << ","
            << com_duration << ","
            << idle_before_com << "\n";

      entry_count_++;
    }

    file_.flush();
  }

  bool IsInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
  }

  const std::string& GetFilepath() const { return filepath_; }

 private:
  void Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    file_.open(filepath_, std::ios::out | std::ios::trunc);
    if (!file_.is_open()) {
      RTC_LOG(LS_ERROR) << "[FLOW-TIMING-LOGGER] Failed to open " << filepath_;
      return;
    }

    // CSV header
    file_ << "query_id,flow_id,label,context_tokens,"
          << "net_start_ms,net_end_ms,net_duration_ms,"
          << "com_start_ms,com_end_ms,com_duration_ms,"
          << "idle_before_com_ms\n";

    file_.flush();
    initialized_ = true;
    RTC_LOG(LS_INFO) << "[FLOW-TIMING-LOGGER] Initialized: " << filepath_;
  }

  std::string filepath_;
  std::ofstream file_;
  mutable std::mutex mutex_;
  bool initialized_;
  size_t entry_count_;
};


class ComputeMetricsLogger {
 public:
  // Constructor - opens file for writing
  explicit ComputeMetricsLogger(const std::string& filepath)
      : filepath_(filepath), initialized_(false), entry_count_(0) {
    Initialize();
  }

  // Destructor - closes file
  ~ComputeMetricsLogger() {
    if (file_.is_open()) {
      file_.close();
      RTC_LOG(LS_INFO) << "[METRICS-LOGGER] Closed " << filepath_
                       << ", logged " << entry_count_ << " queries";
    }
  }

  // Non-copyable
  ComputeMetricsLogger(const ComputeMetricsLogger&) = delete;
  ComputeMetricsLogger& operator=(const ComputeMetricsLogger&) = delete;

  // -------------------------------------------------------------------------
  // Logging methods
  // -------------------------------------------------------------------------

  // Log a completed query's metrics
  void LogQuery(const QueryMetrics& metrics) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!file_.is_open()) {
      RTC_LOG(LS_WARNING) << "[METRICS-LOGGER] File not open, cannot log";
      return;
    }

    // Write row
    file_ << metrics.query_id << ","
          << metrics.start_ms << ","
          << metrics.sent_ms << ","
          << metrics.first_flow_ms << ","
          << metrics.last_flow_ms << ","
          << metrics.final_decode_start_ms << ","
          << metrics.complete_ms << ","
          << metrics.response_time_ms << ","
          << metrics.network_time_ms << ","
          << std::fixed << std::setprecision(2) << metrics.total_compute_ms << ","
          << std::fixed << std::setprecision(2) << metrics.idle_time_ms << ","
          << std::fixed << std::setprecision(4) << metrics.utilization << ","
          << metrics.num_flows << ","
          << metrics.final_decode_tokens
          << "\n";

    file_.flush();  // Ensure data is written immediately
    entry_count_++;

    RTC_LOG(LS_VERBOSE) << "[METRICS-LOGGER] Logged query " << metrics.query_id
                        << ", response_time=" << metrics.response_time_ms
                        << "ms, utilization=" << (metrics.utilization * 100) << "%";
  }

  // -------------------------------------------------------------------------
  // State queries
  // -------------------------------------------------------------------------

  // Check if logger is ready
  bool IsInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
  }

  // Get number of logged entries
  size_t GetEntryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entry_count_;
  }

  // Get filepath
  const std::string& GetFilepath() const {
    return filepath_;
  }

  // -------------------------------------------------------------------------
  // Helper: Create callback for QueryTracker
  // -------------------------------------------------------------------------

  // Returns a callback suitable for QueryTracker::SetOnQueryComplete
  QueryCompleteCallback CreateCallback() {
    return [this](const QueryMetrics& metrics) {
      LogQuery(metrics);
    };
  }

 private:
  void Initialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    file_.open(filepath_, std::ios::out | std::ios::trunc);
    if (!file_.is_open()) {
      RTC_LOG(LS_ERROR) << "[METRICS-LOGGER] Failed to open " << filepath_;
      return;
    }

    // Write CSV header
    file_ << "query_id,start_ms,sent_ms,first_flow_ms,last_flow_ms,"
          << "final_decode_start_ms,complete_ms,response_time_ms,network_time_ms,"
          << "compute_time_ms,idle_time_ms,utilization,num_flows,final_decode_tokens\n";

    file_.flush();
    initialized_ = true;

    RTC_LOG(LS_INFO) << "[METRICS-LOGGER] Initialized: " << filepath_;
  }

  std::string filepath_;
  std::ofstream file_;
  mutable std::mutex mutex_;
  bool initialized_;
  size_t entry_count_;
};

}  // namespace coordinator
}  // namespace webrtc

#endif  // PC_COORDINATOR_COMPUTE_METRICS_LOGGER_H_
