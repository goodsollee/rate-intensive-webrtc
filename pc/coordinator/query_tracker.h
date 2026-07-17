/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_COORDINATOR_QUERY_TRACKER_H_
#define PC_COORDINATOR_QUERY_TRACKER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "pc/coordinator/compute_manager.h"
#include "rtc_base/logging.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_base/time_utils.h"

namespace webrtc {
namespace coordinator {

// ============================================================================
// FlowSpec - Specification for a single flow within a query
// ============================================================================

struct FlowSpec {
  int stream_id = 0;              // SCTP stream ID
  std::string label;              // "kv", "lora", "context", etc.
  size_t total_bytes = 0;         // Network transfer size

  // Compute specification
  size_t context_tokens = 0;      // Tokens to prefill (0 for KV cache)
  size_t decode_tokens = 0;       // Predicted decode tokens (for scheduling P_hat)
  size_t actual_decode_tokens = 0; // Actual decode tokens (for compute emulation)

  // Overlapped prefill: front tokens prefilled during network transfer
  double parallel_prefill_ms = 0.0;     // Front prefill time (0 = disabled)
  size_t front_context_tokens = 0;       // Number of front tokens to prefill
  std::string front_text_content;        // Raw text for front tokens (real backend)
};

// ============================================================================
// QueryConfig - Configuration for a single query
// ============================================================================

struct QueryConfig {
  uint64_t query_id = 0;

  // Per-query flow list (can vary per query)
  std::vector<FlowSpec> flows;

  // Final decode specification
  size_t final_decode_tokens = 256;
};

// ============================================================================
// QueryState - Runtime state for tracking a query
// ============================================================================

enum class QueryPhase {
  CREATED,          // Query created, not yet sent
  SENT,             // Query sent to sender
  RECEIVING,        // Receiving flow data
  COMPUTING,        // Processing flow computes
  FINAL_DECODE,     // Final decode in progress
  COMPLETED,        // Query completed
  FAILED            // Query failed
};

// QueryFlowState: State of a flow within a query (distinct from HAFS FlowState)
struct QueryFlowState {
  FlowSpec spec;
  bool received = false;           // Flow data fully received
  bool compute_started = false;    // Compute task scheduled
  bool compute_done = false;       // Compute completed

  // Timing
  int64_t receive_start_ms = 0;
  int64_t receive_done_ms = 0;
  int64_t transfer_first_byte_ms = 0;  // Actual first byte arrival (for overlap calc)
  int64_t compute_start_ms = 0;
  int64_t compute_done_ms = 0;
  std::string decode_output;  // Saved text from this flow decode
};

struct QueryState {
  QueryConfig config;
  QueryPhase phase = QueryPhase::CREATED;

  // Flow tracking
  std::map<int, QueryFlowState> flows;  // stream_id -> QueryFlowState

  // Timing
  int64_t created_ms = 0;
  int64_t sent_ms = 0;
  int64_t first_flow_received_ms = 0;
  int64_t all_flows_received_ms = 0;
  int64_t final_decode_start_ms = 0;
  int64_t completed_ms = 0;

  // Metrics
  double total_compute_ms = 0.0;

  // Helper methods
  bool AllFlowsReceived() const {
    for (const auto& [sid, state] : flows) {
      if (!state.received) return false;
    }
    return true;
  }

  bool AllFlowsComputeDone() const {
    for (const auto& [sid, state] : flows) {
      // Check flows that need compute (context_tokens > 0 OR decode_tokens > 0)
      if ((state.spec.context_tokens > 0 || state.spec.decode_tokens > 0) && !state.compute_done) {
        return false;
      }
    }
    return true;
  }

  std::string BuildAggregatePrompt() const {
    std::string prompt;
    int ctx_idx = 1;
    for (const auto& [stream_id, flow] : flows) {
      if (!flow.decode_output.empty()) {
        prompt += "Context " + std::to_string(ctx_idx++) + ": "
                + flow.decode_output + "\n";
      }
    }
    if (!prompt.empty()) {
      prompt += "\nBased on the information above, provide a comprehensive answer.\n";
    }
    return prompt;
  }

  size_t GetReceivedFlowCount() const {
    size_t count = 0;
    for (const auto& [sid, state] : flows) {
      if (state.received) count++;
    }
    return count;
  }

  size_t GetComputeCompleteCount() const {
    size_t count = 0;
    for (const auto& [sid, state] : flows) {
      if (state.compute_done) count++;
    }
    return count;
  }
};

// ============================================================================
// QueryMetrics - Aggregated metrics for a completed query
// ============================================================================

struct QueryMetrics {
  uint64_t query_id = 0;

