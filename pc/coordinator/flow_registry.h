/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_COORDINATOR_FLOW_REGISTRY_H_
#define PC_COORDINATOR_FLOW_REGISTRY_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "rtc_base/logging.h"
#include "rtc_base/synchronization/mutex.h"

namespace webrtc {
namespace coordinator {

// FlowState - Per-flow bookkeeping for MAFS scheduling.
// Each flow represents one SCTP DataChannel corresponding to one agent context.
struct FlowState {
  uint32_t flow_id = 0;
  int stream_id = -1;
  std::string label;

  size_t total_bytes = 0;
  std::atomic<size_t> bytes_received{0};
  std::atomic<size_t> bytes_sent{0};

  size_t BytesRemaining() const {
    size_t received = bytes_received.load(std::memory_order_relaxed);
    size_t sent = bytes_sent.load(std::memory_order_relaxed);
    size_t progress = std::max(received, sent);
    return (total_bytes > progress) ? (total_bytes - progress) : 0;
  }

  double P_hat_ms = 0.0;
  double sigma_P_ms = 0.0;
  double effective_P_hat_ms = 0.0;

  double N_hat_ms = 0.0;
  double sigma_N_ms = 0.0;

  double rho = 0.0;

  int64_t created_time_ms = 0;
  int64_t last_update_ms = 0;

  bool active = true;
  bool compute_time_set = false;

  FlowState() = default;

  FlowState(uint32_t id, int sid, const std::string& lbl, size_t total,
            int64_t now_ms)
      : flow_id(id),
        stream_id(sid),
        label(lbl),
        total_bytes(total),
        created_time_ms(now_ms),
        last_update_ms(now_ms) {}

  bool IsComplete() const {
    return bytes_received.load(std::memory_order_relaxed) >= total_bytes;
  }

  bool HasComputeTime() const { return compute_time_set; }

  bool IsSchedulable() const {
    return active && !IsComplete() && BytesRemaining() > 0;
  }
};

struct FlowSnapshot {
  uint32_t flow_id = 0;
  int stream_id = -1;
  double rho = 0.0;
  double P_hat_ms = 0.0;
  double N_hat_ms = 0.0;
  size_t total_bytes = 0;
  size_t bytes_remaining = 0;
  bool active = false;

  FlowSnapshot() = default;
  FlowSnapshot(const FlowState& state)
      : flow_id(state.flow_id),
        stream_id(state.stream_id),
        rho(state.rho),
        P_hat_ms(state.P_hat_ms),
        N_hat_ms(state.N_hat_ms),
        total_bytes(state.total_bytes),
        bytes_remaining(state.BytesRemaining()),
        active(state.active) {}
};

// FlowRegistry - Thread-safe registry for flow state management.
class FlowRegistry {
 public:
  FlowRegistry() : next_flow_id_(1) {}

  uint32_t RegisterFlow(int stream_id,
                        const std::string& label,
                        size_t total_bytes,
                        int64_t now_ms) {
    webrtc::MutexLock lock(&mutex_);

    uint32_t flow_id = next_flow_id_++;
    auto flow = std::make_unique<FlowState>(flow_id, stream_id, label,
                                            total_bytes, now_ms);
    stream_to_flow_[stream_id] = flow_id;
    flows_[flow_id] = std::move(flow);

    RTC_LOG(LS_INFO) << "[FLOW-REGISTRY] Registered flow: id=" << flow_id
                     << ", stream=" << stream_id << ", label=" << label
                     << ", total_bytes=" << total_bytes;
    return flow_id;
  }

  void OnFlowDataReceived(int stream_id, size_t bytes) {
    webrtc::MutexLock lock(&mutex_);
    auto it = stream_to_flow_.find(stream_id);
    if (it == stream_to_flow_.end()) return;

    auto flow_it = flows_.find(it->second);
    if (flow_it != flows_.end() && flow_it->second) {
      flow_it->second->bytes_received.fetch_add(bytes, std::memory_order_relaxed);
    }
  }

  void OnFlowDataSent(int stream_id, size_t bytes) {
    webrtc::MutexLock lock(&mutex_);
    auto it = stream_to_flow_.find(stream_id);
    if (it == stream_to_flow_.end()) return;

    auto flow_it = flows_.find(it->second);
    if (flow_it != flows_.end() && flow_it->second) {
      flow_it->second->bytes_sent.fetch_add(bytes, std::memory_order_relaxed);
    }
  }

  bool SetComputeTime(uint32_t flow_id,
                      double P_hat_ms,
                      double sigma_P_ms,
                      int64_t now_ms) {
    webrtc::MutexLock lock(&mutex_);
    auto it = flows_.find(flow_id);
    if (it == flows_.end() || !it->second) return false;

    FlowState* flow = it->second.get();
    flow->P_hat_ms = P_hat_ms;
    flow->sigma_P_ms = sigma_P_ms;
    flow->compute_time_set = true;
    flow->last_update_ms = now_ms;

    RTC_LOG(LS_INFO) << "[FLOW-REGISTRY] Flow " << flow_id
                     << " compute time set: P_hat=" << P_hat_ms
                     << "ms, sigma_P=" << sigma_P_ms << "ms";
    return true;
  }

