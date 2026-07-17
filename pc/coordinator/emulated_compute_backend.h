/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_COORDINATOR_EMULATED_COMPUTE_BACKEND_H_
#define PC_COORDINATOR_EMULATED_COMPUTE_BACKEND_H_

#include <chrono>
#include <ratio>
#include <cstddef>
#include <string>
#include <thread>

#include "pc/coordinator/compute_backend.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace coordinator {

// ============================================================================
// GpuConfig - Configuration for GPU compute capabilities
//
// Supports two modes:
// 1. Legacy fixed-rate mode: prefill_tokens_per_sec, decode_tokens_per_sec
// 2. Regression mode: context-aware rates from measured data
//
// Regression formulas (from measured Llama3-8B-8bit data):
//   prefill_ms = prefill_a * context_tokens + prefill_b * context_tokens^2
//   decode_ms_per_token = decode_base + decode_slope * context_tokens
//
// Default values are for Jetson Nano Orin (legacy fixed mode):
// - Prefill: 400 tokens/second
// - Decode: 10 tokens/second
// ============================================================================

struct GpuConfig {
  // ----- Legacy fixed-rate mode -----
  // Prefill throughput (tokens per second)
  double prefill_tokens_per_sec = 400.0;

  // Decode throughput (tokens per second)
  double decode_tokens_per_sec = 10.0;

  // ----- Regression mode (context-aware) -----
  // Enable regression mode (uses prefill_a/b and decode_base/slope)
  bool use_regression = false;

  // Prefill regression: prefill_ms = prefill_a * ctx + prefill_b * ctx^2
  // Derived from Llama3-8B-8bit: prefill_a=2.3825, prefill_b=0.00004731
  double prefill_a = 2.3825;       // Linear coefficient (ms per token)
  double prefill_b = 0.00004731;   // Quadratic coefficient (ms per token^2)

  // Decode regression: decode_ms_per_token = decode_base + decode_slope * ctx
  // Derived from Llama3-8B-8bit: decode_base=118.82, decode_slope=0.00194
  double decode_base = 118.82;     // Base decode time per token (ms)
  double decode_slope = 0.00194;   // Additional decode time per context token (ms)

  // ----- KV cache configuration -----
  // KV cache bytes per token (Llama3-8B: 128KB = 131072 bytes)
  size_t kv_bytes_per_token = 131072;

  // ----- Metadata -----
  // Device name for logging
  std::string device_name = "jetson_nano_orin";

  // Validate configuration
  bool IsValid() const {
    if (use_regression) {
      return prefill_a > 0.0 || prefill_b > 0.0;
    }
    return prefill_tokens_per_sec > 0.0 && decode_tokens_per_sec > 0.0;
  }

  // Helper: Calculate prefill time for given context length
  double CalculatePrefillMs(size_t context_tokens) const {
    if (context_tokens == 0) return 0.0;

    if (use_regression) {
      // Regression: prefill_ms = a * ctx + b * ctx^2
      double ctx = static_cast<double>(context_tokens);
      return prefill_a * ctx + prefill_b * ctx * ctx;
    } else {
      // Legacy: prefill_ms = ctx / rate * 1000
      return (static_cast<double>(context_tokens) / prefill_tokens_per_sec) * 1000.0;
    }
  }

  // Helper: Calculate decode time per token for given context length
  double CalculateDecodeMs(size_t context_tokens, size_t decode_tokens) const {
    if (decode_tokens == 0) return 0.0;

    if (use_regression) {
      // Regression: decode_ms_per_token = base + slope * ctx
      double ctx = static_cast<double>(context_tokens);
      double ms_per_token = decode_base + decode_slope * ctx;
      return ms_per_token * static_cast<double>(decode_tokens);
    } else {
      // Legacy: decode_ms = tokens / rate * 1000
      return (static_cast<double>(decode_tokens) / decode_tokens_per_sec) * 1000.0;
    }
  }

  // Helper: Calculate KV cache size for given context length
  size_t CalculateKvCacheBytes(size_t context_tokens) const {
    return context_tokens * kv_bytes_per_token;
  }

  // Initialize from environment variables (called from conductor.cc)
  static GpuConfig FromEnvironment() {
    GpuConfig config;

    // Check for regression mode environment variables
    const char* prefill_a_env = std::getenv("COMPUTE_PREFILL_A");
    const char* prefill_b_env = std::getenv("COMPUTE_PREFILL_B");
    const char* decode_base_env = std::getenv("COMPUTE_DECODE_BASE");
    const char* decode_slope_env = std::getenv("COMPUTE_DECODE_SLOPE");

    if (prefill_a_env || prefill_b_env) {
      // Use regression mode
      config.use_regression = true;
      if (prefill_a_env) config.prefill_a = std::stod(prefill_a_env);
      if (prefill_b_env) config.prefill_b = std::stod(prefill_b_env);
      if (decode_base_env) config.decode_base = std::stod(decode_base_env);
      if (decode_slope_env) config.decode_slope = std::stod(decode_slope_env);
    } else {
      // Legacy fixed-rate mode
      const char* prefill_rate = std::getenv("COMPUTE_PREFILL_RATE");
      const char* decode_rate = std::getenv("COMPUTE_DECODE_RATE");
      if (prefill_rate) config.prefill_tokens_per_sec = std::stod(prefill_rate);
      if (decode_rate) config.decode_tokens_per_sec = std::stod(decode_rate);
    }

    // KV cache bytes per token
    const char* kv_bytes_env = std::getenv("COMPUTE_KV_BYTES_PER_TOKEN");
    if (kv_bytes_env) config.kv_bytes_per_token = std::stoull(kv_bytes_env);

    // GPU model name
    const char* gpu_model_env = std::getenv("COMPUTE_GPU_MODEL");
    if (gpu_model_env) config.device_name = gpu_model_env;

    return config;
  }
};

