/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_COORDINATOR_LLAMA_COMPUTE_BACKEND_H_
#define PC_COORDINATOR_LLAMA_COMPUTE_BACKEND_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "pc/coordinator/compute_backend.h"
#include "third_party/llamacpp-kvcache/LLMEngine.h"
#include "rtc_base/logging.h"

namespace webrtc {
namespace coordinator {

// ============================================================================
// LlamaCppBackend - Real GPU inference via llama.cpp LLMEngine
//
// This backend performs actual GPU inference using llama.cpp.
// Supports two data modes:
// - KV cache: Load pre-computed KV state, then decode tokens
// - Raw text: Full prefill + decode from text content
//
// Data flow:
// 1. SetKVCacheBuffer() or SetTextContent() sets pending data
// 2. Prefill() loads KV cache or processes text (clears pending data)
// 3. Decode() generates output tokens
//
// All times are measured wall-clock milliseconds.
// ============================================================================

class LlamaCppBackend : public ComputeBackend {
 public:
  struct Config {
    std::string model_path;
    int gpu_layers = 100;        // -1 = auto from LLAMA_GPU_LAYERS env
    int n_ctx = 4096;
    int n_batch = 2048;
    int decode_length = 20;      // Default tokens to generate per flow
    bool verbose = false;

    static Config FromEnvironment() {
      Config c;
      if (auto* p = std::getenv("LLAMA_MODEL_PATH")) c.model_path = p;
      if (auto* p = std::getenv("LLAMA_GPU_LAYERS")) c.gpu_layers = std::atoi(p);
      if (auto* p = std::getenv("LLAMA_N_CTX")) c.n_ctx = std::atoi(p);
      if (auto* p = std::getenv("LLAMA_N_BATCH")) c.n_batch = std::atoi(p);
      if (auto* p = std::getenv("LLAMA_DECODE_LENGTH")) c.decode_length = std::atoi(p);
      if (auto* p = std::getenv("LLAMA_VERBOSE")) c.verbose = (std::string(p) == "true");
      return c;
    }
  };

  LlamaCppBackend() = default;
  ~LlamaCppBackend() override = default;

  // Initialize the backend: load model into GPU
  // Returns true on success
  bool Init(const Config& config) {
    config_ = config;

    if (config_.model_path.empty()) {
      RTC_LOG(LS_ERROR) << "[LLAMA-BACKEND] No model path specified "
                        << "(set LLAMA_MODEL_PATH env var)";
      return false;
    }

    engine_ = std::make_unique<LLMEngine>();
    engine_->set_verbose(config_.verbose);

    LLMEngineParams params;
    params.n_gpu_layers = config_.gpu_layers;
    params.n_ctx = config_.n_ctx;
    params.n_batch = config_.n_batch;

    RTC_LOG(LS_INFO) << "[LLAMA-BACKEND] Loading model: " << config_.model_path
                     << " (gpu_layers=" << config_.gpu_layers
                     << ", n_ctx=" << config_.n_ctx
                     << ", n_batch=" << config_.n_batch << ")";

    if (!engine_->load_model(config_.model_path, params)) {
      RTC_LOG(LS_ERROR) << "[LLAMA-BACKEND] Failed to load model: "
                        << config_.model_path;
      engine_.reset();
      return false;
    }

    RTC_LOG(LS_INFO) << "[LLAMA-BACKEND] Model loaded successfully";
    return true;
  }

  bool IsLoaded() const {
    return engine_ && engine_->is_loaded();
  }

  // -------------------------------------------------------------------------
  // ComputeBackend interface - Estimation
  // -------------------------------------------------------------------------

  double EstimatePrefillMs(size_t context_tokens) const override {
    // For real GPU, use approximate rates based on GH200
    // GH200 prefill: ~50k tok/s for 7B model
    if (context_tokens == 0) return 0.0;
    return static_cast<double>(context_tokens) / 50000.0 * 1000.0;
  }

  double EstimateDecodeMs(size_t decode_tokens) const override {
    // GH200 decode: ~80 tok/s for 7B model
    if (decode_tokens == 0) return 0.0;
    return static_cast<double>(decode_tokens) / 80.0 * 1000.0;
  }

  // -------------------------------------------------------------------------
  // ComputeBackend interface - Execution
  // -------------------------------------------------------------------------

