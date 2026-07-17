/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_COORDINATOR_COMPUTE_EMULATION_INTEGRATION_H_
#define PC_COORDINATOR_COMPUTE_EMULATION_INTEGRATION_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "pc/coordinator/compute_manager.h"
#include "pc/coordinator/compute_metrics_logger.h"
#include "pc/coordinator/emulated_compute_backend.h"
#ifdef ENABLE_LLAMA_INFERENCE
#include "pc/coordinator/llama_compute_backend.h"
#endif
#include "pc/coordinator/query_tracker.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace coordinator {

// ============================================================================
// FlowConfig - Configuration for a single flow from CSV
// ============================================================================

struct FlowConfig {
  std::string label;              // e.g., "q1_kv_cache"
  uint64_t query_id = 0;          // Extracted from label: 1
  std::string flow_type;          // Extracted from label: "kv_cache"
  size_t total_bytes = 0;         // Network transfer size
  double P_hat_ms = 0.0;          // Estimated compute time from CSV
  double sigma_P_ms = 0.0;        // Variance in compute time
  int stream_id = 0;              // SCTP stream ID

  // Compute tokens (derived from flow_type)
  size_t context_tokens = 0;      // Tokens to prefill (0 for KV cache)
};

// ============================================================================
// ComputeEmulationIntegration - Ties together all compute emulation components
//
// This class integrates:
// - EmulatedComputeBackend (GPU simulation via sleep)
// - ComputeManager (task queue and scheduling)
// - QueryTracker (query lifecycle management)
// - ComputeMetricsLogger (CSV output)
//
// Usage:
// 1. Create with output directory
// 2. RegisterFlow() or RegisterFlowFull() for each SCTP flow
// 3. OnFlowCompleted() when a file is fully received
// 4. CSV output written automatically via ComputeMetricsLogger
// ============================================================================

class ComputeEmulationIntegration {
 public:
  // Constructor - initializes all components
  explicit ComputeEmulationIntegration(const std::string& log_dir,
                                        const GpuConfig& gpu_config = GpuConfig())
      : log_dir_(log_dir),
        gpu_config_(gpu_config),
        initialized_(false) {
    Initialize();
  }

  ~ComputeEmulationIntegration() {
    RTC_LOG(LS_INFO) << "[COMPUTE-EMULATION] Shutdown. Total queries: "
                     << completed_query_count_;
  }

  // Non-copyable
  ComputeEmulationIntegration(const ComputeEmulationIntegration&) = delete;
  ComputeEmulationIntegration& operator=(const ComputeEmulationIntegration&) = delete;

  // -------------------------------------------------------------------------
  // Flow registration (call from Conductor during setup)
  // -------------------------------------------------------------------------

  // Register a flow with full token details (preferred method for V21+)
  // Accepts all fields directly, no external dependencies needed
  void RegisterFlowFull(const std::string& label, int stream_id,
                        uint64_t query_id, size_t total_bytes,
                        size_t context_tokens, size_t decode_tokens,
                        size_t actual_decode_tokens, size_t final_decode_tokens,
                        double parallel_prefill_ms = 0.0,
                        size_t front_context_tokens = 0,
                        const std::string& front_text_content = std::string()) {
    FlowConfig flow_config;
    flow_config.label = label;
    flow_config.query_id = query_id;
    flow_config.flow_type = label;  // Use full label as flow type
    flow_config.total_bytes = total_bytes;
    flow_config.stream_id = stream_id;
    flow_config.context_tokens = context_tokens;

    flows_[label] = flow_config;
    stream_to_label_[stream_id] = label;

    RTC_LOG(LS_INFO) << "[COMPUTE-EMULATION] RegisterFlowFull: " << label
                     << " query=" << query_id
                     << " stream=" << stream_id
                     << " bytes=" << total_bytes
                     << " context_tokens=" << context_tokens
                     << " decode_tokens=" << decode_tokens
                     << " actual_decode_tokens=" << actual_decode_tokens
                     << " parallel_prefill_ms=" << parallel_prefill_ms;

    // Build query config with full token info
    auto& config = query_configs_[query_id];
    config.query_id = query_id;

    FlowSpec spec;
    spec.stream_id = stream_id;
    spec.label = label;
    spec.total_bytes = total_bytes;
    spec.context_tokens = context_tokens;
    spec.decode_tokens = decode_tokens;
    spec.actual_decode_tokens = actual_decode_tokens > 0
                                    ? actual_decode_tokens
                                    : decode_tokens;
    spec.parallel_prefill_ms = parallel_prefill_ms;
    spec.front_context_tokens = front_context_tokens;
    spec.front_text_content = front_text_content;

    config.flows.push_back(spec);

    if (final_decode_tokens > 0) {
      config.final_decode_tokens = final_decode_tokens;
    } else if (config.final_decode_tokens == 0) {
      config.final_decode_tokens = 256;  // Default
    }
  }

