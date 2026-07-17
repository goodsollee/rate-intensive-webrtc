/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_COORDINATOR_MULTI_AGENT_FLOW_COORDINATOR_H_
#define PC_COORDINATOR_MULTI_AGENT_FLOW_COORDINATOR_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pc/coordinator/flow_registry.h"
#include "pc/coordinator/mafs_priority_calculator.h"
#include "pc/coordinator/strict_priority_mapper.h"
#include "pc/coordinator/uplink_telemetry.h"
#include "rtc_base/logging.h"
#include "rtc_base/synchronization/mutex.h"

namespace webrtc {
namespace coordinator {

// MultiAgentFlowCoordinator - Orchestrates MAFS-style flow scheduling.
// Components: UplinkTelemetry, FlowRegistry, MafsPriorityCalculator, StrictPriorityMapper.
class MultiAgentFlowCoordinator {
 public:
  struct Config {
    int64_t update_interval_ms = 500;
    UplinkTelemetry::Config telemetry_config;
    MafsPriorityCalculator::Config calculator_config;
    StrictPriorityMapper::Config mapper_config;
    bool enable_logging = true;
    bool auto_register_flows = false;
    double default_P_hat_ms = 0.0;
    double default_sigma_P_ms = 0.0;

    Config() {
      telemetry_config.sample_interval_ms = 1000;
      telemetry_config.ewma_window_ms = 5000;
      telemetry_config.ewma_alpha = 0.2;
    }

    bool IsValid() const {
      return update_interval_ms > 0 &&
             telemetry_config.IsValid() &&
             calculator_config.IsValid() &&
             mapper_config.IsValid();
    }

    template <typename MafsConfigType>
    static Config FromMafsConfig(const MafsConfigType& mafs) {
      Config config;
      config.update_interval_ms = mafs.update_interval_ms;
      config.telemetry_config.sample_interval_ms = mafs.telemetry_sample_ms;
      config.telemetry_config.ewma_window_ms = mafs.telemetry_window_ms;
      config.telemetry_config.ewma_alpha = mafs.telemetry_alpha;
      config.calculator_config.beta = mafs.beta;
      config.calculator_config.min_rate_bps = mafs.min_rate_bps;
      config.calculator_config.default_P_hat_ms = mafs.default_P_hat_ms;
      config.calculator_config.default_sigma_P_ms = mafs.default_sigma_P_ms;
      config.calculator_config.force_zero_P_hat = mafs.force_zero_P_hat;
      config.calculator_config.force_equal_priority = mafs.force_equal_priority;
      config.mapper_config.priority_max = mafs.priority_max;
      config.mapper_config.priority_min = mafs.priority_min;
      config.mapper_config.priority_spacing = mafs.priority_spacing;
      config.default_P_hat_ms = mafs.default_P_hat_ms;
      config.default_sigma_P_ms = mafs.default_sigma_P_ms;
      config.auto_register_flows = true;
      return config;
    }
  };

  using PriorityUpdateCallback =
      std::function<void(const std::map<int, uint16_t>&)>;

  MultiAgentFlowCoordinator()
      : config_(),
        telemetry_(config_.telemetry_config),
        calculator_(config_.calculator_config),
        mapper_(config_.mapper_config),
        last_update_ms_(0),
        enabled_(true) {}

  explicit MultiAgentFlowCoordinator(const Config& config)
      : config_(config),
        telemetry_(config.telemetry_config),
        calculator_(config.calculator_config),
        mapper_(config.mapper_config),
        last_update_ms_(0),
        enabled_(true) {}

  uint32_t RegisterFlow(int stream_id,
                        const std::string& label,
                        size_t total_bytes,
                        int64_t now_ms) {
    webrtc::MutexLock lock(&mutex_);
    uint32_t flow_id = registry_.RegisterFlow(stream_id, label, total_bytes, now_ms);
    if (config_.enable_logging) {
      RTC_LOG(LS_INFO) << "[MAFC] Registered flow: id=" << flow_id
                       << ", stream=" << stream_id
                       << ", label=" << label
                       << ", total_bytes=" << total_bytes;
    }
    return flow_id;
  }

  void OnDataReceived(int stream_id, size_t bytes, int64_t timestamp_ms) {
    webrtc::MutexLock lock(&mutex_);
    if (!enabled_) return;
    telemetry_.OnBytesReceived(stream_id, bytes, timestamp_ms);

    if (config_.auto_register_flows && stream_id >= 100) {
      FlowState* flow = registry_.FindByStreamId(stream_id);
      if (!flow) {
        std::string label = "auto_" + std::to_string(stream_id);
        size_t default_total = 100 * 1024 * 1024;
        uint32_t flow_id = registry_.RegisterFlow(
            stream_id, label, default_total, timestamp_ms);
        if (config_.default_P_hat_ms > 0 || config_.default_sigma_P_ms > 0) {
          registry_.SetComputeTime(flow_id, config_.default_P_hat_ms,
                                   config_.default_sigma_P_ms, timestamp_ms);
        }
      }
    }

    registry_.OnFlowDataReceived(stream_id, bytes);
  }

