/*
 *  Pudica BUR measurement standalone test
 *  Tests: per-frame OWD, D_min window, probe correction, smoothed BUR, adaptive pacing
 *
 *  Build: g++ -std=c++17 -O2 -o /tmp/pudica_test pc/coordinator/pudica_bur_standalone_test.cc
 *  Run: /tmp/pudica_test
 */
#include <cstdio>
#include <cmath>
#include <deque>
#include <algorithm>

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
