/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_COORDINATOR_MAFS_PRIORITY_CALCULATOR_H_
#define PC_COORDINATOR_MAFS_PRIORITY_CALCULATOR_H_

#include <algorithm>
#include <cstdint>
#include <vector>

#include "pc/coordinator/flow_registry.h"
#include "pc/coordinator/uplink_telemetry.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace coordinator {

// MafsPriorityCalculator - Computes MAFS priority scores for flows.
//
// Johnson's Rule priority:
//   Group A (N <= P): rho = MAX_TIME - N  (positive, smaller N first)
//   Group B (N > P):  rho = P - MAX_TIME  (negative, larger P first)

class MafsPriorityCalculator {
 public:
  struct Config {
    double beta = 1.0;
    double min_rate_bps = 100000000.0;  // 100 Mbps
    double default_P_hat_ms = 0.0;
    double default_sigma_P_ms = 0.0;
    bool force_zero_P_hat = false;  // SNF mode
    bool force_equal_priority = false;  // RR mode: all rho=0

    bool IsValid() const {
      return beta >= 0.0 && min_rate_bps > 0.0;
    }
  };

  MafsPriorityCalculator() : config_() {}
  explicit MafsPriorityCalculator(const Config& config) : config_(config) {}

  double CalculateNetworkTime(size_t bytes_remaining, double R_hat_bps) const {
    double rate = std::max(R_hat_bps, config_.min_rate_bps);
    return (bytes_remaining * 8.0 / rate) * 1000.0;
  }

  double CalculateNetworkUncertainty(size_t bytes_remaining,
                                     double R_hat_bps,
                                     double sigma_R) const {
    double rate = std::max(R_hat_bps, config_.min_rate_bps);
    return (bytes_remaining * 8.0 / (rate * rate)) * sigma_R * 1000.0;
  }

  double CalculatePriority(double P_hat_ms,
                           double /* sigma_P_ms */,
                           double N_hat_ms,
                           double /* sigma_N_ms */) const {
    constexpr double MAX_TIME = 1000000.0;

    if (N_hat_ms <= P_hat_ms) {
      return MAX_TIME - N_hat_ms;
    } else {
      return P_hat_ms - MAX_TIME;
    }
  }

  size_t UpdateFlowPriorities(FlowRegistry* registry,
                              const UplinkTelemetry* telemetry,
                              int64_t now_ms) {
    if (!registry || !telemetry) return 0;
    double R_hat_bps = telemetry->GetUplinkRateBps();
    double sigma_R = telemetry->GetSigmaR();
    return UpdateFlowPrioritiesWithRate(registry, R_hat_bps, sigma_R, now_ms);
  }

  size_t UpdateFlowPrioritiesWithRate(FlowRegistry* registry,
                                       double R_hat_bps,
                                       double sigma_R,
                                       int64_t now_ms) {
    if (!registry) return 0;

    std::vector<FlowState*> flows = registry->GetSchedulableFlows();
    if (flows.empty()) return 0;

    // RR mode: all flows get equal priority (rho=0)
    if (config_.force_equal_priority) {
      size_t count = 0;
      for (FlowState* flow : flows) {
        if (!flow) continue;
        registry->SetPriority(flow->flow_id, 0.0, now_ms);
        RTC_LOG(LS_VERBOSE) << "[MAFS-CALC] Flow " << flow->flow_id
                            << " (" << flow->label << "): rho=0 (RR mode)";
        ++count;
      }
      return count;
    }

    size_t updated_count = 0;
    for (FlowState* flow : flows) {
      if (!flow) continue;

      size_t bytes_remaining = flow->BytesRemaining();
      double N_hat_ms = CalculateNetworkTime(bytes_remaining, R_hat_bps);
      double sigma_N_ms = CalculateNetworkUncertainty(bytes_remaining,
                                                       R_hat_bps, sigma_R);

      double P_hat_ms;
      double sigma_P_ms;
      if (config_.force_zero_P_hat) {
        // SNF mode: set P very large so all flows land in Group A
        // (N <= P). Johnson Group A sorts ascending N => SNF behavior.
        P_hat_ms = 1000001.0;
        sigma_P_ms = 0.0;
      } else {
        P_hat_ms = flow->HasComputeTime() ? flow->P_hat_ms
                                          : config_.default_P_hat_ms;
        sigma_P_ms = flow->HasComputeTime() ? flow->sigma_P_ms
                                            : config_.default_sigma_P_ms;
      }

      flow->effective_P_hat_ms = P_hat_ms;

      double rho = CalculatePriority(P_hat_ms, sigma_P_ms, N_hat_ms, sigma_N_ms);

      registry->SetNetworkTime(flow->flow_id, N_hat_ms, sigma_N_ms, now_ms);
      registry->SetPriority(flow->flow_id, rho, now_ms);

      RTC_LOG(LS_VERBOSE) << "[MAFS-CALC] Flow " << flow->flow_id
                          << " (" << flow->label << "): "
                          << "bytes_rem=" << bytes_remaining
                          << ", P_hat=" << P_hat_ms << "ms"
                          << ", N_hat=" << N_hat_ms << "ms"
                          << ", rho=" << rho;
      ++updated_count;
    }
    return updated_count;
  }

  std::vector<FlowState*> GetFlowsByPriority(FlowRegistry* registry) const {
    if (!registry) return {};

    std::vector<FlowState*> flows = registry->GetSchedulableFlows();
    std::sort(flows.begin(), flows.end(),
              [](const FlowState* a, const FlowState* b) {
                return a->rho > b->rho;
              });
    return flows;
  }

  const Config& GetConfig() const { return config_; }
  void SetConfig(const Config& config) { config_ = config; }
  void SetBeta(double beta) { config_.beta = beta; }

 private:
  Config config_;
};

}  // namespace coordinator
}  // namespace webrtc

#endif  // PC_COORDINATOR_MAFS_PRIORITY_CALCULATOR_H_
