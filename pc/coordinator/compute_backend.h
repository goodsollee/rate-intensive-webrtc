/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_COORDINATOR_COMPUTE_BACKEND_H_
#define PC_COORDINATOR_COMPUTE_BACKEND_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace webrtc {
namespace coordinator {

// ============================================================================
// ComputeBackend - Abstract interface for compute backends
//
// This interface defines the API contract for compute implementations.
// The primary implementations are:
// - EmulatedComputeBackend: Timing-based emulation (sleeps for duration)
// - LlamaCppBackend: Real GPU inference (future)
//
// Key methods:
// - EstimatePrefillMs(): Get estimated prefill time without executing
// - EstimateDecodeMs(): Get estimated decode time without executing
// - Prefill(): Execute prefill (blocking), return actual time
// - Decode(): Execute decode (blocking), return actual time
//
// All times are in milliseconds.
// ============================================================================

class ComputeBackend {
 public:
  virtual ~ComputeBackend() = default;

  // -------------------------------------------------------------------------
  // Estimation methods (instant, no execution)
  // Used by HAFS to calculate P̂_k without blocking
  // -------------------------------------------------------------------------

  // Estimate prefill time for context processing
  // context_tokens: Number of input tokens to process
  // Returns: Estimated time in milliseconds
  virtual double EstimatePrefillMs(size_t context_tokens) const = 0;

  // Estimate decode time for token generation
  // decode_tokens: Number of output tokens to generate
  // Returns: Estimated time in milliseconds
  virtual double EstimateDecodeMs(size_t decode_tokens) const = 0;

  // Combined estimate (convenience method)
  // Returns: EstimatePrefillMs + EstimateDecodeMs
  double EstimateTotalMs(size_t context_tokens, size_t decode_tokens) const {
    return EstimatePrefillMs(context_tokens) + EstimateDecodeMs(decode_tokens);
  }

  // -------------------------------------------------------------------------
  // Execution methods (blocking)
  // Used by ComputeManager to actually perform compute
  // -------------------------------------------------------------------------

  // Execute prefill operation
  // context_tokens: Number of input tokens to process
  // Returns: Actual time taken in milliseconds
  virtual double Prefill(size_t context_tokens) = 0;

  // Execute decode operation
  // decode_tokens: Number of output tokens to generate
  // Returns: Actual time taken in milliseconds
  virtual double Decode(size_t decode_tokens) = 0;

  // -------------------------------------------------------------------------
  // Data passing (optional, no-op default for emulation)
  // Override in backends that accept real data (e.g., LlamaCppBackend)
  // -------------------------------------------------------------------------

  // Set KV cache binary buffer for next Prefill() call
  virtual void SetKVCacheBuffer(std::vector<uint8_t> /*buffer*/) {}

  // Set raw text content for next Prefill()/Decode() call
  virtual void SetTextContent(const std::string& /*text*/) {}

  // Set aggregate prompt for final decode (from-scratch prefill + decode)
  virtual void SetAggregatePrompt(const std::string& /*prompt*/) {}

  // Get the text output from the last Decode() call.
  // Default returns empty (EmulatedComputeBackend has no real output).
  virtual std::string GetLastDecodeOutput() const { return ""; }

  // -------------------------------------------------------------------------
  // Metadata
  // -------------------------------------------------------------------------

  // Get backend name for logging
  virtual std::string GetName() const = 0;
};

}  // namespace coordinator
}  // namespace webrtc

#endif  // PC_COORDINATOR_COMPUTE_BACKEND_H_
