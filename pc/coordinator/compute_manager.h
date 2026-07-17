/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_COORDINATOR_COMPUTE_MANAGER_H_
#define PC_COORDINATOR_COMPUTE_MANAGER_H_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "pc/coordinator/compute_backend.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace coordinator {

// ============================================================================
// ComputeTask - Specification for a compute task
// ============================================================================

struct ComputeTask {
  uint64_t query_id = 0;
  uint32_t flow_id = 0;
  std::string label;           // "kv", "lora", "context", "final"

  // Compute specification
  size_t context_tokens = 0;   // Tokens to prefill (0 for KV cache)
  size_t decode_tokens = 0;    // Tokens to decode (0 for intermediate flows)

  // Optional data payloads (for real inference backends)
  std::string data_format;                // "kv_cache", "raw_text", "" (emulation)
  std::vector<uint8_t> kvcache_buffer;    // KV cache binary (optional)
  std::string text_content;               // Raw text (optional)

  // Aggregate prompt (set by QueryTracker for final decode)
  std::string aggregate_prompt;

  // Output from decode (filled by ComputeManager after execution)
  std::string decode_output;

  // Timing information (filled by ComputeManager)
  int64_t queued_time_ms = 0;       // When task was queued
  int64_t compute_start_ms = 0;     // When compute actually started
  int64_t prefill_done_ms = 0;      // When prefill completed
  int64_t compute_done_ms = 0;      // When all compute completed

  // Actual durations (filled after execution)
  double actual_prefill_ms = 0.0;
  double actual_decode_ms = 0.0;

  // Overlapped prefill: front portion prefilled in parallel with network transfer
  double parallel_prefill_ms = 0.0;    // Time for front-token prefill (0 = disabled)
  int64_t transfer_start_ms = 0;        // When network transfer started (first byte)
  size_t front_context_tokens = 0;      // Number of front tokens to prefill locally
  std::string front_text_content;       // Raw text for front tokens (real GPU backend)
};

// Callback invoked when a compute task completes
using ComputeCallback = std::function<void(const ComputeTask& completed_task)>;

// ============================================================================
// ComputeManager - GPU queue and scheduling orchestrator
//
// This class manages the compute task queue and utilization tracking.
// It owns a ComputeBackend instance and processes tasks sequentially
// (single GPU model - no parallel compute).
//
// Key responsibilities:
// - Schedule tasks with ScheduleTask()
// - Provide P̂_k estimates with EstimateTotalComputeMs()
// - Track utilization metrics
// - Process tasks in FIFO order via worker thread
// ============================================================================

class ComputeManager {
 public:
  // Constructor with backend
  explicit ComputeManager(std::unique_ptr<ComputeBackend> backend);

  // Destructor - stops worker thread
  ~ComputeManager();

  // Non-copyable, non-movable
  ComputeManager(const ComputeManager&) = delete;
  ComputeManager& operator=(const ComputeManager&) = delete;

  // -------------------------------------------------------------------------
  // Task scheduling
  // -------------------------------------------------------------------------

  // Schedule a compute task
  // Task enters queue, executes when GPU is free
  // on_complete is called on worker thread when task finishes
  void ScheduleTask(ComputeTask task, ComputeCallback on_complete);

  // -------------------------------------------------------------------------
  // Estimation (for HAFS P̂_k calculation)
  // -------------------------------------------------------------------------

  // Estimate total compute time without blocking
  double EstimateTotalComputeMs(size_t context_tokens, size_t decode_tokens) const;

  // Estimate prefill time
  double EstimatePrefillMs(size_t context_tokens) const;

  // Estimate decode time
  double EstimateDecodeMs(size_t decode_tokens) const;

  // -------------------------------------------------------------------------
  // State queries
  // -------------------------------------------------------------------------

  // Check if GPU is currently busy
  bool IsBusy() const;

  // Get number of pending tasks in queue
  size_t GetQueueDepth() const;

  // Get total tasks completed
  size_t GetCompletedTaskCount() const;

  // -------------------------------------------------------------------------
  // Utilization tracking
  // -------------------------------------------------------------------------

  // Get current utilization = compute_time / (compute_time + idle_time)
  // Returns value between 0.0 and 1.0
  double GetUtilization() const;

  // Get total compute time in milliseconds
  int64_t GetTotalComputeTimeMs() const;

  // Get total idle time in milliseconds
  int64_t GetTotalIdleTimeMs() const;

  // Reset utilization tracking
  void ResetUtilization();

  // -------------------------------------------------------------------------
  // Backend access
  // -------------------------------------------------------------------------

  const ComputeBackend* GetBackend() const { return backend_.get(); }

 private:
  // Worker thread function
  void WorkerLoop();

  // Process a single task
  void ProcessTask(ComputeTask& task, ComputeCallback callback);

  // Get current time in milliseconds
  static int64_t GetCurrentTimeMs();