  // Register a flow from sctp.csv config (legacy method)
  // label: traffic name (e.g., "q1_kv_cache")
  // stream_id: SCTP stream ID
  // total_bytes: network transfer size
  // P_hat_ms: estimated compute time from CSV (optional)
  void RegisterFlow(const std::string& label, int stream_id,
                    size_t total_bytes, double P_hat_ms = 0.0) {
    FlowConfig config;
    config.label = label;
    config.stream_id = stream_id;
    config.total_bytes = total_bytes;
    config.P_hat_ms = P_hat_ms;

    // Parse query_id and flow_type from label (e.g., "q1_kv_cache" -> 1, "kv_cache")
    ParseFlowLabel(label, config.query_id, config.flow_type);

    // Determine context_tokens based on flow_type
    config.context_tokens = DeriveContextTokens(config.flow_type, config.P_hat_ms);

    flows_[label] = config;
    stream_to_label_[stream_id] = label;

    RTC_LOG(LS_INFO) << "[COMPUTE-EMULATION] Registered flow: " << label
                     << " query=" << config.query_id
                     << " type=" << config.flow_type
                     << " stream=" << stream_id
                     << " bytes=" << total_bytes
                     << " P_hat=" << P_hat_ms << "ms"
                     << " context_tokens=" << config.context_tokens;

    // Create or update query config
    EnsureQueryConfig(config.query_id, config);
  }

  // -------------------------------------------------------------------------
  // Flow completion callback (call from file_receiver when file is complete)
  // -------------------------------------------------------------------------

  // Called when a flow's network transfer is complete
  // first_arrival_ms: timestamp when first packet arrived (network start)
  // last_arrival_ms: timestamp when last packet arrived (network end)
  void OnFlowCompleted(const std::string& label,
                       int64_t first_arrival_ms = 0,
                       int64_t last_arrival_ms = 0) {
    int64_t timestamp_ms = last_arrival_ms;
    if (timestamp_ms == 0) {
      timestamp_ms = GetCurrentTimeMs();
    }

    auto it = flows_.find(label);
    if (it == flows_.end()) {
      RTC_LOG(LS_WARNING) << "[COMPUTE-EMULATION] Unknown flow: " << label;
      return;
    }

    const FlowConfig& flow = it->second;
    uint64_t query_id = flow.query_id;

    // Ensure query is created
    auto query_it = active_queries_.find(query_id);
    if (query_it == active_queries_.end()) {
      // Query not yet created - create it now (first flow of query)
      // Use first_arrival_ms as the query start time for accurate timing
      CreateQuery(query_id, first_arrival_ms);
    }

    // Look up the tracker's actual_id for this query
    auto tracker_id_it = query_id_to_tracker_id_.find(query_id);
    if (tracker_id_it == query_id_to_tracker_id_.end()) {
      RTC_LOG(LS_WARNING) << "[COMPUTE-EMULATION] No tracker_id mapping for query=" << query_id;
      return;
    }
    uint64_t tracker_id = tracker_id_it->second;

    RTC_LOG(LS_VERBOSE) << "[COMPUTE-EMULATION] Flow completed: " << label
                        << " query=" << query_id << " tracker_id=" << tracker_id
                        << " stream=" << flow.stream_id;

    // Notify QueryTracker using the tracker's actual_id
    // Pass first_arrival_ms for accurate network timing
    if (query_tracker_) {
      query_tracker_->OnFlowReceived(tracker_id, flow.stream_id, timestamp_ms,
                                      first_arrival_ms);
    }
  }

