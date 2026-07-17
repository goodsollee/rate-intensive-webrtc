/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "pc/coordinator/mafs_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>

#include "rtc_base/logging.h"

namespace webrtc {

namespace {

std::string ToLower(const std::string& str) {
  std::string result = str;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

// Read env var with MAFS_* priority, HAFS_* fallback
const char* GetEnvWithFallback(const char* mafs_name, const char* hafs_name) {
  const char* val = std::getenv(mafs_name);
  if (val) return val;
  if (hafs_name) return std::getenv(hafs_name);
  return nullptr;
}

double ReadEnvDoubleOrDefault(const char* name, double default_value,
                              const char* fallback = nullptr) {
  const char* env = GetEnvWithFallback(name, fallback);
  if (env) {
    char* end;
    double val = std::strtod(env, &end);
    if (end != env && *end == '\0') return val;
  }
  return default_value;
}

int64_t ReadEnvInt64OrDefault(const char* name, int64_t default_value,
                              const char* fallback = nullptr) {
  const char* env = GetEnvWithFallback(name, fallback);
  if (env) {
    char* end;
    long long val = std::strtoll(env, &end, 10);
    if (end != env && *end == '\0') return static_cast<int64_t>(val);
  }
  return default_value;
}

}  // namespace

// static
MafsConfig MafsConfig::FromEnvironment() {
  MafsConfig config;

  // MAFS_ENABLE or HAFS_ENABLE=1 to enable
  const char* enable_env = GetEnvWithFallback("MAFS_ENABLE", "HAFS_ENABLE");
  config.enabled = enable_env && (std::string(enable_env) == "1" ||
                                  ToLower(enable_env) == "true");

  // MAFS_MA_PROFILE or HAFS_MA_PROFILE=rr|johnson|snf|static
  const char* profile_env = GetEnvWithFallback("MAFS_MA_PROFILE", "HAFS_MA_PROFILE");
  if (profile_env) {
    config.ma_profile = ParseMaProfile(profile_env);
  }

  // SNF mode: force P_hat=0
  if (config.ma_profile == MaProfile::kSnf) {
    config.force_zero_P_hat = true;
  }

  // RR mode: equal priority for all flows
  if (config.ma_profile == MaProfile::kRoundRobin) {
    config.force_equal_priority = true;
  }
  const char* force_zero_env = GetEnvWithFallback("MAFS_FORCE_ZERO_P_HAT",
                                                   "HAFS_FORCE_ZERO_P_HAT");
  if (force_zero_env && std::string(force_zero_env) == "1") {
    config.force_zero_P_hat = true;
  }

  config.beta = ReadEnvDoubleOrDefault("MAFS_BETA", 1.0, "HAFS_BETA");
  config.default_P_hat_ms = ReadEnvDoubleOrDefault("MAFS_DEFAULT_P_HAT_MS", 0.0,
                                                    "HAFS_DEFAULT_P_HAT_MS");
  config.update_interval_ms = ReadEnvInt64OrDefault("MAFS_UPDATE_INTERVAL_MS", 500,
                                                     "HAFS_UPDATE_INTERVAL_MS");
  config.priority_max = static_cast<uint16_t>(
      ReadEnvInt64OrDefault("MAFS_PRIORITY_MAX", 65535, "HAFS_PRIORITY_MAX"));
  config.telemetry_alpha = ReadEnvDoubleOrDefault("MAFS_TELEMETRY_ALPHA", 0.2,
                                                   "HAFS_TELEMETRY_ALPHA");

  if (config.enabled) {
    RTC_LOG(LS_INFO) << "[MAFS_CONFIG] " << config.ToString();
  }

  return config;
}

// static
MafsConfig MafsConfig::Default() {
  MafsConfig config;
  config.enabled = false;
  config.ma_profile = MaProfile::kRoundRobin;
  return config;
}

std::string MafsConfig::ToString() const {
  std::ostringstream oss;
  oss << "enabled=" << (enabled ? "true" : "false")
      << ", profile=" << MaProfileToString(ma_profile)
      << ", beta=" << beta
      << ", force_zero_P_hat=" << (force_zero_P_hat ? "true" : "false")
      << ", update_interval=" << update_interval_ms << "ms"
      << ", default_P_hat=" << default_P_hat_ms << "ms";
  return oss.str();
}

bool MafsConfig::IsValid() const {
  return beta >= 0.0 && update_interval_ms > 0 && min_rate_bps > 0.0;
}

// static
const char* MafsConfig::MaProfileToString(MaProfile profile) {
  switch (profile) {
    case MaProfile::kRoundRobin: return "rr";
    case MaProfile::kJohnson:    return "johnson";
    case MaProfile::kStatic:     return "static";
    case MaProfile::kSnf:        return "snf";
  }
  return "unknown";
}

// static
MaProfile MafsConfig::ParseMaProfile(const std::string& str) {
  std::string lower = ToLower(str);

  if (lower == "rr" || lower == "roundrobin" || lower == "round-robin" ||
      lower == "none" || lower == "default") {
    return MaProfile::kRoundRobin;
  }
  if (lower == "johnson" || lower == "hafs" || lower == "dynamic" ||
      lower == "compute") {
    return MaProfile::kJohnson;
  }
  if (lower == "static" || lower == "fixed" || lower == "type") {
    return MaProfile::kStatic;
  }
  if (lower == "snf" || lower == "shortestnetwork" ||
      lower == "shortest-network" || lower == "networkfirst") {
    return MaProfile::kSnf;
  }

  RTC_LOG(LS_WARNING) << "[MAFS_CONFIG] Unknown MA profile '" << str
                      << "', defaulting to 'rr'";
  return MaProfile::kRoundRobin;
}

// static
TrafficType MafsConfig::ParseTrafficType(const std::string& label) {
  std::string lower = ToLower(label);

  if (lower.find("raw") != std::string::npos ||
      lower.find("prompt") != std::string::npos ||
      lower.find("input") != std::string::npos) {
    return TrafficType::kRaw;
  }
  if (lower.find("lora") != std::string::npos ||
      lower.find("adapter") != std::string::npos ||
      lower.find("weight") != std::string::npos) {
    return TrafficType::kLora;
  }
  if (lower.find("kv") != std::string::npos ||
      lower.find("cache") != std::string::npos ||
      lower.find("context") != std::string::npos ||
      lower.find("ctx") != std::string::npos) {
    return TrafficType::kKvCache;
  }

  return TrafficType::kUnknown;
}

// static
size_t MafsConfig::GetRankForTrafficType(TrafficType type) {
  switch (type) {
    case TrafficType::kRaw:     return 0;
    case TrafficType::kLora:    return 1;
    case TrafficType::kKvCache: return 2;
    case TrafficType::kUnknown: return 3;
  }
  return 3;
}

}  // namespace webrtc
