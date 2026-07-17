/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_COORDINATOR_STRICT_PRIORITY_MAPPER_H_
#define PC_COORDINATOR_STRICT_PRIORITY_MAPPER_H_

#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#include "pc/coordinator/flow_registry.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace coordinator {

// StrictPriorityMapper - Maps MAFS rho rankings to dcSCTP stream priorities.
// Uses exponential gaps by default for true strict priority in WFQ.
class StrictPriorityMapper {
 public:
  struct Config {
    uint16_t priority_max = 65535;
    uint16_t priority_min = 1;
    uint16_t priority_spacing = 1000;
    bool use_exponential_gaps = true;

    bool IsValid() const {
      if (use_exponential_gaps) return priority_max > priority_min;
      return priority_max > priority_min && priority_spacing > 0;
    }
  };

  StrictPriorityMapper() : config_() {}
  explicit StrictPriorityMapper(const Config& config) : config_(config) {}

  uint16_t PriorityForRank(size_t rank) const {
    if (config_.use_exponential_gaps) {
      switch (rank) {
        case 0: return 65535;
        case 1: return 100;
        case 2: return 10;
        case 3: return 5;
        case 4: return 4;
        case 5: return 3;
        case 6: return 2;
        default: return 1;
      }
    }

    uint32_t priority = config_.priority_max;
    uint32_t decrement = static_cast<uint32_t>(rank) * config_.priority_spacing;

    if (decrement >= priority) return config_.priority_min;

    uint32_t result = priority - decrement;
    if (result < config_.priority_min) return config_.priority_min;

    return static_cast<uint16_t>(result);
  }

  std::map<int, uint16_t> MapFlowsToPriorities(
      const std::vector<FlowState*>& sorted_flows) const {
    std::map<int, uint16_t> result;
    size_t rank = 0;
    double prev_rho = 0.0;
    bool first_valid = true;
    for (size_t i = 0; i < sorted_flows.size(); ++i) {
      if (!sorted_flows[i]) continue;
      // Only advance rank when rho differs (ties get same priority)
      if (!first_valid && sorted_flows[i]->rho != prev_rho) {
        rank++;
      }
      prev_rho = sorted_flows[i]->rho;
      first_valid = false;

      int stream_id = sorted_flows[i]->stream_id;
      uint16_t priority = PriorityForRank(rank);
      result[stream_id] = priority;

      RTC_LOG(LS_INFO) << "[PRIORITY-MAP] Flow " << sorted_flows[i]->flow_id
                       << " (stream=" << stream_id
                       << ", rho=" << sorted_flows[i]->rho
                       << ") -> priority=" << priority
                       << " (rank=" << rank << ")";
    }
    return result;
  }

  struct PriorityMapping {
    uint32_t flow_id;
    int stream_id;
    double rho;
    size_t rank;
    uint16_t priority;
    double P_hat_ms;
    double N_hat_ms;
    double sigma_P_ms;
    double sigma_N_ms;
    size_t total_bytes;
    size_t bytes_remaining;
  };

  std::vector<PriorityMapping> GetPriorityMappings(
      const std::vector<FlowState*>& sorted_flows) const {
    std::vector<PriorityMapping> result;
    result.reserve(sorted_flows.size());
    size_t rank = 0;
    double prev_rho = 0.0;
    bool first_valid = true;
    for (size_t i = 0; i < sorted_flows.size(); ++i) {
      if (!sorted_flows[i]) continue;
      if (!first_valid && sorted_flows[i]->rho != prev_rho) {
        rank++;
      }
      prev_rho = sorted_flows[i]->rho;
      first_valid = false;

      PriorityMapping mapping;
      mapping.flow_id = sorted_flows[i]->flow_id;
      mapping.stream_id = sorted_flows[i]->stream_id;
      mapping.rho = sorted_flows[i]->rho;
      mapping.rank = rank;
      mapping.priority = PriorityForRank(rank);
      mapping.P_hat_ms = sorted_flows[i]->effective_P_hat_ms;
      mapping.N_hat_ms = sorted_flows[i]->N_hat_ms;
      mapping.sigma_P_ms = sorted_flows[i]->sigma_P_ms;
      mapping.sigma_N_ms = sorted_flows[i]->sigma_N_ms;
      mapping.total_bytes = sorted_flows[i]->total_bytes;
      mapping.bytes_remaining = sorted_flows[i]->BytesRemaining();
      result.push_back(mapping);
    }
    return result;
  }

  size_t MaxDistinctPriorities() const {
    if (config_.use_exponential_gaps) return 16;
    if (config_.priority_spacing == 0) return 1;
    uint32_t range = config_.priority_max - config_.priority_min;
    return (range / config_.priority_spacing) + 1;
  }

  const Config& GetConfig() const { return config_; }
  void SetConfig(const Config& config) { config_ = config; }

 private:
  Config config_;
};

}  // namespace coordinator
}  // namespace webrtc

#endif  // PC_COORDINATOR_STRICT_PRIORITY_MAPPER_H_