  bool SetNetworkTime(uint32_t flow_id,
                      double N_hat_ms,
                      double sigma_N_ms,
                      int64_t now_ms) {
    webrtc::MutexLock lock(&mutex_);
    auto it = flows_.find(flow_id);
    if (it == flows_.end() || !it->second) return false;

    FlowState* flow = it->second.get();
    flow->N_hat_ms = N_hat_ms;
    flow->sigma_N_ms = sigma_N_ms;
    flow->last_update_ms = now_ms;
    return true;
  }

  bool SetPriority(uint32_t flow_id, double rho, int64_t now_ms) {
    webrtc::MutexLock lock(&mutex_);
    auto it = flows_.find(flow_id);
    if (it == flows_.end() || !it->second) return false;

    it->second->rho = rho;
    it->second->last_update_ms = now_ms;
    return true;
  }

  void MarkFlowComplete(uint32_t flow_id) {
    webrtc::MutexLock lock(&mutex_);
    auto it = flows_.find(flow_id);
    if (it != flows_.end() && it->second) {
      it->second->active = false;
      RTC_LOG(LS_INFO) << "[FLOW-REGISTRY] Flow " << flow_id
                       << " marked complete";
    }
  }

  void RemoveFlow(uint32_t flow_id) {
    webrtc::MutexLock lock(&mutex_);
    auto it = flows_.find(flow_id);
    if (it != flows_.end() && it->second) {
      stream_to_flow_.erase(it->second->stream_id);
      flows_.erase(it);
    }
  }

  std::vector<FlowState*> GetActiveFlows() {
    webrtc::MutexLock lock(&mutex_);
    std::vector<FlowState*> result;
    for (auto& [id, flow] : flows_) {
      if (flow && flow->active) result.push_back(flow.get());
    }
    return result;
  }

  std::vector<const FlowState*> GetActiveFlows() const {
    webrtc::MutexLock lock(&mutex_);
    std::vector<const FlowState*> result;
    for (const auto& [id, flow] : flows_) {
      if (flow && flow->active) result.push_back(flow.get());
    }
    return result;
  }

  std::vector<FlowSnapshot> GetActiveFlowSnapshots() const {
    webrtc::MutexLock lock(&mutex_);
    std::vector<FlowSnapshot> result;
    for (const auto& [id, flow] : flows_) {
      if (flow && flow->active) result.emplace_back(*flow);
    }
    return result;
  }

  std::vector<FlowState*> GetSchedulableFlows() {
    webrtc::MutexLock lock(&mutex_);
    std::vector<FlowState*> result;
    for (auto& [id, flow] : flows_) {
      if (flow && flow->IsSchedulable()) result.push_back(flow.get());
    }
    return result;
  }

  FlowState* FindByFlowId(uint32_t flow_id) {
    webrtc::MutexLock lock(&mutex_);
    auto it = flows_.find(flow_id);
    return (it != flows_.end() && it->second) ? it->second.get() : nullptr;
  }

  const FlowState* FindByFlowId(uint32_t flow_id) const {
    webrtc::MutexLock lock(&mutex_);
    auto it = flows_.find(flow_id);
    return (it != flows_.end() && it->second) ? it->second.get() : nullptr;
  }

  FlowState* FindByStreamId(int stream_id) {
    webrtc::MutexLock lock(&mutex_);
    auto it = stream_to_flow_.find(stream_id);
    if (it == stream_to_flow_.end()) return nullptr;
    auto flow_it = flows_.find(it->second);
    return (flow_it != flows_.end() && flow_it->second)
               ? flow_it->second.get()
               : nullptr;
  }

  const FlowState* FindByStreamId(int stream_id) const {
    webrtc::MutexLock lock(&mutex_);
    auto it = stream_to_flow_.find(stream_id);
    if (it == stream_to_flow_.end()) return nullptr;
    auto flow_it = flows_.find(it->second);
    return (flow_it != flows_.end() && flow_it->second)
               ? flow_it->second.get()
               : nullptr;
  }

  size_t GetFlowCount() const {
    webrtc::MutexLock lock(&mutex_);
    return flows_.size();
  }

  size_t GetActiveFlowCount() const {
    webrtc::MutexLock lock(&mutex_);
    size_t count = 0;
    for (const auto& [id, flow] : flows_) {
      if (flow && flow->active) ++count;
    }
    return count;
  }

  bool IsRegisteredStream(int stream_id) const {
    webrtc::MutexLock lock(&mutex_);
    return stream_to_flow_.find(stream_id) != stream_to_flow_.end();
  }

  void Clear() {
    webrtc::MutexLock lock(&mutex_);
    flows_.clear();
    stream_to_flow_.clear();
    next_flow_id_ = 1;
  }

 private:
  mutable webrtc::Mutex mutex_;
  std::map<uint32_t, std::unique_ptr<FlowState>> flows_;
  std::map<int, uint32_t> stream_to_flow_;
  uint32_t next_flow_id_;
};

}  // namespace coordinator
}  // namespace webrtc

#endif  // PC_COORDINATOR_FLOW_REGISTRY_H_
