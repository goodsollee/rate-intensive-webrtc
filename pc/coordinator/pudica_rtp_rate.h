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

// Application bitrate cap. Matches the PeerConnection max used in this
// testbed (KFT_MAX_BITRATE_KBPS default 100000). Paper §5.1 used 50 Mbps;
// our encoder is 100. This is the published-B ceiling, not the I-formula
// B_max (cfg.bmax_mbps, still paper 50 unless PUDICA_BMAX_MBPS is set).
static constexpr int64_t kPudicaAppCapBps = 100'000'000;

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
    double bmax_mbps = 50.0;      // paper §5.1 B_max, used in I only
    double aimd_a_bound = 0.10;   // |A|/B cap; paper says bounds exist, no number
    // Two OFF-BY-DEFAULT experiment knobs for the MI branch. MI is the one
    // adjustment path with no bound of any kind: xi = gamma*((alpha+1)/2 - R)/R
    // diverges as R -> 0, and R is small exactly when the controller has just
    // throttled itself and the queue is empty. Measured on run 1788865209
    // (40 Mbps link): 38% of MI steps had xi > 1, and B reached the 100 Mbps
    // app cap from under 40 in a median of 1.44 s. AI-MD has aimd_a_bound for
    // the same reason; MI has no equivalent.
    //
    // Both default to 0 = disabled, so the shipped behaviour is unchanged and
    // these exist to be measured against each other, one run per knob:
    //   xi_max     caps the STEP  (how fast B rises)
    //   mi_recv_k  caps the LEVEL (B <= k * receiving_rate; MI is otherwise
    //              entirely rate-blind, multiplying committed_bps, a number
    //              that can be a stale echo of a rate that no longer flows)
    double xi_max = 0.0;          // PUDICA_XI_MAX      (0 = unbounded)
    double mi_recv_k = 0.0;       // PUDICA_MI_RECV_K   (0 = no clamp)
    // Third MI knob, also OFF by default. xi_max and mi_recv_k both bound the
    // NUMBER; this one bounds the CADENCE, and it is the one that follows the
    // paper's own sentence: "the next adjustment is postponed until the
    // feedback regarding the current adjustment is received ... to prevent
    // overaggressive rate increases". The wait below already implements that
    // sentence literally -- measured on run 1788919321, 0 of 321 consecutive
    // MI steps were closer than one frame apart, and the median gap (170 ms)
    // is one frame OWD (147 ms) plus one frame interval. It still ramped
    // 2.2 -> 18.6 Mbps in 1.07 s, because one OWD on this link is 150 ms and
    // the step is multiplicative: 0.3 per step at 6.6 steps/s is x5.6 per
    // second.
    //
    // The reason the literal rule does not bite is that the feedback it waits
    // for carries no information about the adjustment. B was raised to 9.6
    // Mbps at t=22.58 and the sender's actual rate over the next 200 ms was
    // 1.33 Mbps (14% of it) -- the encoder had not adopted the new target yet.
    // So the frame that released the wait queued as if 1.3 Mbps were being
    // sent, BUR stayed low, and MI read "still headroom" and raised again.
    // Six rounds of that reached 15.2 Mbps on an 8 Mbps link.
    //
    // mi_send_ratio closes it: hold until the sender has actually OFFERED at
    // least this fraction of the committed rate since the decision. No link
    // estimate and no new constant about the network -- it only asks whether
    // the last increase was carried out before allowing the next one.
    //
    // Deadlock is not a risk: DRAIN, the drain exit and FALLBACK are all
    // evaluated BEFORE this gate, so holding forever blocks only increases,
    // and an increase that the sender cannot realise is one that should not
    // happen.
    double mi_send_ratio = 0.0;   // PUDICA_MI_SEND_RATIO (0 = no gate)
    // [A23] Upper bound on how long (b) may hold, ms. 0 = unbounded (the
    // shipped behaviour, and the defect below).
    //
    // The "Deadlock is not a risk" claim above is wrong and was retracted: it
    // reasoned that holding blocks only INCREASES, which is true, and then
    // assumed an increase the sender cannot realise is one that should not
    // happen -- which is false right after a collapse, when the sender cannot
    // realise ANY rate for reasons that have nothing to do with the target.
    // The gate then waits on the encoder while the encoder waits on the gate.
    //
    // Measured, run 1788934307 (fastdrop: 200 -> 3 Mbps for ONE second -> 40):
    // B froze at 3.84 Mbps from t=22.089 to t=29.953 -- 7.87 s -- with
    // mi_need_ms = 0.000 throughout (so (c) was satisfied and only (b) held)
    // while sent/committed crawled 0.13 -> 0.70 and BUR read 0.03-0.09 against
    // a true link utilisation of 0.02-0.08 on a 40 Mbps link. Full recovery
    // from the one-second outage took 16 s. The same lock cost 3.43 s in the
    // deepdip run 1788934092 (B pinned at 4.33 on a 7.32 Mbps payload link).
    //
    // Past the cap the question stops carrying information: if the sender
    // still cannot offer the current target after this long, the target is not
    // what limits it, and holding only freezes the controller at a rate its
    // own BUR says is far below capacity. (a) and (c) still gate MI, so the
    // cadence stays bounded -- this removes a stall, not a brake.
    double mi_send_hold_ms = 0.0; // PUDICA_MI_SEND_HOLD_MS (0 = unbounded)
    // Fourth MI knob, OFF by default. mi_send_ratio waits for the ENCODER to
    // realise the new target; this waits for the MEASUREMENT to be about it.
    //
    // MI does not read one frame's delay, it reads smoothed_bur -- Eq.6 over a
    // 200 ms window (kPudicaBurWindowUs) whose recency weight (k+20) is nearly
    // flat, so one fresh sample barely moves it. A sample's timestamp is its
    // FEEDBACK time, and the frame it describes was sent one OWD earlier, so at
    // decision time T the window describes frames SENT in
    // [T - OWD - 200ms, T - OWD]. For that window to be entirely about the
    // previous adjustment, decisions must be at least OWD + 200 ms apart.
    //
    // Measured on run 1788919321: required 347 ms (OWD p50 147 + 200), actual
    // median gap 170 ms -- 49% of it. The share of the window describing
    // post-adjustment sends was p50 = 15%, and only 34 of 321 steps reached
    // 100%. On the ramp that produced the 231 KB resolution-switch keyframe the
    // decision that set B to 15.16 Mbps read a window that was 83% about the
    // rate before it.
    //
    // Unlike mi_send_ratio this cannot deadlock: the condition is monotonic in
    // time, so it always opens. That property is why it is preferred -- see
    // run 1788925150, where mi_send_ratio locked MI out for the whole run.
    //
    // K = 1.0 is one full window. No new constant about the network: OWD is
    // measured per frame and 200 ms is Eq.6's own window.
    double mi_settle_k = 0.0;     // PUDICA_MI_SETTLE_K (0 = no settle wait)
    // DRAIN knob, OFF by default. §4.3: "the volume of self-induced queued
    // data is quantified by measuring the number of in-flight packets".
    //
    // The shipped path reconstructs that volume from BUR instead:
    //   queue = recv x frame_bur x L        (A11)
    // which is algebraically the same quantity -- but frame_bur is ONE frame's
    // OWD, unsmoothed. Measured on run 1788928047 t=43.6-43.9, alpha*recv held
    // flat at 37.5-39.2 Mbps while drain_rate swung 8.1 <-> 24.1 (3x) purely
    // from frame_bur 2.36 -> 3.12 -> 1.67, and B fell 39 -> 15 Mbps with it.
    // One of those swings is the frame's own emission span (100 ms) dropping
    // out of D. That is the whole 2.5 s oscillation.
    //
    // data_in_flight is a transport counter over hundreds of packets, so the
    // single-frame noise term does not exist there. What it does carry is the
    // BDP: in_flight ~= recv x D = recv x D_min + recv x (D - D_min), and only
    // the second half is self-induced. A11 already paid for this lesson from
    // the other direction -- using the full D made drain_rate ~= 0.6 x rate on
    // an empty queue. So the BDP is subtracted here too; taking the paper's
    // sentence literally without it would roughly DOUBLE drain_rate.
    bool drain_inflight = false;  // PUDICA_DRAIN_INFLIGHT
    int64_t tau_reset_us = 5'000'000;
    int64_t min_rate_bps = 1'000'000;
    int64_t max_rate_bps = kPudicaAppCapBps;  // published B ceiling (100 Mbps)
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
      c.xi_max = d("PUDICA_XI_MAX", 0.0);
      c.mi_recv_k = d("PUDICA_MI_RECV_K", 0.0);
      c.mi_send_hold_ms = d("PUDICA_MI_SEND_HOLD_MS", 0.0);
      c.mi_send_ratio = d("PUDICA_MI_SEND_RATIO", 0.0);
      c.mi_settle_k = d("PUDICA_MI_SETTLE_K", 0.0);
      c.drain_inflight = d("PUDICA_DRAIN_INFLIGHT", 0.0) != 0.0;
      int64_t tau_ms = i64("PUDICA_TAU_RESET_MS", 5000);
      c.tau_reset_us = tau_ms * 1000;
      int64_t max_kbps = i64("PUDICA_MAX_RATE_KBPS", 0);
      if (max_kbps > 0) c.max_rate_bps = max_kbps * 1000;
      // Unset → 100 Mbps. Do not inherit BUR_MAX_RATE_MBPS (SCTP, default 1000).
      return c;
    }
  };

  // Decision instant of a pending MI/AI-MD, or -1 when none is pending. The
  // caller sums offered bytes from here so the gate's interval and the
  // measurement's interval are the same by construction.
  int64_t MiDecisionUs() const {
    return mi_pending_ ? mi_decision_now_us_ : -1;
  }

  struct Input {
    double frame_bur = 0.0;
    double smoothed_bur = 0.0;
    int64_t recv_rate_bps = 0;     // current windowed receiving_rate
    int64_t inflight_bytes = 0;    // paper: in-flight volume for draining_rate
    int64_t now_us = 0;
    int64_t frame_send_us = 0;     // first-packet send time of the BUR frame
    // Media bytes the sender put on the wire over exactly `sent_span_us`,
    // ending now and starting at the later of the pending MI decision and the
    // send window's own left edge. Both are filled by the caller so the sum and
    // the span cannot disagree: dividing bytes capped by a 2 s window by an
    // unbounded elapsed time made the measured rate DECAY the longer the gate
    // held, which locked MI out for a whole run (1788925150).
    int64_t sent_bytes_since_decision = 0;
    int64_t sent_span_us = 0;
    // One-way delay of the frame reporting this BUR, and the width of the Eq.6
    // smoothing window -- the two terms of the settle horizon.
    int64_t frame_owd_us = 0;
    int64_t bur_window_us = 0;
    // Measured outstanding bytes (BDP + self-induced queue) and the D_min used
    // to split them. Only read when cfg.drain_inflight is set.
    int64_t inflight_meas_bytes = 0;
    int64_t d_min_us = 0;
  };

  struct Output {
    int64_t target_bps = 0;
    const char* mode = "PUD-HOLD";
    bool draining = false;
    int consecutive_high = 0;
    int64_t committed_bps = 0;
    int64_t drain_recv_bps = 0;
    double xi_or_A = 0.0;  // MI ξ or AI-MD A (Mbps)
    // Rate the sender actually offered since the pending decision, bps.
    // -1 when nothing is pending or the interval is too short to divide by.
    double mi_send_bps = -1.0;
    // Settle horizon still owed at this tick, µs. -1 when nothing is pending
    // or mi_settle_k is off; 0 once satisfied.
    int64_t mi_need_us = -1;
    // DRAIN diagnostics: the two terms of B = alpha*recv - drain_rate, and the
    // queue volume they came from. -1 when this tick is not a DRAIN.
    double drain_rate_bps = -1.0;
    double drain_queue_bytes = -1.0;
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
      double queue_bytes = 0.0;
      if (cfg.drain_inflight) {
        // Paper path: measured in-flight, minus the part that is merely on the
        // wire. recv is the bottleneck rate (A11), so BDP = recv * D_min.
        const double bdp_bytes = recv * (static_cast<double>(in.d_min_us) / 1e6) / 8.0;
        queue_bytes = static_cast<double>(in.inflight_meas_bytes) - bdp_bytes;
        if (queue_bytes < 0.0) queue_bytes = 0.0;
      } else {
        queue_bytes = static_cast<double>(in.inflight_bytes);
      }
      double drain_rate = 0.0;
      if (queue_bytes > 0.0 && cfg.drain_horizon_s > 0.0) {
        drain_rate = queue_bytes * 8.0 / cfg.drain_horizon_s;
      }
      out.drain_rate_bps = drain_rate;
      out.drain_queue_bytes = queue_bytes;
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
    if (mi_pending_ && in.frame_send_us > 0 && mi_decision_now_us_ > 0) {
      // (a) paper's rule, unchanged: the reporting frame must have been sent
      //     at or after the decision.
      bool stale_frame = in.frame_send_us < mi_decision_now_us_;
      // (b) mi_send_ratio: and the sender must have actually offered the rate
      //     it committed to. Averaged over [decision, now], so it starts near
      //     zero and rises as feedback for the new bytes arrives -- the gate
      //     opens when the offer is real, not when the clock says so.
      bool offer_unrealised = false;
      if (cfg.mi_send_ratio > 0.0 && committed_bps > 0 && in.sent_span_us > 0) {
        double sent_bps = static_cast<double>(in.sent_bytes_since_decision) *
                          8.0 * 1e6 / static_cast<double>(in.sent_span_us);
        out.mi_send_bps = sent_bps;
        offer_unrealised =
            sent_bps < cfg.mi_send_ratio * static_cast<double>(committed_bps);
        // [A23] ...but only for a bounded time after the decision.
        if (offer_unrealised && cfg.mi_send_hold_ms > 0.0 &&
            (in.now_us - mi_decision_now_us_) >
                static_cast<int64_t>(cfg.mi_send_hold_ms * 1000.0)) {
          offer_unrealised = false;
        }
      }
      // (c) mi_settle_k: and smoothed_bur must have had time to become a
      //     measurement OF the new rate — one OWD for the first post-decision
      //     frame to report, plus K Eq.6 windows for the window to turn over.
      bool not_settled = false;
      if (cfg.mi_settle_k > 0.0 && in.bur_window_us > 0) {
        int64_t need_us =
            std::max<int64_t>(0, in.frame_owd_us) +
            static_cast<int64_t>(cfg.mi_settle_k *
                                 static_cast<double>(in.bur_window_us));
        int64_t waited_us = in.now_us - mi_decision_now_us_;
        out.mi_need_us = std::max<int64_t>(0, need_us - waited_us);
        not_settled = waited_us < need_us;
      }
      if (stale_frame || offer_unrealised || not_settled)
        return Finish(out, "PUD-HOLD", in);
    }
    mi_pending_ = false;

    if (in.smoothed_bur <= cfg.alpha) {
      double R = std::max(in.smoothed_bur, 1e-3);
      double xi = cfg.gamma_mi * (((cfg.alpha + 1.0) / 2.0) - R) / R;
      if (xi < 0.0) xi = 0.0;
      if (cfg.xi_max > 0.0 && xi > cfg.xi_max) xi = cfg.xi_max;
      committed_bps = static_cast<int64_t>(
          static_cast<double>(committed_bps) * (1.0 + xi));
      // Level clamp: never aim more than k x what is actually arriving. Guarded
      // on recv_rate_bps > 0 so a feedback gap cannot clamp B to zero.
      if (cfg.mi_recv_k > 0.0 && in.recv_rate_bps > 0) {
        int64_t ceiling = static_cast<int64_t>(cfg.mi_recv_k *
                                               static_cast<double>(in.recv_rate_bps));
        if (ceiling > 0 && committed_bps > ceiling) committed_bps = ceiling;
      }
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