  // Timing (all in milliseconds)
  int64_t start_ms = 0;
  int64_t sent_ms = 0;
  int64_t first_flow_ms = 0;
  int64_t last_flow_ms = 0;
  int64_t final_decode_start_ms = 0;
  int64_t complete_ms = 0;

  // Derived metrics
  int64_t response_time_ms = 0;       // complete - start
  int64_t network_time_ms = 0;        // last_flow - first_flow
  double total_compute_ms = 0.0;      // sum of all compute times
  double idle_time_ms = 0.0;          // response_time - compute
  double utilization = 0.0;           // compute / response

  size_t num_flows = 0;
  size_t final_decode_tokens = 0;
};

// Callback types
using QueryCompleteCallback = std::function<void(const QueryMetrics& metrics)>;

// TaskEnricher: Injects data (KV cache / raw text) into ComputeTask before scheduling
using TaskEnricher = std::function<void(ComputeTask& task, const std::string& label)>;

// ============================================================================
// QueryTracker - Tracks active queries and coordinates compute
//
// This class manages the lifecycle of queries:
// 1. CreateQuery() - Create and configure a query
// 2. OnQuerySent() - Mark query as sent to sender
// 3. OnFlowReceived() - Track flow data completion
// 4. OnComputeComplete() - Track compute task completion
// 5. OnFinalDecodeComplete() - Query completed
//
// Works with ComputeManager to schedule compute tasks when flow data arrives.
// ============================================================================

class QueryTracker {
 public:
  // Constructor with ComputeManager for scheduling
  explicit QueryTracker(ComputeManager* compute_manager)
      : compute_manager_(compute_manager) {}

  // -------------------------------------------------------------------------
  // Query lifecycle
  // -------------------------------------------------------------------------

  // Create a new query with given config
  // Returns query_id
  // start_time_ms: optional custom start time (uses current time if 0)
  uint64_t CreateQuery(const QueryConfig& config, int64_t start_time_ms = 0);

  // Mark query as sent to sender
  void OnQuerySent(uint64_t query_id);

  // Called when flow data is fully received
  // Automatically schedules compute task if needed
  // first_arrival_ms: network start time (0 = use query creation time)
  // timestamp_ms: network end time (flow complete time)
  void OnFlowReceived(uint64_t query_id, int stream_id,
                      int64_t timestamp_ms, int64_t first_arrival_ms = 0);

  // Called when a compute task completes
  // Automatically schedules final decode when all flows done
  // actual_start_ms: when compute actually started (not when scheduled)
  // timestamp_ms: when compute finished
  void OnComputeComplete(uint64_t query_id, int stream_id, int64_t timestamp_ms,
                         int64_t actual_start_ms = 0,
                         const std::string& decode_output = std::string());

  // Called when final decode completes
  // Computes final metrics and invokes callback
  void OnFinalDecodeComplete(uint64_t query_id, int64_t timestamp_ms);

  // Set callback for query completion
  void SetOnQueryComplete(QueryCompleteCallback callback);

  // Set task enricher callback (injects data into ComputeTask before scheduling)
  void SetTaskEnricher(TaskEnricher enricher) {
    task_enricher_ = std::move(enricher);
  }

  // -------------------------------------------------------------------------
  // State queries
  // -------------------------------------------------------------------------

  // Get query state (returns nullptr if not found)
  const QueryState* GetQueryState(uint64_t query_id) const;

  // Check if query is ready for final decode
  bool IsReadyForFinalDecode(uint64_t query_id) const;

  // Get number of active queries
  size_t GetActiveQueryCount() const;

  // Get all active query IDs
  std::vector<uint64_t> GetActiveQueryIds() const;

  // -------------------------------------------------------------------------
  // Metrics
  // -------------------------------------------------------------------------

  // Get metrics for completed query
  std::optional<QueryMetrics> GetQueryMetrics(uint64_t query_id) const;

 private:
  // Schedule compute for a flow (called when flow data received)
  void ScheduleFlowCompute(uint64_t query_id, int stream_id);

  // Schedule final decode (called when all flow computes done)
  void ScheduleFinalDecode(uint64_t query_id);

  // Compute final metrics for query
  QueryMetrics ComputeMetrics(const QueryState& query) const;

  // Get current time in milliseconds
  static int64_t GetCurrentTimeMs();

  // Compute manager (not owned)
  ComputeManager* compute_manager_;

