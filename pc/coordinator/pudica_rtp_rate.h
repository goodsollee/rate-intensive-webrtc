/*
 * Copyright 2026 The WebRTC project authors. All Rights Reserved.
 *
 * Pudica NSDI'24 §4.2–§4.3 RTP-video rate controller.
 *
 * Paper: Wang et al., "Pudica: Toward Near-Zero Queuing Delay in Congestion
 * Control for Cloud Gaming", NSDI'24.
 *
 * This header is STL-only so the standalone test can include it without the
 * rest of the WebRTC tree. The coordinator owns one instance and feeds it
 * per-frame BUR + receiving_rate.
 *
 * Control order (short-term §4.3 overrides smoothed §4.2):
 *   1. Drain exit: draining AND frame BUR < 1  →  B ← receiving_rate
 *   2. Active drain: 3 consecutive frame BUR > 1  →  Eq.11
 *   3. Temporary fallback: one frame BUR > 1  →  B_pub ← B×(1−ζ) for one
 *      frame, then revert. Committed B is unchanged.
 *   4. Hold while waiting for feedback of the last MI/AI-MD.
 *   5. Smoothed BUR ≤ α  →  MI  Eq.7–8
 *   6. Smoothed BUR >  α  →  AI-MD Eq.9–10
 *
 * HOLD is "keep committed B". It is not "set B to receiving_rate".
 * One-step restore to receiving_rate runs only when leaving the drain phase.
 */
#ifndef PC_COORDINATOR_PUDICA_RTP_RATE_H_
#define PC_COORDINATOR_PUDICA_RTP_RATE_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>

namespace webrtc {

// One BUR sample for Eq.6. bitrate_bps is the encoding bitrate B_k of that
// frame (paper Appendix B / Eq.6).
struct PudicaBurSampleEq6 {
  int64_t time_us = 0;
  double bur = 0.0;
  double bitrate_bps = 0.0;
};

// Eq.6 + Appendix B. Weights:
//   ω_I   = min(R_k + 1, 2)
//   ω_II  = min(B_k_Mbps + 10, 50)
//   ω_III = k + 20   (k = 1-indexed position in the window, newer = larger)
//   ω_k   = product / Σ product
//   R̃    = Σ ω_k × R_k × (B / B_k)
inline double PudicaSmoothBurEq6(
    const std::deque<PudicaBurSampleEq6>& history,
    double current_bitrate_bps,
    int64_t now_us,
    int64_t window_us) {
  if (history.empty()) return 0.0;
  if (current_bitrate_bps <= 0.0) current_bitrate_bps = 1.0;

  struct Term {
    double bur;
    double ratio;
    double wI;
    double wII;
    double wIII;
  };
  std::deque<Term> terms;
  int k = 0;
  for (const auto& s : history) {
    if (now_us > 0 && window_us > 0 && (now_us - s.time_us) > window_us) {
      continue;
    }
    ++k;
    double Bk_mbps = s.bitrate_bps / 1e6;
    if (Bk_mbps < 0.0) Bk_mbps = 0.0;
    double ratio = (s.bitrate_bps > 0.0)
                       ? (current_bitrate_bps / s.bitrate_bps)
                       : 1.0;
    Term t;
    t.bur = s.bur;
    t.ratio = ratio;
    t.wI = std::min(s.bur + 1.0, 2.0);
    t.wII = std::min(Bk_mbps + 10.0, 50.0);
    t.wIII = static_cast<double>(k + 20);
    terms.push_back(t);
  }
  if (terms.empty()) return 0.0;

  double sum_prod = 0.0;
  for (const auto& t : terms) sum_prod += t.wI * t.wII * t.wIII;
  if (sum_prod <= 0.0) return 0.0;

  double r_tilde = 0.0;
  for (const auto& t : terms) {
    double w = (t.wI * t.wII * t.wIII) / sum_prod;
    r_tilde += w * t.bur * t.ratio;
  }
  return r_tilde;
}

class PudicaRtpRateCtrl {
 public:
  struct Config {
    double alpha = 0.85;          // §4.2 efficiency/fairness threshold
    double gamma_mi = 0.30;       // Eq.8
    double gamma_md = 0.05;       // Eq.10
    double zeta = 0.15;           // §4.3 temporary fallback
    double drain_horizon_s = 0.200;
    double bmax_mbps = 50.0;      // paper application cap, used in I
    double aimd_a_bound = 0.10;   // |A|/B cap; paper says bounds exist, no number
    int64_t tau_reset_us = 5'000'000;
    int64_t min_rate_bps = 1'000'000;
    int64_t max_rate_bps = 1'000'000'000;
    int consecutive_drain = 3;
    double bur_high = 1.0;        // short-term BUR threshold
    double bur_recover = 1.0;     // drain-exit threshold