  // -------------------------------------------------------------------------
  // State queries
  // -------------------------------------------------------------------------

  bool IsInitialized() const { return initialized_; }

  size_t GetCompletedQueryCount() const { return completed_query_count_; }

  const ComputeMetricsLogger* GetMetricsLogger() const {
    return metrics_logger_.get();
  }

  const ComputeManager* GetComputeManager() const {
    return compute_manager_.get();
  }

  // -------------------------------------------------------------------------
  // Data passing for real inference backends
  // -------------------------------------------------------------------------

  // Store flow data (KV cache or raw text) to be injected into ComputeTask
  // Call before OnFlowCompleted() for the same label
  void SetFlowData(const std::string& label,
                   std::vector<uint8_t> kvcache_buffer,
                   std::string text_content,
                   const std::string& data_format = "") {
    PendingFlowData data;
    data.kvcache = std::move(kvcache_buffer);
    data.text = std::move(text_content);
    data.data_format = data_format;
    pending_data_[label] = std::move(data);

    RTC_LOG(LS_VERBOSE) << "[COMPUTE-EMULATION] SetFlowData: " << label
                        << " kvcache=" << pending_data_[label].kvcache.size()
                        << " text=" << pending_data_[label].text.size()
                        << " format=" << data_format;
  }

  // External callback type for query completion notification
  using ExternalQueryCompleteCallback = std::function<void(uint64_t query_id)>;

  // Set external callback for query completion (called by QueryManager)
  void SetExternalQueryCompleteCallback(ExternalQueryCompleteCallback callback) {
    external_query_complete_callback_ = std::move(callback);
  }

  // Reset all flow/query state for a new query set (receiver-driven mode)
  void ResetForNewQuery() {
    flows_.clear();
    stream_to_label_.clear();
    query_configs_.clear();
    active_queries_.clear();
    query_id_to_tracker_id_.clear();
    pending_data_.clear();
    RTC_LOG(LS_INFO) << "[COMPUTE-EMULATION] ResetForNewQuery: cleared all state";
  }