  bool OnDataSent(int stream_id, size_t bytes, int64_t timestamp_ms) {
    webrtc::MutexLock lock(&mutex_);
    if (!enabled_) return false;
    telemetry_.OnBytesReceived(stream_id, bytes, timestamp_ms);

    if (config_.auto_register_flows && stream_id >= 100) {
      FlowState* flow = registry_.FindByStreamId(stream_id);
      if (!flow) {
        std::string label = "auto_" + std::to_string(stream_id);
        size_t default_total = 100 * 1024 * 1024;
        uint32_t flow_id = registry_.RegisterFlow(
            stream_id, label, default_total, timestamp_ms);
        if (config_.default_P_hat_ms > 0 || config_.default_sigma_P_ms > 0) {
          registry_.SetComputeTime(flow_id, config_.default_P_hat_ms,
                                   config_.default_sigma_P_ms, timestamp_ms);
        }
      }
    }

    registry_.OnFlowDataSent(stream_id, bytes);

    FlowState* flow = registry_.FindByStreamId(stream_id);
    if (flow && flow->IsComplete() && flow->active) {
      registry_.MarkFlowComplete(flow->flow_id);
      return true;
    }
    return false;
  }

  bool SetComputeTime(uint32_t flow_id,
                      double P_hat_ms,
                      double sigma_P_ms,
                      int64_t now_ms) {
    webrtc::MutexLock lock(&mutex_);
    return registry_.SetComputeTime(flow_id, P_hat_ms, sigma_P_ms, now_ms);
  }

  bool SetComputeTimeByStream(int stream_id,
                              double P_hat_ms,
                              double sigma_P_ms,
                              int64_t now_ms) {
    webrtc::MutexLock lock(&mutex_);
    FlowState* flow = registry_.FindByStreamId(stream_id);
    if (!flow) return false;
    return registry_.SetComputeTime(flow->flow_id, P_hat_ms, sigma_P_ms, now_ms);
  }

  std::map<int, uint16_t> UpdatePriorities(int64_t now_ms) {
    webrtc::MutexLock lock(&mutex_);
    if (!enabled_) return {};
    if (last_update_ms_ > 0 &&
        (now_ms - last_update_ms_) < config_.update_interval_ms) {
      return {};
    }

    last_update_ms_ = now_ms;

    double downlink_rate = downlink_rate_bps_;
    double uplink_rate = telemetry_.GetUplinkRateBps();
    double effective_rate_bps = 0.0;

    if (downlink_rate > 0 && uplink_rate > 0) {
      effective_rate_bps = std::min(downlink_rate, uplink_rate);
    } else if (downlink_rate > 0) {
      effective_rate_bps = downlink_rate;
    } else {
      effective_rate_bps = uplink_rate;
    }

    size_t updated = calculator_.UpdateFlowPrioritiesWithRate(
        &registry_, effective_rate_bps, 0.0, now_ms);

    if (updated == 0) return {};

    auto sorted_flows = calculator_.GetFlowsByPriority(&registry_);
    auto priority_map = mapper_.MapFlowsToPriorities(sorted_flows);

    if (config_.enable_logging && !priority_map.empty()) {
      RTC_LOG(LS_INFO) << "[MAFC] Updated priorities for " << updated
                       << " flows, rate=" << (effective_rate_bps / 1000000.0)
                       << " Mbps";
    }

    if (priority_callback_ && !priority_map.empty()) {
      priority_callback_(priority_map);
    }
    return priority_map;
  }

  std::map<int, uint16_t> ForceUpdatePriorities(int64_t now_ms) {
    webrtc::MutexLock lock(&mutex_);
    last_update_ms_ = now_ms;

    double downlink_rate = downlink_rate_bps_;
    double uplink_rate = telemetry_.GetUplinkRateBps();
    double effective_rate_bps = 0.0;

    if (downlink_rate > 0 && uplink_rate > 0) {
      effective_rate_bps = std::min(downlink_rate, uplink_rate);
    } else if (downlink_rate > 0) {
      effective_rate_bps = downlink_rate;
    } else {
      effective_rate_bps = uplink_rate;
    }

    size_t updated = calculator_.UpdateFlowPrioritiesWithRate(
        &registry_, effective_rate_bps, 0.0, now_ms);
    if (updated == 0) return {};

    auto sorted_flows = calculator_.GetFlowsByPriority(&registry_);
    auto priority_map = mapper_.MapFlowsToPriorities(sorted_flows);

    if (priority_callback_ && !priority_map.empty()) {
      priority_callback_(priority_map);
    }
    return priority_map;
  }

