/*
 *  Pudica BUR measurement standalone test
 *  Tests: per-frame OWD, D_min window, probe correction, smoothed BUR, adaptive pacing
 *
 *  Build: g++ -std=c++17 -O2 -o /tmp/pudica_test pc/coordinator/pudica_bur_standalone_test.cc
 *  Run: /tmp/pudica_test
 */
#include "pudica_rtp_rate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

static int failed_count = 0;
#define EXPECT_NEAR(a, b, tol) do { \
  double _a = (a), _b = (b), _t = (tol); \
  if (std::abs(_a - _b) > _t) { \
    fprintf(stderr, "  FAIL: %s ≈ %s (%.4f vs %.4f, tol=%.4f)\n", #a, #b, _a, _b, _t); \
    failed_count++; \
  } } while(0)
#define EXPECT_GT(a, b) do { if (!((a) > (b))) { fprintf(stderr, "  FAIL: %s > %s\n", #a, #b); failed_count++; } } while(0)
#define EXPECT_LT(a, b) do { if (!((a) < (b))) { fprintf(stderr, "  FAIL: %s < %s\n", #a, #b); failed_count++; } } while(0)
#define EXPECT_GE(a, b) do { if (!((a) >= (b))) { fprintf(stderr, "  FAIL: %s >= %s\n", #a, #b); failed_count++; } } while(0)
#define EXPECT_LE(a, b) do { if (!((a) <= (b))) { fprintf(stderr, "  FAIL: %s <= %s\n", #a, #b); failed_count++; } } while(0)
#define EXPECT_STREQ(a, b) do { \
  if (std::string(a) != std::string(b)) { \
    fprintf(stderr, "  FAIL: %s == %s (got '%s')\n", #a, #b, (a)); \
    failed_count++; \
  } } while(0)

// Minimal Pudica BUR computation (mirrors rtp_sctp_coordinator.cc)
struct PudicaBur {
  double L_ms = 16.67;  // 60fps frame interval
  double gamma_rho = 1.25;
  int num_probes = 4;

  // D_min tracking
  double d_min_ms = -1.0;
  std::deque<std::pair<int64_t, double>> owd_window;
  static constexpr int64_t kDminWindowMs = 10000;

  // Per-frame
  int64_t first_send_ms = -1;
  int64_t last_recv_ms = -1;

  // Probe results
  struct ProbeResult { double owd_ms; double h_ms; };
  std::deque<ProbeResult> probes;
  int64_t last_frame_recv_ms = 0;

  double rho = 2.0;

  void UpdateDmin(int64_t time_ms, double owd_ms) {
    owd_window.push_back({time_ms, owd_ms});
    while (!owd_window.empty() && (time_ms - owd_window.front().first) > kDminWindowMs)
      owd_window.pop_front();
    d_min_ms = owd_ms;
    for (auto& [t, d] : owd_window) d_min_ms = std::min(d_min_ms, d);
  }

  void OnMediaPacket(int64_t send_ms, int64_t recv_ms, bool is_last) {
    double owd = (double)(recv_ms - send_ms);
    UpdateDmin(recv_ms, owd);
    if (first_send_ms < 0) first_send_ms = send_ms;
    last_recv_ms = recv_ms;
    if (is_last) {
      // frame complete
    }
  }

  void OnProbePacket(int64_t send_ms, int64_t recv_ms) {
    double owd = (double)(recv_ms - send_ms);
    UpdateDmin(recv_ms, owd);
    double h = (last_frame_recv_ms > 0) ? (double)(recv_ms - last_frame_recv_ms) : owd;
    probes.push_back({owd, h});
  }

  double ComputeFrameBur() {
    if (first_send_ms < 0 || last_recv_ms < 0 || d_min_ms < 0) return -1;
    double D = (double)(last_recv_ms - first_send_ms);
    double R = std::max(0.0, (D - d_min_ms) / L_ms);

    // Probe correction
    if (!probes.empty()) {
      double T_packet = (1.0 - 1.0 / rho) * L_ms / (num_probes + 1);
      if (T_packet < 0.1) T_packet = 0.1;
      double correction = 0;
      for (auto& p : probes) {
        double excess = std::max(0.0, p.owd_ms - d_min_ms);
        double T_i = std::min({excess, p.h_ms, T_packet});
        if (T_i < 0) T_i = 0;
        correction += T_i;
      }
      R += correction / L_ms;
    }

    // Update ρ
    double r_clamped = std::max(0.01, std::min(R, 1.0));
    rho = gamma_rho / r_clamped;
    rho = std::max(1.0, std::min(rho, 10.0));

    // Reset for next frame
    last_frame_recv_ms = last_recv_ms;
    first_send_ms = -1;
    last_recv_ms = -1;
    probes.clear();

    return R;
  }
};