  // Backend
  std::unique_ptr<ComputeBackend> backend_;

  // Task queue
  std::queue<std::pair<ComputeTask, ComputeCallback>> task_queue_;

  // Worker thread
  std::thread worker_thread_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool shutdown_ = false;

  // State tracking
  bool is_busy_ = false;
  size_t completed_count_ = 0;

  // Utilization tracking
  int64_t tracking_start_ms_ = 0;
  int64_t total_compute_ms_ = 0;
  int64_t total_idle_ms_ = 0;
  int64_t last_task_end_ms_ = 0;
  bool tracking_initialized_ = false;
};

// ============================================================================
// Implementation
// ============================================================================

inline ComputeManager::ComputeManager(std::unique_ptr<ComputeBackend> backend)
    : backend_(std::move(backend)) {
  tracking_start_ms_ = GetCurrentTimeMs();
  last_task_end_ms_ = tracking_start_ms_;
  tracking_initialized_ = true;

  // Start worker thread
  worker_thread_ = std::thread(&ComputeManager::WorkerLoop, this);

  RTC_LOG(LS_INFO) << "[COMPUTE-MANAGER] Started with backend: "
                   << (backend_ ? backend_->GetName() : "null");
}

inline ComputeManager::~ComputeManager() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
  }
  cv_.notify_all();

  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }

  RTC_LOG(LS_INFO) << "[COMPUTE-MANAGER] Shutdown. Completed tasks: "
                   << completed_count_
                   << ", Utilization: " << (GetUtilization() * 100) << "%";
}

inline void ComputeManager::ScheduleTask(ComputeTask task, ComputeCallback on_complete) {
  task.queued_time_ms = GetCurrentTimeMs();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    task_queue_.push({task, std::move(on_complete)});

    fprintf(stderr, "[COMPUTE-TASK] Queued: query=%lu flow=%u label=%s "
            "ctx_tokens=%zu dec_tokens=%zu format=%s queue_depth=%zu\n",
            (unsigned long)task.query_id, task.flow_id, task.label.c_str(),
            task.context_tokens, task.decode_tokens, task.data_format.c_str(),
            task_queue_.size());
  }

  cv_.notify_one();
}

inline double ComputeManager::EstimateTotalComputeMs(
    size_t context_tokens, size_t decode_tokens) const {
  if (!backend_) return 0.0;
  return backend_->EstimateTotalMs(context_tokens, decode_tokens);
}

inline double ComputeManager::EstimatePrefillMs(size_t context_tokens) const {
  if (!backend_) return 0.0;
  return backend_->EstimatePrefillMs(context_tokens);
}

inline double ComputeManager::EstimateDecodeMs(size_t decode_tokens) const {
  if (!backend_) return 0.0;
  return backend_->EstimateDecodeMs(decode_tokens);
}

inline bool ComputeManager::IsBusy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return is_busy_;
}

inline size_t ComputeManager::GetQueueDepth() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return task_queue_.size();
}

inline size_t ComputeManager::GetCompletedTaskCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return completed_count_;
}

inline double ComputeManager::GetUtilization() const {
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t total = total_compute_ms_ + total_idle_ms_;
  if (total == 0) return 0.0;
  return static_cast<double>(total_compute_ms_) / static_cast<double>(total);
}

inline int64_t ComputeManager::GetTotalComputeTimeMs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return total_compute_ms_;
}

inline int64_t ComputeManager::GetTotalIdleTimeMs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return total_idle_ms_;
}

inline void ComputeManager::ResetUtilization() {
  std::lock_guard<std::mutex> lock(mutex_);
  tracking_start_ms_ = GetCurrentTimeMs();
  last_task_end_ms_ = tracking_start_ms_;
  total_compute_ms_ = 0;
  total_idle_ms_ = 0;
}

inline void ComputeManager::WorkerLoop() {
  while (true) {
    std::pair<ComputeTask, ComputeCallback> task_pair;

    {
      std::unique_lock<std::mutex> lock(mutex_);

      // Wait for task or shutdown
      cv_.wait(lock, [this] {
        return shutdown_ || !task_queue_.empty();
      });

      if (shutdown_ && task_queue_.empty()) {
        break;
      }

      if (task_queue_.empty()) {
        continue;
      }

      // Get next task
      task_pair = std::move(task_queue_.front());
      task_queue_.pop();
      is_busy_ = true;
    }

    // Process task (outside lock)
    ProcessTask(task_pair.first, task_pair.second);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      is_busy_ = false;
      completed_count_++;
    }
  }
}