  // Active queries
  std::map<uint64_t, QueryState> queries_;

  // Completed query metrics (retained for retrieval)
  std::map<uint64_t, QueryMetrics> completed_metrics_;

  // Next query ID
  uint64_t next_query_id_ = 1;

  // Callbacks
  QueryCompleteCallback on_complete_callback_;
  TaskEnricher task_enricher_;

  mutable webrtc::Mutex mutex_;
};

// ============================================================================
// Implementation
// ============================================================================

inline uint64_t QueryTracker::CreateQuery(const QueryConfig& config,
                                          int64_t start_time_ms) {
  webrtc::MutexLock lock(&mutex_);

  uint64_t query_id = (config.query_id > 0) ? config.query_id : next_query_id_++;

  // Use provided start_time_ms if given, otherwise use current time
  int64_t effective_start_time = (start_time_ms > 0) ? start_time_ms : GetCurrentTimeMs();

  QueryState state;
  state.config = config;
  state.config.query_id = query_id;
  state.phase = QueryPhase::CREATED;
  state.created_ms = effective_start_time;

  // Initialize flow states
  for (const auto& flow : config.flows) {
    QueryFlowState flow_state;
    flow_state.spec = flow;
    // Set receive_start_ms to query creation time (all flows start together)
    flow_state.receive_start_ms = state.created_ms;
    state.flows[flow.stream_id] = flow_state;
  }

  queries_[query_id] = std::move(state);

  RTC_LOG(LS_INFO) << "[QUERY-TRACKER] Created query " << query_id
                   << " with " << config.flows.size() << " flows"
                   << ", final_decode_tokens=" << config.final_decode_tokens
                   << ", start_time=" << effective_start_time;

  return query_id;
}

inline void QueryTracker::OnQuerySent(uint64_t query_id) {
  webrtc::MutexLock lock(&mutex_);

  auto it = queries_.find(query_id);
  if (it == queries_.end()) {
    RTC_LOG(LS_WARNING) << "[QUERY-TRACKER] OnQuerySent: unknown query "
                        << query_id;
    return;
  }

  it->second.phase = QueryPhase::SENT;
  it->second.sent_ms = GetCurrentTimeMs();

  RTC_LOG(LS_VERBOSE) << "[QUERY-TRACKER] Query " << query_id << " sent";
}

inline void QueryTracker::OnFlowReceived(uint64_t query_id, int stream_id,
                                          int64_t timestamp_ms,
                                          int64_t first_arrival_ms) {
  webrtc::MutexLock lock(&mutex_);

  auto it = queries_.find(query_id);
  if (it == queries_.end()) {
    RTC_LOG(LS_WARNING) << "[QUERY-TRACKER] OnFlowReceived: unknown query "
                        << query_id;
    return;
  }

  QueryState& query = it->second;
  auto flow_it = query.flows.find(stream_id);
  if (flow_it == query.flows.end()) {
    RTC_LOG(LS_WARNING) << "[QUERY-TRACKER] OnFlowReceived: unknown stream "
                        << stream_id << " for query " << query_id;
    return;
  }

  QueryFlowState& flow = flow_it->second;

  // Only update timing on FIRST receive (ignore repeated file transfers)
  if (!flow.received) {
    flow.received = true;
    flow.receive_done_ms = timestamp_ms;
    flow.transfer_first_byte_ms = first_arrival_ms > 0 ? first_arrival_ms : timestamp_ms;
    // Keep receive_start_ms as query creation time (set in CreateQuery)
    // This ensures all flows in a query have the same network start time
    // Do NOT overwrite with first_arrival_ms
  } else {
    // Already received - ignore repeated transfers
    return;
  }

  // Track first flow timing
  if (query.first_flow_received_ms == 0) {
    query.first_flow_received_ms = timestamp_ms;
    query.phase = QueryPhase::RECEIVING;
  }

  RTC_LOG(LS_INFO) << "[QUERY-TRACKER] Query " << query_id
                   << " flow " << stream_id << " (" << flow.spec.label
                   << ") received, " << query.GetReceivedFlowCount()
                   << "/" << query.flows.size() << " flows";

  // Schedule compute if needed (prefill and/or per-flow decode)
  if (flow.spec.context_tokens > 0 || flow.spec.decode_tokens > 0 ||
      flow.spec.actual_decode_tokens > 0) {
    fprintf(stderr, "[QUERY-TRACKER] Scheduling compute: query=%lu stream=%d "
            "ctx_tokens=%zu dec_tokens=%zu actual_dec=%zu label=%s\n",
            (unsigned long)query_id, stream_id,
            flow.spec.context_tokens, flow.spec.decode_tokens,
            flow.spec.actual_decode_tokens, flow.spec.label.c_str());
    // Release lock before scheduling (callback may reenter)
    mutex_.Unlock();
    ScheduleFlowCompute(query_id, stream_id);
    mutex_.Lock();
  } else {
    // No compute needed for this flow, mark as done
    flow.compute_done = true;

    fprintf(stderr, "[QUERY-TRACKER] No compute: query=%lu stream=%d "
            "ctx_tokens=%zu dec_tokens=%zu — skipping\n",
            (unsigned long)query_id, stream_id,
            flow.spec.context_tokens, flow.spec.decode_tokens);
  }

  // Check if all flows received
  if (query.AllFlowsReceived()) {
    // Compute max receive_done_ms across all flows (not just current timestamp)
    // This ensures we capture the actual time when ALL data has arrived,
    // even if flows are processed out of order due to priority scheduling
    int64_t max_receive_ms = 0;
    for (const auto& [sid, fstate] : query.flows) {
      if (fstate.receive_done_ms > max_receive_ms) {
        max_receive_ms = fstate.receive_done_ms;
      }
    }
    query.all_flows_received_ms = max_receive_ms;
    RTC_LOG(LS_INFO) << "[QUERY-TRACKER] Query " << query_id
                     << " all flows received at " << max_receive_ms;
  }

  // For flows with no compute (context_tokens=0 AND decode_tokens=0),
  // check if query is complete since OnComputeComplete won't be called
  if (flow.spec.context_tokens == 0 && flow.spec.decode_tokens == 0 &&
      query.AllFlowsReceived() && query.AllFlowsComputeDone()) {
    // Release lock before scheduling final decode
    mutex_.Unlock();
    ScheduleFinalDecode(query_id);
    mutex_.Lock();
  }
}

inline void QueryTracker::OnComputeComplete(uint64_t query_id, int stream_id,
                                             int64_t timestamp_ms,
                                             int64_t actual_start_ms,
                                             const std::string& decode_output) {
  bool ready_for_final = false;

  {
    webrtc::MutexLock lock(&mutex_);

    auto it = queries_.find(query_id);
    if (it == queries_.end()) {
      RTC_LOG(LS_WARNING) << "[QUERY-TRACKER] OnComputeComplete: unknown query "
                          << query_id;
      return;
    }

    QueryState& query = it->second;
    auto flow_it = query.flows.find(stream_id);
    if (flow_it == query.flows.end()) {
      RTC_LOG(LS_WARNING) << "[QUERY-TRACKER] OnComputeComplete: unknown stream "
                          << stream_id << " for query " << query_id;
      return;
    }

    QueryFlowState& flow = flow_it->second;
    flow.compute_done = true;
    flow.compute_done_ms = timestamp_ms;
    flow.decode_output = decode_output;
    // Use actual start time from ComputeManager if provided (more accurate)
    if (actual_start_ms > 0) {
      flow.compute_start_ms = actual_start_ms;
    }

    fprintf(stderr, "[COMPUTE-FLOW-DONE] query=%lu stream=%d "
            "%zu/%zu flows computed, duration=%lldms\n",
            (unsigned long)query_id, stream_id,
            query.GetComputeCompleteCount(), query.flows.size(),
            (long long)(flow.compute_done_ms - flow.compute_start_ms));

    // Check if ready for final decode
    ready_for_final = query.AllFlowsReceived() && query.AllFlowsComputeDone();
  }

  if (ready_for_final) {
    ScheduleFinalDecode(query_id);
  }
}

inline void QueryTracker::OnFinalDecodeComplete(uint64_t query_id,
                                                 int64_t timestamp_ms) {
  QueryMetrics metrics;

  {
    webrtc::MutexLock lock(&mutex_);

    auto it = queries_.find(query_id);
    if (it == queries_.end()) {
      RTC_LOG(LS_WARNING) << "[QUERY-TRACKER] OnFinalDecodeComplete: unknown query "
                          << query_id;
      return;
    }

    QueryState& query = it->second;
    query.phase = QueryPhase::COMPLETED;
    query.completed_ms = timestamp_ms;

    metrics = ComputeMetrics(query);
    completed_metrics_[query_id] = metrics;

    fprintf(stderr, "[E2E-RESPONSE] query=%lu response_time=%lldms "
            "network=%lldms compute=%lldms idle=%.0fms util=%.1f%%\n",
            (unsigned long)query_id,
            (long long)metrics.response_time_ms,
            (long long)metrics.network_time_ms,
            (long long)metrics.total_compute_ms,
            metrics.idle_time_ms,
            metrics.utilization * 100.0);
    RTC_LOG(LS_INFO) << "[QUERY-TRACKER] Query " << query_id << " COMPLETED"
                     << ", response_time=" << metrics.response_time_ms << "ms"
                     << ", compute_time=" << metrics.total_compute_ms << "ms"
                     << ", utilization=" << (metrics.utilization * 100) << "%";
  }

  // Invoke callback outside lock
  if (on_complete_callback_) {
    on_complete_callback_(metrics);
  }
}

inline void QueryTracker::SetOnQueryComplete(QueryCompleteCallback callback) {
  webrtc::MutexLock lock(&mutex_);
  on_complete_callback_ = std::move(callback);
}

inline const QueryState* QueryTracker::GetQueryState(uint64_t query_id) const {
  webrtc::MutexLock lock(&mutex_);
  auto it = queries_.find(query_id);
  return (it != queries_.end()) ? &it->second : nullptr;
}

inline bool QueryTracker::IsReadyForFinalDecode(uint64_t query_id) const {
  webrtc::MutexLock lock(&mutex_);
  auto it = queries_.find(query_id);
  if (it == queries_.end()) return false;
  const QueryState& query = it->second;
  return query.AllFlowsReceived() && query.AllFlowsComputeDone();
}

inline size_t QueryTracker::GetActiveQueryCount() const {
  webrtc::MutexLock lock(&mutex_);
  size_t count = 0;
  for (const auto& [id, query] : queries_) {
    if (query.phase != QueryPhase::COMPLETED &&
        query.phase != QueryPhase::FAILED) {
      count++;
    }
  }
  return count;
}

inline std::vector<uint64_t> QueryTracker::GetActiveQueryIds() const {
  webrtc::MutexLock lock(&mutex_);
  std::vector<uint64_t> ids;
  for (const auto& [id, query] : queries_) {
    if (query.phase != QueryPhase::COMPLETED &&
        query.phase != QueryPhase::FAILED) {
      ids.push_back(id);
    }
  }
  return ids;
}

inline std::optional<QueryMetrics> QueryTracker::GetQueryMetrics(
    uint64_t query_id) const {
  webrtc::MutexLock lock(&mutex_);
  auto it = completed_metrics_.find(query_id);
  if (it != completed_metrics_.end()) {
    return it->second;
  }
  return std::nullopt;
}

inline void QueryTracker::ScheduleFlowCompute(uint64_t query_id, int stream_id) {
  if (!compute_manager_) {
    RTC_LOG(LS_WARNING) << "[QUERY-TRACKER] No ComputeManager available";
    return;
  }

  webrtc::MutexLock lock(&mutex_);

  auto it = queries_.find(query_id);
  if (it == queries_.end()) {
    return;
  }

  QueryState& query = it->second;
  auto flow_it = query.flows.find(stream_id);
  if (flow_it == query.flows.end()) {
    return;
  }

  QueryFlowState& flow = flow_it->second;
  if (flow.compute_started) {
    return;  // Already scheduled
  }

  flow.compute_started = true;
  flow.compute_start_ms = GetCurrentTimeMs();
  query.phase = QueryPhase::COMPUTING;

  ComputeTask task;
  task.query_id = query_id;
  task.flow_id = stream_id;
  task.label = flow.spec.label;
  task.context_tokens = flow.spec.context_tokens;
  task.decode_tokens = flow.spec.actual_decode_tokens > 0
                        ? flow.spec.actual_decode_tokens
                        : flow.spec.decode_tokens;  // Use actual if available
  task.parallel_prefill_ms = flow.spec.parallel_prefill_ms;
  task.transfer_start_ms = flow.transfer_first_byte_ms;
  task.front_context_tokens = flow.spec.front_context_tokens;
  task.front_text_content = flow.spec.front_text_content;

  // Inject pending data (KV cache / raw text) via enricher
  if (task_enricher_) {
    task_enricher_(task, flow.spec.label);
  }

  RTC_LOG(LS_VERBOSE) << "[QUERY-TRACKER] Scheduling compute for query "
                      << query_id << " flow " << stream_id
                      << " (" << flow.spec.label << ")"
                      << ", context_tokens=" << task.context_tokens;

  // Release lock before scheduling
  mutex_.Unlock();

  compute_manager_->ScheduleTask(task, [this, query_id, stream_id](
      const ComputeTask& completed) {
    OnComputeComplete(query_id, stream_id, completed.compute_done_ms,
                      completed.compute_start_ms, completed.decode_output);
  });

  mutex_.Lock();
}

inline void QueryTracker::ScheduleFinalDecode(uint64_t query_id) {
  if (!compute_manager_) {
    RTC_LOG(LS_WARNING) << "[QUERY-TRACKER] No ComputeManager available";
    return;
  }

  size_t final_decode_tokens;

  {
    webrtc::MutexLock lock(&mutex_);

    auto it = queries_.find(query_id);
    if (it == queries_.end()) {
      return;
    }

    QueryState& query = it->second;
    if (query.phase == QueryPhase::FINAL_DECODE ||
        query.phase == QueryPhase::COMPLETED) {
      return;  // Already in progress or done
    }

    query.phase = QueryPhase::FINAL_DECODE;
    query.final_decode_start_ms = GetCurrentTimeMs();
    final_decode_tokens = query.config.final_decode_tokens;

    RTC_LOG(LS_INFO) << "[QUERY-TRACKER] Starting final decode for query "
                     << query_id << ", tokens=" << final_decode_tokens;
  }

  // Build aggregate prompt from per-flow outputs
  std::string aggregate_prompt;
  {
    webrtc::MutexLock lock(&mutex_);
    auto it = queries_.find(query_id);
    if (it != queries_.end()) {
      aggregate_prompt = it->second.BuildAggregatePrompt();
    }
  }

  fprintf(stderr, "[AGGREGATE] query=%lu prompt_len=%zu prompt=%.500s%s\n",
          (unsigned long)query_id, aggregate_prompt.size(),
          aggregate_prompt.c_str(),
          aggregate_prompt.size() > 500 ? "..." : "");

  ComputeTask task;
  task.query_id = query_id;
  task.flow_id = 99;  // Special ID for final decode
  task.label = "final";
  task.context_tokens = 0;
  task.decode_tokens = final_decode_tokens;
  task.aggregate_prompt = std::move(aggregate_prompt);
  task.data_format = "raw_text";  // Final decode uses from-scratch prefill

  compute_manager_->ScheduleTask(task, [this, query_id](
      const ComputeTask& completed) {
    OnFinalDecodeComplete(query_id, GetCurrentTimeMs());
  });
}

inline QueryMetrics QueryTracker::ComputeMetrics(const QueryState& query) const {
  QueryMetrics m;
  m.query_id = query.config.query_id;
  m.start_ms = query.created_ms;
  m.sent_ms = query.sent_ms;
  m.first_flow_ms = query.first_flow_received_ms;
  m.last_flow_ms = query.all_flows_received_ms;
  m.final_decode_start_ms = query.final_decode_start_ms;
  m.complete_ms = query.completed_ms;

  m.response_time_ms = m.complete_ms - m.start_ms;
  m.network_time_ms = (m.last_flow_ms > 0 && m.first_flow_ms > 0)
                          ? (m.last_flow_ms - m.first_flow_ms)
                          : 0;

  m.num_flows = query.flows.size();
  m.final_decode_tokens = query.config.final_decode_tokens;

  // Sum up compute times
  m.total_compute_ms = 0.0;
  for (const auto& [sid, flow] : query.flows) {
    if (flow.compute_done_ms > 0 && flow.compute_start_ms > 0) {
      m.total_compute_ms += (flow.compute_done_ms - flow.compute_start_ms);
    }
  }
  // Add final decode time
  if (m.complete_ms > 0 && m.final_decode_start_ms > 0) {
    m.total_compute_ms += (m.complete_ms - m.final_decode_start_ms);
  }

  m.idle_time_ms = m.response_time_ms - m.total_compute_ms;
  if (m.idle_time_ms < 0) m.idle_time_ms = 0;

  m.utilization = (m.response_time_ms > 0)
                      ? (m.total_compute_ms / m.response_time_ms)
                      : 0.0;

  // Clamp utilization to [0, 1] due to timing measurement variations
  if (m.utilization > 1.0) m.utilization = 1.0;

  return m;
}

inline int64_t QueryTracker::GetCurrentTimeMs() {
  // Use webrtc::TimeMillis() for consistency with file_receiver timing
  return webrtc::TimeMillis();
}

}  // namespace coordinator
}  // namespace webrtc

#endif  // PC_COORDINATOR_QUERY_TRACKER_H_