struct TestEntry { const char* name; void (*fn)(); };
std::deque<TestEntry> tests;
#define TEST(name) \
  static void test_##name(); \
  struct Reg_##name { Reg_##name() { tests.push_back({#name, test_##name}); } } r_##name; \
  void test_##name()

TEST(FrameOwdBasic) {
  PudicaBur bur;
  // Frame: 5 packets, send at t=0..4ms, recv at t=10..14ms → OWD ≈ 10ms
  for (int i = 0; i < 5; i++)
    bur.OnMediaPacket(i, 10 + i, i == 4);
  double R = bur.ComputeFrameBur();
  // D = 14 - 0 = 14ms, D_min = 10ms, L = 16.67ms
  // R = (14 - 10) / 16.67 = 0.24
  EXPECT_NEAR(R, 4.0 / 16.67, 0.01);
  printf("  BUR = %.3f (expected ~0.24)\n", R);
}

TEST(ZeroQueuing) {
  PudicaBur bur;
  // All packets have same OWD → D = D_min → R = 0
  for (int i = 0; i < 5; i++)
    bur.OnMediaPacket(i, 10 + i, i == 4);
  // D = 14, D_min = 10, but let's simulate zero queuing:
  // If all packets have OWD=10, D = last_recv - first_send = 14 - 0 = 14
  // Not exactly zero. For true zero queuing, need OWD = constant AND frame spread = L
  // Let's test with single-packet frame
  PudicaBur bur2;
  bur2.OnMediaPacket(0, 10, true);  // single packet
  double R = bur2.ComputeFrameBur();
  // D = 10, D_min = 10, R = 0
  EXPECT_NEAR(R, 0.0, 0.001);
  printf("  BUR = %.3f (expected 0.0)\n", R);
}

TEST(DminWindow) {
  PudicaBur bur;
  bur.UpdateDmin(0, 100.0);
  bur.UpdateDmin(5000, 50.0);
  bur.UpdateDmin(9000, 80.0);
  EXPECT_NEAR(bur.d_min_ms, 50.0, 0.01);
  // After 10s, first sample (100ms) should expire
  bur.UpdateDmin(11000, 90.0);
  EXPECT_NEAR(bur.d_min_ms, 50.0, 0.01);
  printf("  D_min = %.0f ms (expected 50)\n", bur.d_min_ms);
}

TEST(ProbeCorrection) {
  PudicaBur bur;
  // Frame with some queuing
  bur.OnMediaPacket(0, 15, false);
  bur.OnMediaPacket(1, 16, false);
  bur.OnMediaPacket(2, 17, true);
  bur.last_frame_recv_ms = 17;  // set manually for probes

  // Probes with cross-traffic induced delay
  bur.OnProbePacket(18, 35);  // OWD=17, D_min=15 → excess=2
  bur.OnProbePacket(20, 38);  // OWD=18 → excess=3

  double R_with_probes = bur.ComputeFrameBur();

  // Without probes: R = (17 - 15) / 16.67 = 0.12
  // With probes: additional correction > 0
  EXPECT_GT(R_with_probes, 0.12);
  printf("  BUR with probes = %.3f (should be > 0.12)\n", R_with_probes);
}

TEST(NoCrossTrafficProbes) {
  PudicaBur bur;
  bur.OnMediaPacket(0, 10, false);
  bur.OnMediaPacket(1, 11, true);
  bur.last_frame_recv_ms = 11;

  // Probes with same OWD as D_min → no correction
  bur.OnProbePacket(12, 22);  // OWD=10 = D_min → excess=0
  bur.OnProbePacket(14, 24);  // OWD=10 = D_min → excess=0

  double R = bur.ComputeFrameBur();
  // R = (11-0 - 10) / 16.67 = 0.06, no probe correction
  double R_base = 1.0 / 16.67;
  EXPECT_NEAR(R, R_base, 0.01);
  printf("  BUR without cross-traffic = %.3f (expected ~%.3f)\n", R, R_base);
}

TEST(AdaptivePacing) {
  PudicaBur bur;
  // Low BUR → high ρ
  bur.OnMediaPacket(0, 10, true);
  double R = bur.ComputeFrameBur();  // R ≈ 0
  // ρ = γ_ρ / min(R, 1) → clamped to max=10
  EXPECT_GE(bur.rho, 1.0);
  printf("  ρ after low BUR: %.2f (should be high)\n", bur.rho);

  // High BUR → low ρ (closer to 1)
  PudicaBur bur2;
  bur2.OnMediaPacket(0, 25, true);  // D=25, D_min=25
  // Need D > D_min, so add a smaller OWD first
  PudicaBur bur3;
  bur3.UpdateDmin(0, 5.0);  // D_min = 5
  bur3.first_send_ms = 0;
  bur3.last_recv_ms = 20;  // D = 20, R = (20-5)/16.67 = 0.9
  double R3 = bur3.ComputeFrameBur();
  EXPECT_NEAR(R3, 15.0 / 16.67, 0.05);
  EXPECT_LT(bur3.rho, 2.0);
  printf("  ρ after high BUR (R=%.2f): %.2f (should be close to γ_ρ/R)\n", R3, bur3.rho);
}