 private:
  void Initialize() {
    // Backend selection via environment variable
    std::unique_ptr<ComputeBackend> backend;
    const char* backend_type = std::getenv("COMPUTE_BACKEND");

#ifdef ENABLE_LLAMA_INFERENCE
    if (backend_type && std::string(backend_type) == "llama") {
      auto llama_config = LlamaCppBackend::Config::FromEnvironment();
      auto llama_backend = std::make_unique<LlamaCppBackend>();
      if (!llama_backend->Init(llama_config)) {
        fprintf(stderr, "[COMPUTE-EMULATION] LlamaCppBackend init failed, "
                "falling back to emulation\n");
        backend = std::make_unique<EmulatedComputeBackend>(gpu_config_);
      } else {
        fprintf(stderr, "[COMPUTE-EMULATION] Using LlamaCppBackend (real GPU)\n");
        backend = std::move(llama_backend);
      }
    } else {
      backend = std::make_unique<EmulatedComputeBackend>(gpu_config_);
    }
#else
    if (backend_type && std::string(backend_type) == "llama") {
      fprintf(stderr, "[COMPUTE-EMULATION] COMPUTE_BACKEND=llama but "
              "ENABLE_LLAMA_INFERENCE not compiled. Using emulation.\n");
    }
    backend = std::make_unique<EmulatedComputeBackend>(gpu_config_);
#endif

    // Create compute manager with backend
    compute_manager_ = std::make_unique<ComputeManager>(std::move(backend));

    // Create query tracker with compute manager
    query_tracker_ = std::make_unique<QueryTracker>(compute_manager_.get());

    // Wire task enricher to inject pending data into ComputeTask
    query_tracker_->SetTaskEnricher(
        [this](ComputeTask& task, const std::string& label) {
          auto it = pending_data_.find(label);
          if (it != pending_data_.end()) {
            task.kvcache_buffer = std::move(it->second.kvcache);
            task.text_content = std::move(it->second.text);
            // Use stored format if available, otherwise infer from data
            task.data_format = !it->second.data_format.empty()
                             ? it->second.data_format
                             : !task.kvcache_buffer.empty() ? "kv_cache"
                             : !task.text_content.empty()   ? "raw_text"
                                                            : "";
            pending_data_.erase(it);
            RTC_LOG(LS_INFO) << "[COMPUTE-EMULATION] Enriched task: "
                             << label << " format=" << task.data_format;
          }
        });

    // Create metrics logger
    std::string metrics_path = log_dir_;
    if (!metrics_path.empty() && metrics_path.back() != '/') {
      metrics_path += '/';
    }
    metrics_path += "query_response_time.csv";
    metrics_logger_ = std::make_unique<ComputeMetricsLogger>(metrics_path);

    // Create flow timing logger
    std::string flow_timing_path = log_dir_;
    if (!flow_timing_path.empty() && flow_timing_path.back() != '/') {
      flow_timing_path += '/';
    }
    flow_timing_path += "flow_timing.csv";
    flow_timing_logger_ = std::make_unique<FlowTimingLogger>(flow_timing_path);

    // Wire query completion callback to metrics logger
    query_tracker_->SetOnQueryComplete([this](const QueryMetrics& metrics) {
      OnQueryComplete(metrics);
    });

    initialized_ = true;

    RTC_LOG(LS_INFO) << "[COMPUTE-EMULATION] Initialized with GPU config: "
                     << "prefill=" << gpu_config_.prefill_tokens_per_sec << " tok/s"
                     << ", decode=" << gpu_config_.decode_tokens_per_sec << " tok/s"
                     << ", output=" << metrics_path;
  }

  void OnQueryComplete(const QueryMetrics& metrics) {
    // Log query-level metrics to CSV
    if (metrics_logger_) {
      metrics_logger_->LogQuery(metrics);
    }

    // Log per-flow timing to CSV
    if (flow_timing_logger_ && query_tracker_) {
      const QueryState* state = query_tracker_->GetQueryState(metrics.query_id);
      if (state) {
        flow_timing_logger_->LogQueryFlows(metrics.query_id,
                                            state->created_ms,
                                            state->flows);
      }
    }

    completed_query_count_++;

    RTC_LOG(LS_INFO) << "[COMPUTE-EMULATION] Query " << metrics.query_id
                     << " COMPLETE"
                     << " response_time=" << metrics.response_time_ms << "ms"
                     << " compute_time=" << metrics.total_compute_ms << "ms"
                     << " utilization=" << (metrics.utilization * 100) << "%";

    // Mark query as no longer active
    active_queries_.erase(metrics.query_id);

    // Notify external listener (QueryManager)
    if (external_query_complete_callback_) {
      external_query_complete_callback_(metrics.query_id);
    }
  }

  void ParseFlowLabel(const std::string& label, uint64_t& query_id,
                      std::string& flow_type) {
    // Expected format: "q{N}_{type}" e.g., "q1_kv_cache", "q2_context"
    static std::regex pattern(R"(q(\d+)_(.+))");
    std::smatch match;

    if (std::regex_match(label, match, pattern)) {
      query_id = std::stoull(match[1].str());
      flow_type = match[2].str();
    } else {
      // Fallback: assign to query 0, use full label as type
      query_id = 0;
      flow_type = label;
      RTC_LOG(LS_WARNING) << "[COMPUTE-EMULATION] Non-standard label format: "
                          << label << " (expected q{N}_{type})";
    }
  }

