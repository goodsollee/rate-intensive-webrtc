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

// [A41] One frame's BUR as the paper defines it -- "the ratio of current
// bandwidth usage to the link capacity" (§3.1), measured as Eq.1
// R = (D - D_min) / L with D from the first packet SENT to the last packet
// RECEIVED, i.e. the frame's own emission span included. Eq.2 exists to make
// that work: it emits the frame over L/rho, slightly faster than the
// bottleneck drains it, so the bottleneck's serialisation S/C hides the span
// and D - D_min reads S/C + queue = (b/C + q/L) * L.
//
// The frame BUR the controller uses for its short-term triggers subtracts the
// whole span instead (A10), which was right while the pacer spread frames at
// the committed rate and D was dominated by the sender's own emission. It
// also removes the utilisation term: near capacity it reads about half of
// b/C (run 1789041793, 38-42 s: 0.36 against 0.78 for Eq.1 and 0.76
// measured), so MI never hands over to AI-MD at alpha.
//
// Only one part of the span is the sender's fault: the time the pacer took
// BEYOND the span Eq.2 asked for, and only on a frame whose tail did not
// queue at the bottleneck (emission-limited). When the tail queued, the
// bottleneck was the slower of the two and D - D_min is S/C + queue however
// long the emission took. kTailQueuedUs is the send-side timestamp
// resolution (rtc::SentPacket is in ms): below it the two cannot be told
// apart.
inline double PudicaUtilBur(double D_us, double d_min_us, double span_us,
                            double intended_span_us, double L_us) {
  if (L_us <= 0.0) return 0.0;
  constexpr double kTailQueuedUs = 1000.0;
  const double tail_queue_us = D_us - span_us - d_min_us;
  const double overrun_us = (tail_queue_us <= kTailQueuedUs)
                                ? std::max(0.0, span_us - intended_span_us)
                                : 0.0;
  return std::max(0.0, D_us - d_min_us - overrun_us) / L_us;
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
    // [A32] Delivered-rate ceiling. The reference implementation's only bound
    // on the LEVEL MI can reach: B <= k x max(receiving_rate) over the last
    // window, plus an additive headroom.
    //
    // This replaces the two wait-gates that used to stand in front of MI
    // (mi_send_ratio, mi_settle_k). Both asked "has the previous increase been
    // realised yet?" and held MI until the answer was yes. That condition is
    // not monotonic: right after a collapse the sender cannot realise ANY rate
    // for reasons that have nothing to do with the target, so the gate waited
    // on the encoder while the encoder waited on the gate. Measured on
    // 1789027936 / 1789028040 (q1s_attcampus_5g_122s): 94.5% / 95.1% of all
    // decisions returned PUD-HOLD and MI fired 10 / 8 times in 75 s, against
    // 1053 MI steps in the reference run 1787122944 on the same trace.
    //
    // A ceiling has neither failure mode. It bounds the number instead of
    // waiting for a condition, and it is monotonic in the one signal that
    // still carries information during a collapse: recv rises -> the ceiling
    // rises. It is also the bound MI has always lacked -- xi diverges as
    // R~ -> 0, and R~ is small exactly when the controller has just throttled
    // itself, so MI is otherwise free to multiply a committed_bps that is a
    // stale echo of a rate no longer flowing.
    //
    // Rolling MAX, not mean: recv is sampled per frame and noisy, and a
    // max-filter's error is one-sided upward, so it errs toward letting B grow
    // rather than choking it. k = 1.5 over 500 ms bounds growth at 2.25x/s.
    //
    // The additive headroom exists because a purely multiplicative ceiling can
    // crawl on the way up: recv is itself bounded by B, so the two can pin each
    // other frame for frame. [A33] made the ceiling increase-only, which removes
    // the RATCHET, but not the stall: with headroom 0 the ceiling can still sit
    // BELOW an already-committed B and refuse every increase, which is a fixed
    // point rather than a collapse but stalls just as hard.
    //
    // Measured on the FV3-OFF baseline 1789036115 (headroom 0): at trace t=20 s
    // the link was 100 Mbps and R~ = 0.03 -- the controller's own queue signal
    // saying 30x headroom -- yet recv had collapsed to 0.06 Mbps, so the ceiling
    // was 0.09 < committed 1.78 and MI was refused outright for ~2 s. Over the
    // run, 36.4% of decisions had ceiling < committed and 69.7% of PUD-MI
    // decisions raised committed by nothing at all.
    //
    // 2 Mbps ([A36], run 1789036711): committed p25 7.15 -> 14.55 Mbps, encoder
    // output p25 2.51 -> 5.55, freeze 18.40 s/29 -> 13.44 s/18. Additive, so it
    // dominates off the floor and is negligible once B is near the link.
    //
    // k <= 0 disables all of this and restores the unbounded MI.
    double ack_ceil_k = 1.5;              // PUDICA_ACK_CEIL_K (0 = off)
    double ack_ceil_window_ms = 500.0;    // PUDICA_ACK_CEIL_WINDOW_MS
    double ack_ceil_headroom_mbps = 2.0;  // PUDICA_ACK_CEIL_HEADROOM_MBPS
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
    // [A30] Drain exit never lowers B below the drain it is leaving. ON by
    // default: the lowering case is a defect, not a tuning choice (see the
    // call site). Set PUDICA_RESTORE_NO_LOWER=0 to restore the literal
    // paper rule for an A/B.
    bool restore_no_lower = true; // PUDICA_RESTORE_NO_LOWER
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
      c.ack_ceil_k = d("PUDICA_ACK_CEIL_K", 1.5);
      c.ack_ceil_window_ms = d("PUDICA_ACK_CEIL_WINDOW_MS", 500.0);
      c.ack_ceil_headroom_mbps = d("PUDICA_ACK_CEIL_HEADROOM_MBPS", 2.0);
      c.drain_inflight = d("PUDICA_DRAIN_INFLIGHT", 0.0) != 0.0;
      c.restore_no_lower = d("PUDICA_RESTORE_NO_LOWER", 1.0) != 0.0;
      int64_t tau_ms = i64("PUDICA_TAU_RESET_MS", 5000);
      c.tau_reset_us = tau_ms * 1000;
      int64_t max_kbps = i64("PUDICA_MAX_RATE_KBPS", 0);
      if (max_kbps > 0) c.max_rate_bps = max_kbps * 1000;
      // Unset → 100 Mbps. Do not inherit BUR_MAX_RATE_MBPS (SCTP, default 1000).
      return c;
    }
  };

  struct Input {
    double frame_bur = 0.0;
    double smoothed_bur = 0.0;
    int64_t recv_rate_bps = 0;     // current windowed receiving_rate
    // [A39] The same receiving_rate, but 0 when it was not measured. Only the
    // delivered-rate ceiling reads it. recv_rate_bps cannot serve: the caller
    // substitutes committed_bps when there is no measurement, and a ceiling fed
    // its own output is 1.5 x committed + headroom -- MI compounding 1.5x per
    // step (run 1789042110, 27.22-27.98 s: 7.73 -> 100 Mbps on recv = 0).
    int64_t ack_recv_bps = 0;
    int64_t inflight_bytes = 0;    // paper: in-flight volume for draining_rate
    int64_t now_us = 0;
    int64_t frame_send_us = 0;     // first-packet send time of the BUR frame
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
    // Delivered-rate ceiling in force this tick, bps. -1 when ack_ceil_k <= 0.
    double ack_ceil_bps = -1.0;
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

    // [A32] Refresh the delivered-rate ceiling before any branch runs, so every
    // path below -- MI, AI-MD, HOLD, FALLBACK, the drain exit -- publishes
    // through it. Clamp() applies it.
    //
    // [A33] The snapshot is what makes the ceiling increase-only: Clamp() never
    // pushes a value below the B this decision started from.
    committed_at_entry_ = committed_bps;
    UpdateAckCeiling(in.ack_recv_bps, in.now_us);
    out.ack_ceil_bps = ack_ceil_bps_;

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
      // [A30] Drain exit is a RECOVERY step; it must never publish less than
      // the drain it is exiting. The paper writes "B <- receiving_rate" on the
      // assumption that receiving_rate reflects capacity, but at the instant
      // the queue clears it is still an average over the outage that just
      // ended, so it can read far BELOW what DRAIN itself computed.
      //
      // Measured, run 1789024044 at ctrl_t 54.43-54.48 (link 20.5 Mbps):
      //   PUD-DRAIN    recv=2.23  committed=8.20
      //   PUD-RESTORE  recv=1.85  committed=1.85   <- a 4.4x CUT on recovery
      // AI-MD then nudged it to 2.20 and it stayed there for 18.4 s while the
      // link ran 20-73 Mbps. 3 of 11 RESTOREs in that run cut rather than
      // raised; the same 3-of-9 in run 1789023940.
      //
      // Why the cut is not self-correcting: at 1080p a 2.2 Mbps target is
      // outside the encoder's operating range. Decoded QP pegged at 106/127
      // and libvpx dropped to 1-6 fps, so the encoder realised only 0.97 Mbps
      // (44% of target) -- which was below the mi_send_ratio gate in force at
      // the time, so MI could not raise B, so the resolution never recovered.
      // The run escaped only when QualityScaler finally reached 540p, 18.4 s
      // later. (That gate is gone as of [A32]; the cut itself is still wrong.)
      //
      // max() keeps the paper's intent (restore to the delivered rate when
      // that is the larger number) and removes only the case where the rule
      // fires backwards. DRAIN's own value is already conservative -- it is
      // alpha*recv minus the drain term -- and BUR < 1 means the queue is gone,
      // so holding it is safe.
      if (cfg.restore_no_lower && committed_bps > recv) {
        // keep committed_bps
      } else {
        committed_bps = recv;
      }
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
      // The paper's rule, and now the only one: the reporting frame must have
      // been sent at or after the decision. Monotonic in time, so it always
      // opens -- unlike the realisation gates removed in [A32], which could
      // hold for the length of a run.
      if (in.frame_send_us < mi_decision_now_us_)
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
    recv_window_.clear();
    ack_ceil_bps_ = -1.0;
    committed_at_entry_ = 0;
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
  // [A33] The delivered-rate ceiling bounds GROWTH only. It is floored at the
  // B this decision started from, so it can refuse an increase but can never
  // lower a B already committed.
  //
  // Why. recv is bounded by B -- the encoder cannot deliver more than it was
  // told -- so a ceiling of k x recv used in the DOWNWARD direction is a
  // self-reinforcing ratchet: one round of the loop gives
  //     B_next = k x (realisation x B)
  // which shrinks whenever the encoder realises less than 1/k of its target.
  // At k = 1.5 that threshold is 0.667, and right after a collapse -- keyframe
  // pending, resolution switch, encoder ramp -- realisation is always below it.
  // The ceiling therefore worked backwards at exactly the moment it was needed.
  //
  // Measured, run 1789032702 (q1s_attcampus_5g_122s), trace t=21-24 s with the
  // link at 55-103 Mbps and R~ = 0.006-0.04 (i.e. no queue at all):
  // realisation p50 = 0.54, loop gain 0.81, and committed tracked ack_ceil
  // frame for frame down 2.50 -> 2.28 -> 1.82 -> 1.49 -> 1.18 Mbps, including
  // on PUD-HOLD rows -- a hold that did not hold. The reference run 1787122944
  // sat at realisation p50 = 0.66, gain 0.99, and stalled flat at 5.7-5.9 Mbps
  // for four seconds over the same trace segment: k = 1.5 is a knife edge, not
  // a margin.
  //
  // Nothing is lost by giving up the downward direction. Every path that is
  // supposed to lower B -- DRAIN (Eq.11), the zeta fallback, next delay, the
  // drain-exit restore -- reads the QUEUE directly, and they write
  // committed_bps before Clamp() sees it, so their decreases pass through
  // untouched and the next decision's snapshot follows them down.
  //
  // cfg.max_rate_bps (the application cap) is unconditional and still applies.
  int64_t Clamp(int64_t b) const {
    int64_t ceiling = cfg.max_rate_bps;
    // [A39] >= 0, not > 0: -1 is "disabled"; 0 is a live ceiling (no measured
    // delivery and no headroom) and must mean "no increase", not "no limit".
    if (ack_ceil_bps_ >= 0.0) {
      ceiling = std::min(ceiling,
                         std::max(static_cast<int64_t>(ack_ceil_bps_),
                                  committed_at_entry_));
    }
    return std::max(cfg.min_rate_bps, std::min(ceiling, b));
  }

  // B <= k x max(recv over the window) + headroom. The max is taken over the
  // window AND the sample just pushed, so a single fresh sample can lift the
  // ceiling immediately; only the decay back down waits for the window to age
  // out. recv_rate_bps <= 0 (a feedback gap) contributes a zero sample like any
  // other. Once no positive measurement survives, headroom cannot manufacture
  // evidence of capacity: freeze increases while allowing queue-driven cuts.
  void UpdateAckCeiling(int64_t recv_rate_bps, int64_t now_us) {
    if (cfg.ack_ceil_k <= 0.0) {
      ack_ceil_bps_ = -1.0;
      return;
    }
    const int64_t window_us =
        static_cast<int64_t>(cfg.ack_ceil_window_ms * 1000.0);
    recv_window_.push_back({now_us, recv_rate_bps});
    while (!recv_window_.empty() &&
           now_us - recv_window_.front().us > window_us) {
      recv_window_.pop_front();
    }
    int64_t recv_max = recv_rate_bps;
    for (const auto& smp : recv_window_)
      recv_max = std::max(recv_max, smp.bps);
    ack_ceil_bps_ = recv_max > 0
        ? cfg.ack_ceil_k * static_cast<double>(recv_max) +
              cfg.ack_ceil_headroom_mbps * 1e6
        : 0.0;
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

  struct RecvSample {
    int64_t us;
    int64_t bps;
  };
  std::deque<RecvSample> recv_window_;
  double ack_ceil_bps_ = -1.0;
  int64_t committed_at_entry_ = 0;

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