// ============================================================================
// Pudica NSDI'24 §4.2–§4.3 RTP rate controller
// ============================================================================
using webrtc::PudicaRtpRateCtrl;
using webrtc::PudicaBurSampleEq6;
using webrtc::PudicaSmoothBurEq6;

static PudicaRtpRateCtrl::Input MakeIn(double bur, double rtilde, int64_t recv,
                                       int64_t now_us, int64_t send_us,
                                       int64_t inflight = 0) {
  PudicaRtpRateCtrl::Input in;
  in.frame_bur = bur;
  in.smoothed_bur = rtilde;
  in.recv_rate_bps = recv;
  in.ack_recv_bps = recv;  // [A39] a measured recv, unless a test says otherwise
  in.inflight_bytes = inflight;
  in.now_us = now_us;
  in.frame_send_us = send_us;
  return in;
}

// J-251 (1)(3): low BUR is MI from committed B, not recv × 1.30.
TEST(RestoreOnlyAfterDrain) {
  PudicaRtpRateCtrl c;
  // [A32] Subject is the branch, not the delivered-rate ceiling: these
  // cases drive B deliberately above recv, which the ceiling exists to
  // stop. AckCeilingBoundsMi covers the ceiling itself.
  c.cfg.ack_ceil_k = 0.0;
  c.committed_bps = 50'000'000;
  auto o = c.Update(MakeIn(0.27, 0.27, 5'000'000, 200000, 200000));
  printf("  low-BUR without drain: mode=%s target=%.2f Mbps (committed was 50)\n",
         o.mode, o.target_bps / 1e6);
  EXPECT_GT(o.target_bps, 50'000'000);          // MI raises B
  EXPECT_LT(o.target_bps, 200'000'000);
  EXPECT_GT(o.target_bps, 10'000'000);          // must not snap to 1.30×5 Mbps
}