  size_t DeriveContextTokens(const std::string& flow_type, double P_hat_ms) {
    // KV cache flows have no compute (already pre-computed)
    if (flow_type.find("kv") != std::string::npos) {
      return 0;  // KV cache - no prefill needed
    }

    // For context/lora flows, derive tokens from P_hat_ms
    // Using: T_prefill = tokens / rate * 1000ms
    // => tokens = T_prefill * rate / 1000
    if (P_hat_ms > 0.0) {
      size_t tokens = static_cast<size_t>(
          P_hat_ms * gpu_config_.prefill_tokens_per_sec / 1000.0);
      return tokens > 0 ? tokens : 100;  // Minimum 100 tokens
    }

    // Default values based on flow type
    if (flow_type.find("context") != std::string::npos) {
      return 1000;  // Context typically has more tokens
    }
    if (flow_type.find("lora") != std::string::npos) {
      return 500;   // LoRA weights need some compute
    }

    return 100;  // Default
  }

  void EnsureQueryConfig(uint64_t query_id, const FlowConfig& flow) {
    auto& config = query_configs_[query_id];
    config.query_id = query_id;

    // Add flow spec
    FlowSpec spec;
    spec.stream_id = flow.stream_id;
    spec.label = flow.flow_type;
    spec.total_bytes = flow.total_bytes;
    spec.context_tokens = flow.context_tokens;
    spec.decode_tokens = 0;  // Only final decode has tokens

    config.flows.push_back(spec);

    // Default final decode tokens (256 output tokens at 10 tok/s = 25.6s)
    // Use a smaller value for testing
    config.final_decode_tokens = 256;
  }

  void CreateQuery(uint64_t query_id, int64_t start_time_ms = 0) {
    auto it = query_configs_.find(query_id);
    if (it == query_configs_.end()) {
      RTC_LOG(LS_WARNING) << "[COMPUTE-EMULATION] No config for query " << query_id;
      return;
    }

    // Create query in tracker with optional custom start time
    if (query_tracker_) {
      uint64_t actual_id = query_tracker_->CreateQuery(it->second, start_time_ms);

      // Store mapping from our query_id to tracker's actual_id
      active_queries_[query_id] = true;
      query_id_to_tracker_id_[query_id] = actual_id;

      // Mark as sent immediately (we're on receiver side)
      query_tracker_->OnQuerySent(actual_id);

      RTC_LOG(LS_VERBOSE) << "[COMPUTE-EMULATION] Created query_id=" << query_id
                          << " -> tracker_id=" << actual_id
                          << " start_time=" << start_time_ms;
    }
  }

  static int64_t GetCurrentTimeMs() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
  }

  // Pending flow data for real inference
  struct PendingFlowData {
    std::vector<uint8_t> kvcache;
    std::string text;
    std::string data_format;  // Original format: "kv_cache", "kvzip", "raw_text"
  };

  // Configuration
  std::string log_dir_;
  GpuConfig gpu_config_;
  bool initialized_;

  // Flow tracking
  std::map<std::string, FlowConfig> flows_;           // label -> config
  std::map<int, std::string> stream_to_label_;        // stream_id -> label
  std::map<uint64_t, QueryConfig> query_configs_;     // query_id -> config
  std::map<uint64_t, bool> active_queries_;           // active query tracking (by our query_id)
  std::map<uint64_t, uint64_t> query_id_to_tracker_id_;  // our query_id -> tracker's actual_id
  std::map<std::string, PendingFlowData> pending_data_;  // label -> pending data

  // Core components
  std::unique_ptr<ComputeManager> compute_manager_;
  std::unique_ptr<QueryTracker> query_tracker_;
  std::unique_ptr<ComputeMetricsLogger> metrics_logger_;
  std::unique_ptr<FlowTimingLogger> flow_timing_logger_;

  // Metrics
  size_t completed_query_count_ = 0;

  // External callback for query completion
  ExternalQueryCompleteCallback external_query_complete_callback_;
};

}  // namespace coordinator
}  // namespace webrtc

#endif  // PC_COORDINATOR_COMPUTE_EMULATION_INTEGRATION_H_