  double Prefill(size_t context_tokens) override {
    if (!engine_) return 0.0;

    auto start = std::chrono::high_resolution_clock::now();

    // Handle aggregate prompt (final decode from multi-context aggregation)
    if (!pending_aggregate_prompt_.empty()) {
      engine_->clear_state();
      pending_text_ = std::move(pending_aggregate_prompt_);
      pending_aggregate_prompt_.clear();
      pending_kvcache_.clear();
      RTC_LOG(LS_INFO) << "[LLAMA-BACKEND] Using aggregate prompt for from-scratch prefill";
    }

    if (!pending_kvcache_.empty()) {
      // KV cache mode: load pre-computed state
      // Detect format: SPRV (sparse) vs native llama.cpp state
      // SPRV magic = 0x53505256, stored little-endian: V(0x56) R(0x52) P(0x50) S(0x53)
      uint8_t b0 = pending_kvcache_.size() > 0 ? pending_kvcache_[0] : 0;
      uint8_t b1 = pending_kvcache_.size() > 1 ? pending_kvcache_[1] : 0;
      uint8_t b2 = pending_kvcache_.size() > 2 ? pending_kvcache_[2] : 0;
      uint8_t b3 = pending_kvcache_.size() > 3 ? pending_kvcache_[3] : 0;
      fprintf(stderr, "[LLAMA-BACKEND] KV cache first 4 bytes: %02x %02x %02x %02x\n",
              b0, b1, b2, b3);
      bool is_sprv = pending_kvcache_.size() >= 4 &&
          b0 == 0x56 && b1 == 0x52 && b2 == 0x50 && b3 == 0x53;

      fprintf(stderr, "[LLAMA-BACKEND] Loading KV cache: %zu bytes, format=%s\n",
              pending_kvcache_.size(), is_sprv ? "SPRV" : "native");

      // Clear previous KV cache state before loading new one
      // Without this, loading SPRV into a context with leftover decode state crashes
      engine_->clear_state();

      bool ok;
      if (is_sprv) {
        ok = engine_->load_sparse_kvcache_from_buffer(pending_kvcache_);
      } else {
        ok = engine_->load_kvcache_from_buffer(pending_kvcache_);
      }
      if (!ok) {
        fprintf(stderr, "[LLAMA-BACKEND] Failed to load KV cache (%s) — skipping decode\n",
                is_sprv ? "SPRV" : "native");
        engine_->clear_state();  // Reset to clean state after failed load
        prefill_failed_ = true;
      } else {
        prefill_failed_ = false;
      }
      pending_kvcache_.clear();
      pending_kvcache_.shrink_to_fit();

    } else if (!pending_text_.empty()) {
      // Raw text mode: clear state for fresh prefill
      // Text will be passed to decode() as the prompt
      engine_->clear_state();
      RTC_LOG(LS_INFO) << "[LLAMA-BACKEND] Text prefill prepared: "
                       << pending_text_.size() << " chars";

    } else if (context_tokens > 0) {
      // No data but context_tokens > 0: shouldn't happen in normal flow
      RTC_LOG(LS_WARNING) << "[LLAMA-BACKEND] Prefill with context_tokens="
                          << context_tokens << " but no data";
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double, std::milli>(end - start).count();

    RTC_LOG(LS_INFO) << "[LLAMA-BACKEND] Prefill completed: " << elapsed << " ms";
    return elapsed;
  }

  double Decode(size_t decode_tokens) override {
    if (!engine_) return 0.0;
    if (prefill_failed_) {
      fprintf(stderr, "[LLAMA-BACKEND] Skipping decode (prefill failed)\n");
      prefill_failed_ = false;
      return 0.0;
    }

    // Signed int so that LLAMA_DECODE_LENGTH=-1 (unlimited/EOS-only) can
    // pass through to LLMEngine::decode which treats negative n_predict as
    // "generate until EOS up to an internal cap".
    int n_predict = decode_tokens > 0 ? static_cast<int>(decode_tokens)
                                      : config_.decode_length;

    // Use pending text as prompt (raw text mode) or empty (KV cache mode)
    std::string prompt = std::move(pending_text_);
    pending_text_.clear();

    RTC_LOG(LS_INFO) << "[LLAMA-BACKEND] Decode: n_predict=" << n_predict
                     << ", prompt_len=" << prompt.size();

    auto start = std::chrono::high_resolution_clock::now();

    std::string response = engine_->decode(prompt, n_predict);
    last_decode_output_ = response;  // Save for multi-context aggregation

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double, std::milli>(end - start).count();

    RTC_LOG(LS_INFO) << "[LLAMA-BACKEND] Decode completed: " << elapsed << " ms"
                     << ", output_len=" << response.size();
    fprintf(stderr, "[DECODE-OUTPUT] len=%zu text=%.200s%s\n",
            response.size(), response.c_str(),
            response.size() > 200 ? "..." : "");
    return elapsed;
  }

  // -------------------------------------------------------------------------
  // Data passing (override ComputeBackend defaults)
  // -------------------------------------------------------------------------

  void SetKVCacheBuffer(std::vector<uint8_t> buffer) override {
    pending_kvcache_ = std::move(buffer);
    RTC_LOG(LS_VERBOSE) << "[LLAMA-BACKEND] KV cache buffer set: "
                        << pending_kvcache_.size() << " bytes";
  }

  void SetTextContent(const std::string& text) override {
    pending_text_ = text;
    RTC_LOG(LS_VERBOSE) << "[LLAMA-BACKEND] Text content set: "
                        << pending_text_.size() << " chars";
  }

  // -------------------------------------------------------------------------
  // Metadata
  // -------------------------------------------------------------------------

  std::string GetName() const override { return "LlamaCpp"; }

  std::string GetLastDecodeOutput() const override {
    return last_decode_output_;
  }

  void SetAggregatePrompt(const std::string& prompt) override {
    pending_aggregate_prompt_ = prompt;
    RTC_LOG(LS_INFO) << "[LLAMA-BACKEND] Aggregate prompt set: "
                     << prompt.size() << " chars";
  }

 private:
  std::unique_ptr<LLMEngine> engine_;
  Config config_;

  // Pending data (set before Prefill/Decode, consumed during execution)
  std::vector<uint8_t> pending_kvcache_;
  std::string pending_text_;
  bool prefill_failed_ = false;
  std::string last_decode_output_;
  std::string pending_aggregate_prompt_;
};

}  // namespace coordinator
}  // namespace webrtc

#endif  // PC_COORDINATOR_LLAMA_COMPUTE_BACKEND_H_