// Restore to receiving_rate happens only when leaving drain.
TEST(RestoreAfterDrain) {
  PudicaRtpRateCtrl c;
  c.committed_bps = 40'000'000;
  int64_t now = 0;
  PudicaRtpRateCtrl::Output o{};
  for (int i = 0; i < 3; ++i) {
    now += 200000;
    o = c.Update(MakeIn(2.0, 1.2, 20'000'000, now, now, 50'000));
  }
  printf("  after 3 high BUR: mode=%s draining=%d target=%.2f\n",
         o.mode, (int)o.draining, o.target_bps / 1e6);
  EXPECT_GT(std::string(o.mode) == "PUD-DRAIN" ? 1 : 0, 0);
  now += 200000;
  o = c.Update(MakeIn(0.4, 0.5, 18'000'000, now, now));
  printf("  drain exit: mode=%s target=%.2f (recv=18)\n", o.mode,
         o.target_bps / 1e6);
  EXPECT_STREQ(o.mode, "PUD-RESTORE");
  EXPECT_NEAR(o.target_bps / 1e6, 18.0, 0.01);
}

// Single BUR>1 is a 15% one-frame fallback; committed B is unchanged.
TEST(SingleHighIsFallbackNotDrain) {
  PudicaRtpRateCtrl c;
  c.committed_bps = 70'000'000;
  auto o = c.Update(MakeIn(6.15, 0.4, 50'000'000, 100000, 100000, 200000));
  printf("  one high BUR: mode=%s published=%.2f committed=%.2f\n",
         o.mode, o.target_bps / 1e6, o.committed_bps / 1e6);
  EXPECT_STREQ(o.mode, "PUD-FALLBACK");
  EXPECT_NEAR(o.target_bps / 1e6, 70.0 * 0.85, 0.05);
  EXPECT_NEAR(o.committed_bps / 1e6, 70.0, 0.01);
  // Next frame reverts, then MI from 70 not from 59.5.
  auto o2 = c.Update(MakeIn(0.2, 0.2, 50'000'000, 300000, 300000));
  printf("  after revert: mode=%s target=%.2f\n", o2.mode, o2.target_bps / 1e6);
  EXPECT_GT(o2.target_bps, 70'000'000);
}

// [A30] Drain exit must not CUT the target. Reproduces run 1789024044
// t=54.43-54.48: DRAIN had committed 8.20 Mbps, then recv momentarily read
// 1.85 (an average over the outage that just ended) and the literal paper rule
// republished 1.85 -- a 4.4x cut at the moment of recovery, which then held for
// 18.4 s because the encoder could not realise it and the MI gate stayed shut.
TEST(DrainExitNeverLowersTarget) {
  PudicaRtpRateCtrl c;
  int64_t now = 100000;
  // Enter drain while the link is still healthy, so DRAIN's own averaged recv
  // keeps committed well above what recv reads later (the real run: DRAIN held
  // 8.20 Mbps built from a recv average, then exit saw an instantaneous 1.85).
  for (int i = 0; i < 3; ++i) {
    now += 33000;
    c.Update(MakeIn(4.0, 3.0, 20'000'000, now, now, 2'000));
  }
  int64_t drained = c.committed_bps;
  printf("  after drain: committed=%.2f Mbps\n", drained / 1e6);
  // Queue clears, but recv is still the outage average and reads BELOW it.
  now += 33000;
  auto o = c.Update(MakeIn(0.21, 0.3, 1'850'000, now, now));
  printf("  drain exit: mode=%s committed=%.2f (recv was 1.85)\n",
         o.mode, o.committed_bps / 1e6);
  EXPECT_STREQ(o.mode, "PUD-RESTORE");
  EXPECT_GE(o.committed_bps, drained);
  // With the paper's literal rule the cut still happens.
  PudicaRtpRateCtrl c2;
  c2.cfg = c.cfg;
  c2.cfg.restore_no_lower = false;
  now = 100000;
  for (int i = 0; i < 3; ++i) {
    now += 33000;
    c2.Update(MakeIn(4.0, 3.0, 20'000'000, now, now, 2'000));
  }
  int64_t drained2 = c2.committed_bps;
  now += 33000;
  auto o2 = c2.Update(MakeIn(0.21, 0.3, 1'850'000, now, now));
  printf("  literal rule: committed=%.2f (was %.2f)\n",
         o2.committed_bps / 1e6, drained2 / 1e6);
  EXPECT_NEAR(o2.committed_bps / 1e6, 1.85, 0.01);
}

// Three consecutive BUR>1 enters Eq.11 drain.
TEST(DrainOnThreeConsecutive) {
  PudicaRtpRateCtrl c;
  c.committed_bps = 40'000'000;
  PudicaRtpRateCtrl::Output o{};
  int64_t now = 0;
  for (int i = 0; i < 3; ++i) {
    now += 200000;
    o = c.Update(MakeIn(2.5, 1.4, 30'000'000, now, now, 80'000));
  }
  printf("  3rd high BUR: mode=%s target=%.2f draining=%d\n", o.mode,
         o.target_bps / 1e6, (int)o.draining);
  EXPECT_GT(std::string(o.mode) == "PUD-DRAIN" ? 1 : 0, 0);
  EXPECT_LT(o.target_bps, 40'000'000);
}

// MI waits until a frame sent at/after the decision now.
TEST(MiWaitsForFeedback) {
  PudicaRtpRateCtrl c;
  // [A32] Subject is the branch, not the delivered-rate ceiling: these
  // cases drive B deliberately above recv, which the ceiling exists to
  // stop. AckCeilingBoundsMi covers the ceiling itself.
  c.cfg.ack_ceil_k = 0.0;
  c.committed_bps = 10'000'000;
  auto o1 = c.Update(MakeIn(0.2, 0.2, 8'000'000, 100000, 50000));
  int64_t after_mi = o1.target_bps;
  printf("  first MI: %s %.2f Mbps\n", o1.mode, after_mi / 1e6);
  EXPECT_GT(std::string(o1.mode) == "PUD-MI" ? 1 : 0, 0);
  auto o2 = c.Update(MakeIn(0.2, 0.2, 8'000'000, 120000, 50000));
  printf("  before feedback: %s %.2f Mbps\n", o2.mode, o2.target_bps / 1e6);
  EXPECT_GT(std::string(o2.mode) == "PUD-HOLD" ? 1 : 0, 0);
  EXPECT_NEAR(o2.target_bps / 1e6, after_mi / 1e6, 0.01);
  auto o3 = c.Update(MakeIn(0.2, 0.2, 8'000'000, 250000, 100000));
  printf("  after feedback: %s %.2f Mbps\n", o3.mode, o3.target_bps / 1e6);
  EXPECT_GT(std::string(o3.mode) == "PUD-MI" ? 1 : 0, 0);
  EXPECT_GT(o3.target_bps, after_mi);
}

// HOLD / AI-MD must not snap B to recv_rate.
TEST(AimdDoesNotSnapToRecv) {
  PudicaRtpRateCtrl c;
  // [A32] Subject is the branch, not the delivered-rate ceiling: these
  // cases drive B deliberately above recv, which the ceiling exists to
  // stop. AckCeilingBoundsMi covers the ceiling itself.
  c.cfg.ack_ceil_k = 0.0;
  c.committed_bps = 35'000'000;
  auto o = c.Update(MakeIn(0.95, 0.90, 12'000'000, 200000, 200000));
  printf("  AI-MD: mode=%s target=%.2f recv=12\n", o.mode, o.target_bps / 1e6);
  EXPECT_GT(std::string(o.mode) == "PUD-AIMD" ? 1 : 0, 0);
  EXPECT_GT(o.target_bps, 20'000'000);  // not 12 Mbps
  EXPECT_LT(std::abs(o.target_bps - 35'000'000), 10'000'000);
}

TEST(Eq6WeightsPreferRecent) {
  std::deque<PudicaBurSampleEq6> h;
  h.push_back({0, 0.1, 10e6});
  h.push_back({50000, 0.2, 10e6});
  h.push_back({100000, 0.9, 10e6});
  double r = PudicaSmoothBurEq6(h, 10e6, 100000, 200000);
  printf("  Rtilde=%.3f (should lean toward 0.9)\n", r);
  EXPECT_GT(r, 0.4);
  EXPECT_LT(r, 0.9);
}

// Thin ack window after a sweep must not be how B is set; the paper path
// keeps committed B. (Coordinator skip is the other half; here recv=committed.)
TEST(LowRecvDoesNotCollapseCommitted) {
  PudicaRtpRateCtrl c;
  // [A32] Subject is the branch, not the delivered-rate ceiling: these
  // cases drive B deliberately above recv, which the ceiling exists to
  // stop. AckCeilingBoundsMi covers the ceiling itself.
  c.cfg.ack_ceil_k = 0.0;
  c.committed_bps = 40'000'000;
  auto o = c.Update(MakeIn(0.27, 0.27, 2'000'000, 200000, 200000));
  printf("  recv glitch 2 Mbps, BUR low: mode=%s target=%.2f\n", o.mode,
         o.target_bps / 1e6);
  EXPECT_GT(o.target_bps, 20'000'000);
}

// Application cap: default 100 Mbps. MI from 90 would be ~155 without clamp.
TEST(AppCap100Mbps) {
  PudicaRtpRateCtrl c;
  // [A32] Subject is the branch, not the delivered-rate ceiling: these
  // cases drive B deliberately above recv, which the ceiling exists to
  // stop. AckCeilingBoundsMi covers the ceiling itself.
  c.cfg.ack_ceil_k = 0.0;
  EXPECT_NEAR(c.cfg.max_rate_bps / 1e6, 100.0, 0.01);
  c.committed_bps = 90'000'000;
  auto o = c.Update(MakeIn(0.27, 0.27, 50'000'000, 200000, 200000));
  printf("  MI from 90: mode=%s target=%.2f (cap=100)\n", o.mode,
         o.target_bps / 1e6);
  EXPECT_STREQ(o.mode, "PUD-MI");
  EXPECT_LE(o.target_bps, 100'000'000);
  EXPECT_NEAR(o.target_bps / 1e6, 100.0, 0.01);
  int64_t now = 200000;
  for (int i = 0; i < 6; ++i) {
    now += 200000;
    o = c.Update(MakeIn(0.27, 0.27, 50'000'000, now, now));
  }
  printf("  after more MI: target=%.2f\n", o.target_bps / 1e6);
  EXPECT_LE(o.target_bps, 100'000'000);
}

// [A32] The delivered-rate ceiling is the only bound MI has now that the two
// realisation gates are gone. Three properties, in the order they matter:
//   1. it bounds the level      -- B never exceeds k x max(recv) over the window
//   2. it does not deadlock     -- recv rises => the ceiling rises, so B follows
//   3. it survives a glitch     -- one low recv frame inside the window does not
//                                  pull the ceiling down, because it is a MAX
TEST(AckCeilingBoundsMi) {
  PudicaRtpRateCtrl c;
  // [A36] Subject is the multiplicative ceiling; the shipped 2 Mbps
  // additive headroom would mask it at these rates.
  c.cfg.ack_ceil_headroom_mbps = 0.0;
  EXPECT_NEAR(c.cfg.ack_ceil_k, 1.5, 1e-9);
  c.committed_bps = 8'000'000;
  int64_t now = 100000;
  // 1. Six MI steps at a steady 8 Mbps recv. Unbounded MI multiplies by
  //    (1 + xi) every step; the ceiling pins it at 1.5 x 8 = 12.
  PudicaRtpRateCtrl::Output o;
  for (int i = 0; i < 6; ++i) {
    now += 33000;
    o = c.Update(MakeIn(0.27, 0.27, 8'000'000, now, now));
  }
  printf("  6 MI steps at recv=8: mode=%s target=%.2f (ceil=%.2f)\n", o.mode,
         o.target_bps / 1e6, o.ack_ceil_bps / 1e6);
  EXPECT_NEAR(o.target_bps / 1e6, 12.0, 0.01);
  EXPECT_NEAR(o.ack_ceil_bps / 1e6, 12.0, 0.01);

  // 2. recv steps up to 40 Mbps: the ceiling must open, not hold.
  for (int i = 0; i < 6; ++i) {
    now += 33000;
    o = c.Update(MakeIn(0.27, 0.27, 40'000'000, now, now));
  }
  printf("  recv steps to 40: target=%.2f (ceil=%.2f)\n", o.target_bps / 1e6,
         o.ack_ceil_bps / 1e6);
  EXPECT_NEAR(o.ack_ceil_bps / 1e6, 60.0, 0.01);
  EXPECT_GT(o.target_bps, 12'000'000);

  // 3. One 2 Mbps glitch frame inside the 500 ms window leaves the max, and
  //    therefore the ceiling, where it was.
  now += 33000;
  o = c.Update(MakeIn(0.27, 0.27, 2'000'000, now, now));
  printf("  after 2 Mbps glitch: ceil=%.2f\n", o.ack_ceil_bps / 1e6);
  EXPECT_NEAR(o.ack_ceil_bps / 1e6, 60.0, 0.01);

  // 4. [A33] A blackout long enough to age the window out drops the CEILING,
  //    but must not drop B: lowering is DRAIN/fallback/next-delay's job, and
  //    they read the queue instead of the delivered rate.
  now += 600000;
  o = c.Update(MakeIn(0.27, 0.27, 2'000'000, now, now));
  printf("  after 600 ms blackout: ceil=%.2f target=%.2f\n",
         o.ack_ceil_bps / 1e6, o.target_bps / 1e6);
  EXPECT_NEAR(o.ack_ceil_bps / 1e6, 3.0, 0.01);
  EXPECT_NEAR(o.target_bps / 1e6, 60.0, 0.01);
}

// [A33] The defect the increase-only rule exists to remove, reproduced from
// run 1789032702 t=21-24 s: an empty queue (BUR ~ 0.03), a wide-open link, and
// an encoder realising ~54% of its target. Under the two-sided ceiling that is
// a loop gain of 1.5 x 0.54 = 0.81, and committed fell 2.50 -> 1.18 Mbps with
// MI firing the whole way.
TEST(AckCeilingDoesNotRatchetDown) {
  PudicaRtpRateCtrl c;
  // [A36] Subject is the multiplicative ceiling; the shipped 2 Mbps
  // additive headroom would mask it at these rates.
  c.cfg.ack_ceil_headroom_mbps = 0.0;
  c.committed_bps = 2'500'000;
  int64_t now = 100000;
  PudicaRtpRateCtrl::Output o;
  double low = 1e18;
  for (int i = 0; i < 30; ++i) {
    now += 33000;
    // recv = 54% of the committed rate, the measured realisation.
    int64_t recv = static_cast<int64_t>(0.54 * c.committed_bps);
    o = c.Update(MakeIn(0.03, 0.03, recv, now, now));
    low = std::min(low, o.target_bps / 1e6);
  }
  printf("  30 frames at realisation 0.54: target=%.2f (min seen %.2f)\n",
         o.target_bps / 1e6, low);
  // The ceiling may refuse to let B grow -- that is its job -- but it must
  // never have pushed B below where it started.
  EXPECT_GE(low, 2.49);
}

// [A39] Reproduced from run 1789042110 t=27.14-27.98 s. Right after a
// blackout the ack window holds 1-13 samples over 0-5 ms, the measured recv is
// 0, and the caller hands the controller committed_bps in its place. The
// ceiling must not take that for a delivered rate: fed its own output it is
// 1.5 x committed + headroom, and MI compounded 1.5x per step to the 100 Mbps
// cap (7.73 -> 13.59 -> 22.39 -> 35.58 -> 55.37 -> 85.06 -> 100).
TEST(AckCeilingIgnoresCommittedFallback) {
  PudicaRtpRateCtrl c;
  // [A36] Subject is the multiplicative ceiling; the shipped 2 Mbps
  // additive headroom would mask it at these rates.
  c.cfg.ack_ceil_headroom_mbps = 0.0;
  c.committed_bps = 3'620'000;
  int64_t now = 100000;
  // One measured frame (the RESTORE at 27.14 s read recv = 3.62).
  PudicaRtpRateCtrl::Output o = c.Update(MakeIn(0.03, 0.08, 3'620'000, now, now));
  // Then eight MI decisions ~125 ms apart with nothing measured, filled the way
  // rtp_sctp_coordinator.cc fills them.
  for (int i = 0; i < 8; ++i) {
    now += 125000;
    PudicaRtpRateCtrl::Input in =
        MakeIn(0.06, 0.08, c.committed_bps, now, now);
    in.ack_recv_bps = 0;
    o = c.Update(in);
  }
  printf("  8 MI steps on recv=0: mode=%s target=%.2f (ceil=%.2f)\n", o.mode,
         o.target_bps / 1e6, o.ack_ceil_bps / 1e6);
  // Bounded by the one delivered rate it has seen, 1.5 x 3.62 = 5.43.
  EXPECT_LE(o.target_bps, 5'430'001);
}

// [A41] Steady window of run 1789041793 (38-42 s): D - D_min p50 26 ms, span
// 14 ms against an Eq.2 span of 9.7 ms. The tail queued 12 ms, so the
// bottleneck was the slower of the two and Eq.1 applies whole: 0.78, where
// A10's (D - span - D_min) / L reads 0.36. Measured utilisation was 0.76.
TEST(UtilBurIsEq1WhenTheTailQueued) {
  const double L = 33333.0;
  double r = webrtc::PudicaUtilBur(/*D=*/126000, /*d_min=*/100000,
                                   /*span=*/14000, /*intended=*/9700, L);
  printf("  util BUR=%.3f (A10 would read %.3f)\n", r, (26000.0 - 14000.0) / L);
  EXPECT_NEAR(r, 26000.0 / L, 1e-9);
}

// [A41] An emission-limited frame: the tail arrived within the send-side
// timestamp resolution of its send, so the bottleneck kept up and D - D_min
// is the pacer's own span. Only the part beyond Eq.2's L/rho is removed --
// the rest is what Eq.2 asked for and belongs in Eq.1.
TEST(UtilBurRemovesOnlyThePacerOverrun) {
  const double L = 33333.0;
  double r = webrtc::PudicaUtilBur(/*D=*/120500, /*d_min=*/100000,
                                   /*span=*/20000, /*intended=*/6000, L);
  printf("  util BUR=%.3f (Eq.1 alone would read %.3f)\n", r, 20500.0 / L);
  EXPECT_NEAR(r, (20500.0 - 14000.0) / L, 1e-9);
}

// [A41] The post-DRAIN jump, reduced to Eq.6. Link C = 34 Mbps, committed
// B = 50, and the encoder in its VBV deficit producing a tenth of that. Each
// frame reads its true utilisation b_k / C. With B_k = the target (what the
// history stored before) R~ is the tiny b/C and MI fires; with B_k = the
// frame's realised bitrate, B/B_k puts every sample back at the current rate
// and R~ = B/C = 1.47 -- above alpha, so AI-MD, as the link is overloaded at B.
TEST(Eq6WithRealisedBitrateReadsBOverC) {
  const double C = 34e6, B = 50e6;
  std::deque<PudicaBurSampleEq6> as_target, as_realised;
  for (int i = 0; i < 6; ++i) {
    const double b_k = 0.1 * B * (0.8 + 0.08 * i);  // 4.0-6.0 Mbps frames
    const int64_t t = 30000 * i;
    as_target.push_back({t, b_k / C, B});
    as_realised.push_back({t, b_k / C, b_k});
  }
  double old_r = PudicaSmoothBurEq6(as_target, B, 150000, 200000);
  double new_r = PudicaSmoothBurEq6(as_realised, B, 150000, 200000);
  printf("  R~ with B_k=target %.3f, with B_k=realised %.3f (B/C=%.3f)\n",
         old_r, new_r, B / C);
  EXPECT_LT(old_r, 0.85);
  EXPECT_NEAR(new_r, B / C, 1e-6);
}

TEST(AckCeilingDefaultHeadroomNeedsMeasuredDelivery) {
  PudicaRtpRateCtrl c;
  c.committed_bps = 1'000'000;
  for (int i = 0; i < 8; ++i) {
    const int64_t now = 2'000'000 + i * 100'000;
    auto in = MakeIn(0.03, 0.03, c.committed_bps, now, now);
    in.ack_recv_bps = 0;
    const auto o = c.Update(in);
    EXPECT_NEAR(o.target_bps, 1'000'000, 0);
    EXPECT_NEAR(o.ack_ceil_bps, 0, 0);
  }
}

TEST(AckCeilingHeadroomExpiresWithLastMeasuredSample) {
  PudicaRtpRateCtrl c;
  c.committed_bps = 1'000'000;
  auto first = c.Update(MakeIn(0.03, 0.03, 2'000'000, 1'000'000, 1'000'000));
  EXPECT_GT(first.target_bps, 1'000'000);
  const int64_t held = c.committed_bps;
  auto in = MakeIn(0.03, 0.03, held, 2'000'000, 2'000'000);
  in.ack_recv_bps = 0;
  auto expired = c.Update(in);
  EXPECT_NEAR(expired.target_bps, held, 0);
  EXPECT_NEAR(expired.ack_ceil_bps, 0, 0);
}

// [MOT-EVAL5 L4S-PUDICA-CE] The GCC CE-brake law on Pudica's committed B.
TEST(CeBrakeCutsCommittedByHalfAlpha) {
  PudicaRtpRateCtrl c;
  c.committed_bps = 10'000'000;
  const int64_t after = c.ApplyCeBrake(0.4, 5'000'000);
  printf("  10 Mbps, alpha=0.4 -> %.3f Mbps\n", after / 1e6);
  EXPECT_NEAR(after, 8'000'000, 1);
  EXPECT_NEAR(c.committed_bps, 8'000'000, 1);
  EXPECT_GT(c.mi_pending() ? 1 : 0, 0);
  EXPECT_NEAR(c.mi_decision_now_us(), 5'000'000, 0);
}

TEST(CeBrakeFloorsAtMinRateAndNeverRaises) {
  PudicaRtpRateCtrl c;
  c.cfg.min_rate_bps = 1'000'000;
  c.committed_bps = 1'200'000;
  EXPECT_NEAR(c.ApplyCeBrake(1.0, 1), 1'000'000, 0);  // 0.6 Mbps -> floor
  c.committed_bps = 800'000;  // already below the floor: left alone
  EXPECT_NEAR(c.ApplyCeBrake(0.5, 2), 800'000, 0);
  c.committed_bps = 10'000'000;  // alpha > 1 is clamped to 1 -> x0.5
  EXPECT_NEAR(c.ApplyCeBrake(3.0, 3), 5'000'000, 1);
}

TEST(CeBrakeAlphaZeroOrNoCommittedIsNoop) {
  PudicaRtpRateCtrl c;
  c.committed_bps = 10'000'000;
  EXPECT_NEAR(c.ApplyCeBrake(0.0, 7), 10'000'000, 0);
  EXPECT_LT(c.mi_pending() ? 1 : 0, 1);
  EXPECT_NEAR(c.mi_decision_now_us(), 0, 0);
  PudicaRtpRateCtrl empty;
  EXPECT_NEAR(empty.ApplyCeBrake(0.5, 7), 0, 0);
  EXPECT_LT(empty.mi_pending() ? 1 : 0, 1);
}

// An MI step at t=1 s, a CE cut at t=2 s. A report on a frame sent at 1.5 s
// is feedback of the MI step, not of the cut: without the re-armed barrier it
// releases the MI wait and multiplies the cut B straight back up.
TEST(CeBrakeBlocksInFlightMiFromReRaising) {
  auto run = [](bool cut) {
    PudicaRtpRateCtrl c;
    c.cfg.ack_ceil_k = 0;  // isolate the barrier from the delivered-rate ceiling
    c.committed_bps = 10'000'000;
    auto o = c.Update(MakeIn(0.30, 0.30, 10'000'000, 1'000'000, 900'000));
    EXPECT_STREQ(o.mode, "PUD-MI");
    const int64_t after_mi = c.committed_bps;
    if (cut) c.ApplyCeBrake(0.5, 2'000'000);
    const int64_t before_report = c.committed_bps;
    o = c.Update(MakeIn(0.30, 0.30, 10'000'000, 2'050'000, 1'500'000));
    printf("  cut=%d after_mi=%.2f before=%.2f -> %s %.2f Mbps\n", cut ? 1 : 0,
           after_mi / 1e6, before_report / 1e6, o.mode, c.committed_bps / 1e6);
    return std::make_pair(std::string(o.mode), c.committed_bps);
  };
  auto no_cut = run(false);
  EXPECT_STREQ(no_cut.first.c_str(), "PUD-MI");  // control: barrier opens
  auto with_cut = run(true);
  EXPECT_STREQ(with_cut.first.c_str(), "PUD-HOLD");
  // The first post-cut frame reopens the barrier (paper wait, ~1 RTT).
  PudicaRtpRateCtrl c;
  c.cfg.ack_ceil_k = 0;
  c.committed_bps = 10'000'000;
  c.ApplyCeBrake(0.5, 2'000'000);
  EXPECT_NEAR(c.committed_bps, 7'500'000, 1);
  auto o = c.Update(MakeIn(0.30, 0.30, 7'500'000, 2'100'000, 2'010'000));
  EXPECT_STREQ(o.mode, "PUD-MI");
}

int main() {
  printf("=== Pudica BUR Standalone Tests ===\n\n");
  int pass = 0, fail = 0;
  for (auto& t : tests) {
    failed_count = 0;
    printf("[TEST] %s\n", t.name);
    t.fn();
    if (failed_count == 0) { printf("  PASS\n"); pass++; }
    else { printf("  FAILED (%d)\n", failed_count); fail++; }
  }
  printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);
  return fail > 0 ? 1 : 0;
}