    static Config FromEnv() {
      Config c;
      auto d = [](const char* k, double def) {
        const char* e = std::getenv(k);
        return e ? std::atof(e) : def;
      };
      auto i64 = [](const char* k, int64_t def) {
        const char* e = std::getenv(k);
        return e ? static_cast<int64_t>(std::atoll(e)) : def;
      };
      c.alpha = d("PUDICA_ALPHA", 0.85);
      c.gamma_mi = d("PUDICA_GAMMA_MI", 0.30);
      c.gamma_md = d("PUDICA_GAMMA_MD", 0.05);
      c.zeta = d("PUDICA_ZETA", 0.15);
      c.bmax_mbps = d("PUDICA_BMAX_MBPS", 50.0);
      c.aimd_a_bound = d("PUDICA_AIMD_A_BOUND", 0.10);
      int64_t tau_ms = i64("PUDICA_TAU_RESET_MS", 5000);
      c.tau_reset_us = tau_ms * 1000;
      int64_t max_kbps = i64("PUDICA_MAX_RATE_KBPS", 0);
      if (max_kbps > 0) c.max_rate_bps = max_kbps * 1000;
      return c;
    }
  };

  struct Input {
    double frame_bur = 0.0;
    double smoothed_bur = 0.0;
    int64_t recv_rate_bps = 0;     // current windowed receiving_rate
    int64_t inflight_bytes = 0;    // paper: in-flight volume for draining_rate
    int64_t now_us = 0;
    int64_t frame_send_us = 0;     // first-packet send time of the BUR frame
  };

  struct Output {
    int64_t target_bps = 0;
    const char* mode = "PUD-HOLD";
    bool draining = false;
    int consecutive_high = 0;
    int64_t committed_bps = 0;
    int64_t drain_recv_bps = 0;
    double xi_or_A = 0.0;  // MI ξ or AI-MD A (Mbps)
  };

  Config cfg;
  int64_t committed_bps = 0;