  void MarkFlowComplete(uint32_t flow_id) {
    webrtc::MutexLock lock(&mutex_);
    registry_.MarkFlowComplete(flow_id);
  }

  void RemoveFlow(uint32_t flow_id) {
    webrtc::MutexLock lock(&mutex_);
    registry_.RemoveFlow(flow_id);
  }

  void SetNetworkTimeEstimate(uint32_t flow_id, double N_hat_ms,
                              double sigma_N_ms, int64_t now_ms) {
    webrtc::MutexLock lock(&mutex_);
    registry_.SetNetworkTime(flow_id, N_hat_ms, sigma_N_ms, now_ms);
  }

  void SetPriority(uint32_t flow_id, double rho, int64_t now_ms) {
    webrtc::MutexLock lock(&mutex_);
    registry_.SetPriority(flow_id, rho, now_ms);
  }

  const FlowState* FindFlowById(uint32_t flow_id) const {
    webrtc::MutexLock lock(&mutex_);
    return registry_.FindByFlowId(flow_id);
  }

  void SetPriorityUpdateCallback(PriorityUpdateCallback callback) {
    webrtc::MutexLock lock(&mutex_);
    priority_callback_ = std::move(callback);
  }

  void SetEnabled(bool enabled) {
    webrtc::MutexLock lock(&mutex_);
    enabled_ = enabled;
  }

  bool IsEnabled() const {
    webrtc::MutexLock lock(&mutex_);
    return enabled_;
  }

  double GetUplinkRateBps() const {
    webrtc::MutexLock lock(&mutex_);
    return telemetry_.GetUplinkRateBps();
  }

  double GetSigmaR() const {
    webrtc::MutexLock lock(&mutex_);
    return telemetry_.GetSigmaR();
  }

  void SetDownlinkRate(double rate_bps) {
    webrtc::MutexLock lock(&mutex_);
    downlink_rate_bps_ = rate_bps;
  }

  double GetDownlinkRate() const {
    webrtc::MutexLock lock(&mutex_);
    return downlink_rate_bps_;
  }

  size_t GetFlowCount() const {
    webrtc::MutexLock lock(&mutex_);
    return registry_.GetFlowCount();
  }

  size_t GetActiveFlowCount() const {
    webrtc::MutexLock lock(&mutex_);
    return registry_.GetActiveFlowCount();
  }

  bool IsTelemetryReady() const {
    webrtc::MutexLock lock(&mutex_);
    return telemetry_.IsReady();
  }

  size_t GetTelemetrySampleCount() const {
    webrtc::MutexLock lock(&mutex_);
    return telemetry_.GetSampleCount();
  }

  const FlowState* GetFlowState(uint32_t flow_id) const {
    webrtc::MutexLock lock(&mutex_);
    return registry_.FindByFlowId(flow_id);
  }

  const FlowState* GetFlowStateByStream(int stream_id) const {
    webrtc::MutexLock lock(&mutex_);
    return registry_.FindByStreamId(stream_id);
  }

  std::vector<const FlowState*> GetActiveFlows() const {
    webrtc::MutexLock lock(&mutex_);
    auto flows = registry_.GetActiveFlows();
    std::vector<const FlowState*> result;
    result.reserve(flows.size());
    for (auto* flow : flows) result.push_back(flow);
    return result;
  }

  std::vector<FlowSnapshot> GetActiveFlowSnapshots() const {
    webrtc::MutexLock lock(&mutex_);
    return registry_.GetActiveFlowSnapshots();
  }

  std::vector<StrictPriorityMapper::PriorityMapping> GetCurrentPriorityMappings() const {
    webrtc::MutexLock lock(&mutex_);
    auto sorted_flows = calculator_.GetFlowsByPriority(
        const_cast<FlowRegistry*>(&registry_));
    return mapper_.GetPriorityMappings(sorted_flows);
  }

  const Config& GetConfig() const { return config_; }

  void Clear() {
    webrtc::MutexLock lock(&mutex_);
    registry_.Clear();
    telemetry_.Clear();
    last_update_ms_ = 0;
  }

 private:
  Config config_;
  FlowRegistry registry_;
  UplinkTelemetry telemetry_;
  MafsPriorityCalculator calculator_;
  StrictPriorityMapper mapper_;

  int64_t last_update_ms_;
  bool enabled_;
  double downlink_rate_bps_ = 0.0;

  PriorityUpdateCallback priority_callback_;
  mutable webrtc::Mutex mutex_;
};

}  // namespace coordinator
}  // namespace webrtc

#endif  // PC_COORDINATOR_MULTI_AGENT_FLOW_COORDINATOR_H_