inline void ComputeManager::ProcessTask(ComputeTask& task, ComputeCallback callback) {
  int64_t now = GetCurrentTimeMs();

  // Track idle time since last task
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (tracking_initialized_ && last_task_end_ms_ > 0) {
      int64_t idle_time = now - last_task_end_ms_;
      if (idle_time > 0) {
        total_idle_ms_ += idle_time;
      }
    }
  }

  task.compute_start_ms = now;

  fprintf(stderr, "[COMPUTE-START] query=%lu flow=%u label=%s "
          "queue_wait=%lldms ctx_tokens=%zu dec_tokens=%zu format=%s "
          "kvcache=%zuB text=%zuB\n",
          (unsigned long)task.query_id, task.flow_id, task.label.c_str(),
          (long long)(now - task.queued_time_ms),
          task.context_tokens, task.decode_tokens, task.data_format.c_str(),
          task.kvcache_buffer.size(), task.text_content.size());

  // Pass data to backend before execution
  bool has_kvcache = !task.kvcache_buffer.empty();
  bool has_text = !task.text_content.empty();

  if (has_kvcache && backend_) {
    backend_->SetKVCacheBuffer(std::move(task.kvcache_buffer));
  }
  if (has_text && backend_) {
    backend_->SetTextContent(task.text_content);
  }

  // Pass aggregate prompt to backend if present (final decode path)
  if (!task.aggregate_prompt.empty() && backend_) {
    backend_->SetAggregatePrompt(task.aggregate_prompt);
  }

  // Overlapped prefill: receiver prefilled front tokens during transfer.
  // Wait only for the remaining portion not yet done when transfer ended.
  if (task.parallel_prefill_ms > 0.0 && task.transfer_start_ms > 0) {
    double elapsed_ms = static_cast<double>(now - task.transfer_start_ms);
    double remaining_ms = task.parallel_prefill_ms - elapsed_ms;

    // For real GPU backends: perform actual front token prefill if text available.
    // For emulation: this is a timing-only wait (no real GPU work).
    if (!task.front_text_content.empty() && task.front_context_tokens > 0 && backend_) {
      fprintf(stderr, "[COMPUTE-OVERLAP] Real backend: front prefill %zu tokens\n",
              task.front_context_tokens);
      backend_->SetTextContent(task.front_text_content);
      task.actual_prefill_ms = backend_->Prefill(task.front_context_tokens);
      remaining_ms = 0.0;  // GPU did the work; no additional wait needed
    } else if (remaining_ms > 0.0) {
      fprintf(stderr, "[COMPUTE-OVERLAP] Waiting for front prefill: total=%.0fms "
              "elapsed=%.0fms remaining=%.0fms\n",
              task.parallel_prefill_ms, elapsed_ms, remaining_ms);
      std::this_thread::sleep_for(
          std::chrono::milliseconds(static_cast<int64_t>(remaining_ms)));
      task.actual_prefill_ms = remaining_ms;
    } else {
      fprintf(stderr, "[COMPUTE-OVERLAP] Front prefill already done: total=%.0fms "
              "elapsed=%.0fms (0 wait)\n",
              task.parallel_prefill_ms, elapsed_ms);
      task.actual_prefill_ms = 0.0;
    }
    // Front tokens handled via overlap — skip normal prefill path
  } else {
    // Execute prefill: skip token-based prefill for kv_cache (already prefilled)
    // For kv_cache: pass 0 tokens (EmulatedBackend returns 0ms, LlamaCppBackend restores KV state)
    // For raw_text/other: pass actual context_tokens for full prefill
    size_t prefill_tokens = (task.data_format == "kv_cache" || task.data_format == "kvzip") ? 0 : task.context_tokens;
    if ((prefill_tokens > 0 || has_kvcache || has_text) && backend_) {
      task.actual_prefill_ms = backend_->Prefill(prefill_tokens);
    }
  }
  task.prefill_done_ms = GetCurrentTimeMs();

  // Execute decode
  if (task.decode_tokens > 0 && backend_) {
    task.actual_decode_ms = backend_->Decode(task.decode_tokens);
  }
  task.compute_done_ms = GetCurrentTimeMs();

  // Capture decode output for multi-context aggregation
  if (backend_) {
    task.decode_output = backend_->GetLastDecodeOutput();
  }

  // Track compute time
  {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t compute_time = task.compute_done_ms - task.compute_start_ms;
    total_compute_ms_ += compute_time;
    last_task_end_ms_ = task.compute_done_ms;
  }

  fprintf(stderr, "[COMPUTE-DONE] query=%lu flow=%u label=%s "
          "prefill=%.1fms decode=%.1fms total=%lldms\n",
          (unsigned long)task.query_id, task.flow_id, task.label.c_str(),
          task.actual_prefill_ms, task.actual_decode_ms,
          (long long)(task.compute_done_ms - task.compute_start_ms));

  // Invoke callback
  if (callback) {
    callback(task);
  }
}

inline int64_t ComputeManager::GetCurrentTimeMs() {
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count();
}

}  // namespace coordinator
}  // namespace webrtc

#endif  // PC_COORDINATOR_COMPUTE_MANAGER_H_