  Output Update(const Input& in) {
    Output out;
    if (committed_bps <= 0) {
      committed_bps = in.recv_rate_bps > 0 ? in.recv_rate_bps : cfg.min_rate_bps;
    }

    // One-shot fallback reverts before the new decision (paper: next frame
    // only, then encoder returns to the previous setting).
    if (fallback_revert_) {
      fallback_revert_ = false;
    }

    if (in.frame_bur > cfg.bur_high) {
      consecutive_high_++;
    } else {
      consecutive_high_ = 0;
    }

    // τ: received-frame count since last init. Reset when R̃ > 1 or every 5 s.
    if (in.smoothed_bur > 1.0) {
      tau_ = 0;
      tau_init_us_ = in.now_us;
    } else if (tau_init_us_ > 0 &&
               (in.now_us - tau_init_us_) > cfg.tau_reset_us) {
      tau_ = 0;
      tau_init_us_ = in.now_us;
    }
    if (tau_init_us_ == 0) tau_init_us_ = in.now_us;
    tau_++;

    // --- §4.3 drain exit: one-step restore to receiving_rate ---
    if (draining_ && in.frame_bur < cfg.bur_recover) {
      draining_ = false;
      consecutive_high_ = 0;
      mi_pending_ = false;
      int64_t recv = in.recv_rate_bps > 0 ? in.recv_rate_bps : committed_bps;
      committed_bps = recv;
      tau_ = 0;
      tau_init_us_ = in.now_us;
      return Finish(out, "PUD-RESTORE", in);
    }

    // --- §4.3 active drain: three consecutive BUR > 1 ---
    if (consecutive_high_ >= cfg.consecutive_drain || draining_) {
      if (!draining_) {
        draining_ = true;
        drain_recv_sum_ = 0.0;
        drain_recv_n_ = 0;
        drain_start_us_ = in.now_us;
      }
      if (in.recv_rate_bps > 0) {
        drain_recv_sum_ += static_cast<double>(in.recv_rate_bps);
        drain_recv_n_++;
      }
      double recv = (drain_recv_n_ > 0)
                        ? (drain_recv_sum_ / drain_recv_n_)
                        : static_cast<double>(committed_bps);
      drain_recv_bps_ = static_cast<int64_t>(recv);
      double drain_rate = 0.0;
      if (in.inflight_bytes > 0 && cfg.drain_horizon_s > 0.0) {
        drain_rate = static_cast<double>(in.inflight_bytes) * 8.0 /
                     cfg.drain_horizon_s;
      }
      committed_bps =
          static_cast<int64_t>(cfg.alpha * recv - drain_rate);
      mi_pending_ = false;
      return Finish(out, "PUD-DRAIN", in);
    }

    // --- §4.3 temporary fallback (committed B unchanged) ---
    if (in.frame_bur > cfg.bur_high) {
      int64_t published = static_cast<int64_t>(
          static_cast<double>(committed_bps) * (1.0 - cfg.zeta));
      fallback_revert_ = true;
      out.mode = "PUD-FALLBACK";
      out.target_bps = Clamp(published);
      out.draining = false;
      out.consecutive_high = consecutive_high_;
      out.committed_bps = committed_bps;
      out.drain_recv_bps = drain_recv_bps_;
      return out;
    }

    // --- wait for feedback of last MI / AI-MD ---
    // Paper states this for MI. AI-MD is applied at frame rate without a wait
    // in the text, but then A accumulates tens of Mbps per second; we apply
    // the same wait to AI-MD (J-251). Feedback of an adjustment is a frame
    // whose send time is at/after the decision's recv now (≈ one RTT).
    if (mi_pending_ && in.frame_send_us > 0 && mi_decision_now_us_ > 0 &&
        in.frame_send_us < mi_decision_now_us_) {
      return Finish(out, "PUD-HOLD", in);
    }
    mi_pending_ = false;

    if (in.smoothed_bur <= cfg.alpha) {
      double R = std::max(in.smoothed_bur, 1e-3);
      double xi = cfg.gamma_mi * (((cfg.alpha + 1.0) / 2.0) - R) / R;
      if (xi < 0.0) xi = 0.0;
      committed_bps = static_cast<int64_t>(
          static_cast<double>(committed_bps) * (1.0 + xi));
      mi_pending_ = true;
      mi_decision_now_us_ = in.now_us;
      out.xi_or_A = xi;
      return Finish(out, "PUD-MI", in);
    }

    // AI-MD: I = (Bmax + 2τ / ln(B_Mbps)) × (γ_MD / 2)
    // Layout in the PDF is a stacked fraction 2τ / log(B). Natural log.
    // Floor B at 2 Mbps so ln is ≥ ln2 and I stays finite (paper's B ≫ 1).
    double B_mbps = std::max(committed_bps / 1e6, 2.0);
    double I = (cfg.bmax_mbps + (2.0 * static_cast<double>(tau_)) /
                                    std::log(B_mbps)) *
               (cfg.gamma_md / 2.0);
    double A = I - cfg.gamma_md * B_mbps;
    double bound = cfg.aimd_a_bound * B_mbps;
    A = std::max(-bound, std::min(bound, A));
    committed_bps = static_cast<int64_t>((B_mbps + A) * 1e6);
    mi_pending_ = true;
    mi_decision_now_us_ = in.now_us;
    out.xi_or_A = A;
    return Finish(out, "PUD-AIMD", in);
  }

  void Reset() {
    committed_bps = 0;
    draining_ = false;
    consecutive_high_ = 0;
    fallback_revert_ = false;
    mi_pending_ = false;
    mi_decision_now_us_ = 0;
    tau_ = 0;
    tau_init_us_ = 0;
    drain_recv_sum_ = 0.0;
    drain_recv_n_ = 0;
    drain_start_us_ = 0;
    drain_recv_bps_ = 0;
  }

  bool draining() const { return draining_; }
  int consecutive_high() const { return consecutive_high_; }

 private:
  int64_t Clamp(int64_t b) const {
    return std::max(cfg.min_rate_bps, std::min(cfg.max_rate_bps, b));
  }

  Output Finish(Output& out, const char* mode, const Input& in) {
    committed_bps = Clamp(committed_bps);
    out.mode = mode;
    out.target_bps = committed_bps;
    out.draining = draining_;
    out.consecutive_high = consecutive_high_;
    out.committed_bps = committed_bps;
    out.drain_recv_bps = drain_recv_bps_;
    (void)in;
    return out;
  }

  bool draining_ = false;
  int consecutive_high_ = 0;
  bool fallback_revert_ = false;
  bool mi_pending_ = false;
  int64_t mi_decision_now_us_ = 0;
  int64_t tau_ = 0;
  int64_t tau_init_us_ = 0;
  double drain_recv_sum_ = 0.0;
  int drain_recv_n_ = 0;
  int64_t drain_start_us_ = 0;
  int64_t drain_recv_bps_ = 0;
};

}  // namespace webrtc

#endif  // PC_COORDINATOR_PUDICA_RTP_RATE_H_