// ============================================================================
// EmulatedComputeBackend - Timing-based GPU compute emulation
//
// This backend simulates GPU inference by sleeping for the calculated
// duration based on token counts and throughput configuration.
//
// Supports two modes:
// 1. Legacy fixed-rate: T_prefill = ctx / rate × 1000 (ms)
// 2. Regression mode: T_prefill = a × ctx + b × ctx² (ms)
//
// Special case:
// - KV cache flows have context_tokens=0, so T_prefill=0 (already computed)
// ============================================================================

class EmulatedComputeBackend : public ComputeBackend {
 public:
  // Default constructor with Jetson Nano Orin config
  EmulatedComputeBackend() : config_(), last_context_tokens_(0) {}

  // Constructor with custom config
  explicit EmulatedComputeBackend(const GpuConfig& config)
      : config_(config), last_context_tokens_(0) {}

  // -------------------------------------------------------------------------
  // Estimation methods (instant, no blocking)
  // -------------------------------------------------------------------------

  double EstimatePrefillMs(size_t context_tokens) const override {
    return config_.CalculatePrefillMs(context_tokens);
  }

  double EstimateDecodeMs(size_t decode_tokens) const override {
    // Use last_context_tokens for context-aware decode estimation
    return config_.CalculateDecodeMs(last_context_tokens_, decode_tokens);
  }

  // Context-aware decode estimation (preferred method)
  double EstimateDecodeMsWithContext(size_t context_tokens, size_t decode_tokens) const {
    return config_.CalculateDecodeMs(context_tokens, decode_tokens);
  }

  // -------------------------------------------------------------------------
  // Execution methods (blocking - sleeps for duration)
  // -------------------------------------------------------------------------

  double Prefill(size_t context_tokens) override {
    // Store context for subsequent decode calls
    last_context_tokens_ = context_tokens;

    double duration_ms = EstimatePrefillMs(context_tokens);

    if (duration_ms > 0) {
      RTC_LOG(LS_VERBOSE) << "[EMULATED-COMPUTE] Prefill: "
                          << context_tokens << " tokens"
                          << (config_.use_regression ? " (regression)" : " (fixed-rate)")
                          << ", sleeping " << duration_ms << " ms";

      auto start = std::chrono::high_resolution_clock::now();
      std::this_thread::sleep_for(
          std::chrono::milliseconds(static_cast<int64_t>(duration_ms)));
      auto end = std::chrono::high_resolution_clock::now();

      double actual_ms = std::chrono::duration<double, std::milli>(end - start).count();
      return actual_ms;
    }

    return 0.0;
  }

  double Decode(size_t decode_tokens) override {
    double duration_ms = EstimateDecodeMs(decode_tokens);

    if (duration_ms > 0) {
      RTC_LOG(LS_VERBOSE) << "[EMULATED-COMPUTE] Decode: "
                          << decode_tokens << " tokens"
                          << " (ctx=" << last_context_tokens_ << ")"
                          << (config_.use_regression ? " (regression)" : " (fixed-rate)")
                          << ", sleeping " << duration_ms << " ms";

      auto start = std::chrono::high_resolution_clock::now();
      std::this_thread::sleep_for(
          std::chrono::milliseconds(static_cast<int64_t>(duration_ms)));
      auto end = std::chrono::high_resolution_clock::now();

      double actual_ms = std::chrono::duration<double, std::milli>(end - start).count();
      return actual_ms;
    }

    return 0.0;
  }

  // Context-aware decode execution (preferred method)
  double DecodeWithContext(size_t context_tokens, size_t decode_tokens) {
    last_context_tokens_ = context_tokens;
    return Decode(decode_tokens);
  }

  // -------------------------------------------------------------------------
  // Metadata
  // -------------------------------------------------------------------------

  std::string GetName() const override {
    std::string mode = config_.use_regression ? "regression" : "fixed-rate";
    return "EmulatedComputeBackend(" + config_.device_name + ", " + mode + ")";
  }

  // -------------------------------------------------------------------------
  // Configuration access
  // -------------------------------------------------------------------------

  const GpuConfig& GetConfig() const { return config_; }

  void SetConfig(const GpuConfig& config) { config_ = config; }

  void SetPrefillTokensPerSec(double tokens_per_sec) {
    config_.prefill_tokens_per_sec = tokens_per_sec;
    config_.use_regression = false;  // Switch to legacy mode
  }

  void SetDecodeTokensPerSec(double tokens_per_sec) {
    config_.decode_tokens_per_sec = tokens_per_sec;
    config_.use_regression = false;  // Switch to legacy mode
  }

  // Set context for subsequent decode calls
  void SetLastContextTokens(size_t context_tokens) {
    last_context_tokens_ = context_tokens;
  }

  size_t GetLastContextTokens() const { return last_context_tokens_; }

 private:
  GpuConfig config_;
  size_t last_context_tokens_;  // Context length from last prefill (for decode estimation)
};

}  // namespace coordinator
}  // namespace webrtc

#endif  // PC_COORDINATOR_EMULATED_COMPUTE_BACKEND_H_
