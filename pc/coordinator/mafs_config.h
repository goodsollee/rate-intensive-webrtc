/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_COORDINATOR_MAFS_CONFIG_H_
#define PC_COORDINATOR_MAFS_CONFIG_H_

#include <cstdint>
#include <string>

namespace webrtc {

// MA Profile: Multi-Agent Priority Scheduling
enum class MaProfile {
  kRoundRobin,  // Default WFQ, equal priority
  kJohnson,     // Johnson's Rule dynamic priority (rho = P - N)
  kStatic,      // Static priority by traffic type
  kSnf          // Shortest Network First (rho = -N, P forced to 0)
};

// Traffic type for static MA profile
enum class TrafficType {
  kRaw = 0,
  kLora = 1,
  kKvCache = 2,
  kUnknown = 3
};

// MAFS configuration parsed from environment variables
struct MafsConfig {
  bool enabled = false;
  MaProfile ma_profile = MaProfile::kRoundRobin;

  // Priority calculator
  double beta = 1.0;
  double default_P_hat_ms = 0.0;
  double default_sigma_P_ms = 0.0;
  bool force_zero_P_hat = false;
  bool force_equal_priority = false;  // RR mode: all flows get rho=0
  double min_rate_bps = 100000000.0;  // 100 Mbps

  // Priority mapper
  uint16_t priority_max = 65535;
  uint16_t priority_min = 1;
  uint16_t priority_spacing = 1000;

  // Update interval
  int64_t update_interval_ms = 500;

  // Telemetry
  int64_t telemetry_sample_ms = 1000;
  int64_t telemetry_window_ms = 5000;
  double telemetry_alpha = 0.2;

  // Verbose logging
  bool verbose_logging = false;

  // Parse from environment variables (MAFS_ENABLE, MAFS_MA_PROFILE, etc.)
  static MafsConfig FromEnvironment();

  // Create default config
  static MafsConfig Default();

  // Convert to string for logging
  std::string ToString() const;

  // Validate
  bool IsValid() const;

  // Helper: profile to/from string
  static const char* MaProfileToString(MaProfile profile);
  static MaProfile ParseMaProfile(const std::string& str);

  // Helper: parse traffic type from label
  static TrafficType ParseTrafficType(const std::string& label);
  static size_t GetRankForTrafficType(TrafficType type);
};

}  // namespace webrtc

#endif  // PC_COORDINATOR_MAFS_CONFIG_H_
