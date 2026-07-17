/*
 *  Feedback Timeout Standalone Test
 *  Tests: no-SACK → PROBE bug reproduction, timeout congestion signal,
 *         spike-applied-once, draining, recovery, idle/startup guards.
 *
 *  Build: g++ -std=c++17 -O2 -o /tmp/test_feedback_timeout pc/feedback_timeout_standalone_test.cc
 *  Run: /tmp/test_feedback_timeout
 */
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <deque>
#include <algorithm>
#include <string>

static int failed_count = 0;
#define EXPECT_NEAR(a, b, tol) do { \
  double _a = (a), _b = (b), _t = (tol); \
  if (std::abs(_a - _b) > _t) { \
    fprintf(stderr, "  FAIL: %s ~= %s (%.6f vs %.6f, tol=%.6f) at line %d\n", \
            #a, #b, _a, _b, _t, __LINE__); \
    failed_count++; \
  } } while(0)
#define EXPECT_GT(a, b) do { \
  double _a = (a), _b = (b); \
  if (!(_a > _b)) { \
    fprintf(stderr, "  FAIL: %s > %s (%.6f vs %.6f) at line %d\n", \
            #a, #b, _a, _b, __LINE__); \
    failed_count++; \
  } } while(0)
#define EXPECT_LT(a, b) do { \
  double _a = (a), _b = (b); \
  if (!(_a < _b)) { \
    fprintf(stderr, "  FAIL: %s < %s (%.6f vs %.6f) at line %d\n", \
            #a, #b, _a, _b, __LINE__); \
    failed_count++; \
  } } while(0)
#define EXPECT_EQ(a, b) do { \
  auto _a = (a); auto _b = (b); \
  if (_a != _b) { \
    fprintf(stderr, "  FAIL: %s == %s (%d vs %d) at line %d\n", \
            #a, #b, (int)_a, (int)_b, __LINE__); \
    failed_count++; \
  } } while(0)
#define EXPECT_TRUE(x) do { if (!(x)) { \
    fprintf(stderr, "  FAIL: %s at line %d\n", #x, __LINE__); \
    failed_count++; \
  } } while(0)
#define EXPECT_FALSE(x) do { if ((x)) { \
    fprintf(stderr, "  FAIL: !(%s) at line %d\n", #x, __LINE__); \
    failed_count++; \
  } } while(0)

// ============================================================
// Minimal harness mirroring rtp_sctp_coordinator rate control
// ============================================================

struct Config {
  double bur_threshold = 0.5;
  double max_mi_multiplier = 1.2;
  double spike_reduction = 0.85;
  double draining_target = 0.85;
  double ewma_alpha = 0.30;
  int64_t timeout_ms = 100;        // feedback timeout (was 300, now 100)
  int64_t min_rate_bps = 1'000'000;
  int64_t max_rate_bps = 1'000'000'000;
};

// ACK sample for sliding window
struct AckSample {
  int64_t timestamp_ms;
  int64_t bytes;
};

// Simulated coordinator state
struct CoordinatorState {
  Config config;
  int64_t pacing_rate_bps = 50'000'000;   // 50 Mbps
  int64_t receiving_rate_bps = 50'000'000; // 50 Mbps
  double smoothed_bur = 0.0;
  bool ewma_initialized = false;
  int congestion_count = 0;
  bool spike_applied = false;
  bool timeout_was_draining = false;
  int64_t last_sack_time_us = 0;
  std::string last_mode = "";

  // ACK sliding window (200ms)
  std::deque<AckSample> ack_samples;
  static constexpr int64_t kAckWindowMs = 200;

  int64_t GetAckRateBps(int64_t now_ms) const {
    if (ack_samples.empty()) return 0;
    int64_t oldest_ms = ack_samples.front().timestamp_ms;
    int64_t newest_ms = ack_samples.back().timestamp_ms;
    int64_t window_ms = newest_ms - oldest_ms;
    if (window_ms <= 0) window_ms = kAckWindowMs;
    int64_t total_bytes = 0;
    for (auto& s : ack_samples) total_bytes += s.bytes;
    return (total_bytes * 8 * 1000) / window_ms;
  }

  void AddAckSample(int64_t now_ms, int64_t bytes) {
    ack_samples.push_back({now_ms, bytes});
    while (!ack_samples.empty() &&
           (now_ms - ack_samples.front().timestamp_ms) > kAckWindowMs * 2) {
      ack_samples.pop_front();
    }
  }

  // --- Timeout check (NEW: last_sack_time_us_ based) ---
  bool CheckTimeouts(int64_t now_us) {
    // Don't timeout before first SACK (startup)
    if (last_sack_time_us == 0) return false;

    // SACK arriving normally
    if ((now_us - last_sack_time_us) < config.timeout_ms * 1000) return false;

    // Don't timeout during idle (no SCTP data flowing)
    int64_t now_ms = now_us / 1000;
    if (GetAckRateBps(now_ms) <= 0) return false;

    // Timeout: no SACK for too long
    OnCongestionTimeout(now_us);
    last_sack_time_us = now_us;  // prevent repeated firing per interval
    return true;
  }

  // --- Congestion timeout (spike/draining, spike-applied-once) ---
  void OnCongestionTimeout(int64_t now_us) {
    congestion_count++;

    int64_t old_rate = pacing_rate_bps;
    int64_t recv_rate = receiving_rate_bps > 0 ? receiving_rate_bps : old_rate;
    int64_t new_rate;

    if (congestion_count <= 2) {
      // Spike phase: apply ONCE
      if (!spike_applied) {
        new_rate = static_cast<int64_t>(recv_rate * config.spike_reduction);
        new_rate = std::max(config.min_rate_bps, std::min(config.max_rate_bps, new_rate));
        pacing_rate_bps = new_rate;
        spike_applied = true;
        last_mode = "TIMEOUT-SPIKE";
      } else {
        last_mode = "TIMEOUT-SPIKE-SKIP";
      }
    } else {
      // Draining phase
      timeout_was_draining = true;
      int64_t drain_rate = std::max(int64_t{0}, old_rate - recv_rate);
      new_rate = static_cast<int64_t>(config.draining_target * recv_rate - drain_rate);
      new_rate = std::max(config.min_rate_bps, std::min(config.max_rate_bps, new_rate));
      pacing_rate_bps = new_rate;
      last_mode = "TIMEOUT-DRAIN";
    }
  }

  // --- SACK received → recovery ---
  void OnSackReceived(int64_t now_us, int64_t recv_rate_bps) {
    last_sack_time_us = now_us;
    receiving_rate_bps = recv_rate_bps;

    if (congestion_count > 0 || timeout_was_draining) {
      // Recovery: restore rate if draining reduced it below recv_rate
      if (timeout_was_draining && pacing_rate_bps < recv_rate_bps) {
        pacing_rate_bps = recv_rate_bps;
        last_mode = "TIMEOUT-RECOVER";
      }
      congestion_count = 0;
      spike_applied = false;
      timeout_was_draining = false;
    }
  }

  // --- BUR EWMA update ---
  void UpdateBurEwma(double bur) {
    if (!ewma_initialized) {
      smoothed_bur = bur;
      ewma_initialized = true;
    } else {
      smoothed_bur = config.ewma_alpha * bur +
                     (1.0 - config.ewma_alpha) * smoothed_bur;
    }
  }

  // --- AdjustPacingRate (simplified, mirrors real code) ---
  std::string AdjustPacingRate(double bur_raw) {
    constexpr double kEpsilon = 0.01;
    double r_tilde = smoothed_bur;
    int64_t old_rate = pacing_rate_bps;
    int64_t recv_rate = receiving_rate_bps > 0 ? receiving_rate_bps : old_rate;
    int64_t new_rate = old_rate;
    std::string mode = "STABLE";

    if (bur_raw > 1.0) {
      new_rate = static_cast<int64_t>(recv_rate * 0.85);
      mode = "RAW-CORR";
    } else if (bur_raw > config.bur_threshold) {
      // AI-MD (simplified)
      double delta = config.spike_reduction * (0.9 - r_tilde) * recv_rate;
      new_rate = static_cast<int64_t>(recv_rate + delta);
      mode = "AI-MD";
    } else if (r_tilde > kEpsilon) {
      // MI
      double scale = std::max(0.25, 1.0 - (r_tilde / config.bur_threshold) * 0.75);
      double step = 0.12 * scale * (0.9 - r_tilde) * old_rate;
      double max_step = 0.08 * scale * old_rate;
      step = std::max(-max_step, std::min(max_step, step));
      new_rate = static_cast<int64_t>(old_rate + step);
      mode = "MI";
    } else {
      // PROBE: BUR ~= 0
      if (recv_rate > old_rate * config.max_mi_multiplier) {
        new_rate = recv_rate;
      } else {
        new_rate = static_cast<int64_t>(old_rate * config.max_mi_multiplier);
      }
      mode = "PROBE";
    }

    new_rate = std::max(config.min_rate_bps, std::min(config.max_rate_bps, new_rate));
    pacing_rate_bps = new_rate;
    last_mode = mode;
    return mode;
  }
};

// ============================================================
// Test infrastructure
// ============================================================

struct TestEntry { const char* name; void (*fn)(); };
std::deque<TestEntry> tests;
#define TEST(name) \
  static void test_##name(); \
  struct Reg_##name { Reg_##name() { tests.push_back({#name, test_##name}); } } r_##name; \
  void test_##name()

// ============================================================
// Test 1: Reproduce no-SACK → PROBE bug
// ============================================================
TEST(NoSackProbeRepro) {
  CoordinatorState s;
  s.pacing_rate_bps = 55'000'000;  // 55 Mbps (stable before WiFi stall)
  s.receiving_rate_bps = 55'000'000;
  s.last_sack_time_us = 1'000'000;  // last SACK at t=1s

  // Simulate: TWCC arrives every 33ms with small bur_rtp, NO SACKs
  // Each call = one TWCC feedback with bur = bur_rtp ≈ 0.02
  double bur_rtp = 0.02;
  int probe_count = 0;
  for (int i = 0; i < 10; i++) {
    s.UpdateBurEwma(bur_rtp);
    std::string mode = s.AdjustPacingRate(bur_rtp);
    if (mode == "PROBE") probe_count++;
  }

  // BUG: without timeout fix, rate increases (MI or PROBE) during no-SACK.
  // Whether MI or PROBE doesn't matter — rate going UP during congestion is wrong.
  EXPECT_GT(s.pacing_rate_bps, 55'000'000);  // rate increased (BAD during congestion)
  printf("  Bug repro: rate %.1f Mbps > 55 (MI/PROBE increased during no-SACK)\n",
         s.pacing_rate_bps / 1e6);
}

// ============================================================
// Test 2: Timeout fires before PROBE, reduces rate
// ============================================================
TEST(TimeoutFiresBeforeProbe) {
  CoordinatorState s;
  s.pacing_rate_bps = 55'000'000;
  s.receiving_rate_bps = 55'000'000;
  s.last_sack_time_us = 1'000'000;  // last SACK at t=1.0s

  // Add ack samples so GetAckRateBps > 0 (recent SACKs in window)
  s.AddAckSample(900, 100'000);
  s.AddAckSample(1000, 100'000);

  // At t=1.101s (101ms after last SACK) → should timeout
  int64_t now_us = 1'101'000;
  bool fired = s.CheckTimeouts(now_us);
  EXPECT_TRUE(fired);

  // Rate should be reduced, not increased
  int64_t expected = static_cast<int64_t>(55'000'000 * 0.85);  // ~46.75M
  EXPECT_LT(s.pacing_rate_bps, 55'000'000);
  EXPECT_NEAR(s.pacing_rate_bps, expected, 1'000'000);
  EXPECT_EQ(s.congestion_count, 1);
  printf("  Timeout at 101ms: rate %.1f Mbps (expected ~%.1f)\n",
         s.pacing_rate_bps / 1e6, expected / 1e6);
}

// ============================================================
// Test 3: Spike applied once (not on repeated timeouts)
// ============================================================
TEST(SpikeAppliedOnce) {
  CoordinatorState s;
  s.pacing_rate_bps = 50'000'000;
  s.receiving_rate_bps = 50'000'000;
  s.last_sack_time_us = 1'000'000;
  s.AddAckSample(900, 100'000);
  s.AddAckSample(1000, 100'000);

  // First timeout at t=1.101s
  s.CheckTimeouts(1'101'000);
  int64_t rate_after_first = s.pacing_rate_bps;
  EXPECT_TRUE(s.spike_applied);
  EXPECT_EQ(s.congestion_count, 1);

  // Second timeout at t=1.202s (spike applied, should NOT reduce again)
  s.last_sack_time_us = 1'101'000;  // reset by CheckTimeouts
  s.CheckTimeouts(1'202'000);
  int64_t rate_after_second = s.pacing_rate_bps;
  EXPECT_EQ(s.congestion_count, 2);
  EXPECT_EQ(rate_after_first, rate_after_second);  // rate unchanged in 2nd spike
  printf("  Spike once: 1st=%.1f, 2nd=%.1f (same = correct)\n",
         rate_after_first / 1e6, rate_after_second / 1e6);
}

// ============================================================
// Test 4: Draining phase after 3+ timeouts
// ============================================================
TEST(DrainingPhase) {
  CoordinatorState s;
  s.pacing_rate_bps = 50'000'000;
  s.receiving_rate_bps = 40'000'000;  // recv < pacing (buffering)
  s.last_sack_time_us = 1'000'000;
  s.AddAckSample(900, 100'000);
  s.AddAckSample(1000, 100'000);

  // 3 timeouts to enter draining
  for (int i = 0; i < 3; i++) {
    s.CheckTimeouts(s.last_sack_time_us + 101'000);
  }

  EXPECT_EQ(s.congestion_count, 3);
  EXPECT_TRUE(s.timeout_was_draining);

  // Draining: new_rate = 0.85 * recv - (pacing - recv)
  // After spike: pacing = 40M * 0.85 = 34M, recv = 40M
  // 3rd timeout: drain_rate = max(0, 34M - 40M) = 0
  //              new_rate = 0.85 * 40M - 0 = 34M
  EXPECT_LT(s.pacing_rate_bps, 40'000'000);
  printf("  Draining: rate=%.1f Mbps, draining=%s\n",
         s.pacing_rate_bps / 1e6, s.timeout_was_draining ? "yes" : "no");
}

// ============================================================
// Test 5: Recovery when SACK arrives after draining
// ============================================================
TEST(RecoveryOnSack) {
  CoordinatorState s;
  s.pacing_rate_bps = 50'000'000;
  s.receiving_rate_bps = 50'000'000;
  s.last_sack_time_us = 1'000'000;
  s.AddAckSample(900, 100'000);
  s.AddAckSample(1000, 100'000);

  // Drive into draining (3 timeouts)
  for (int i = 0; i < 3; i++) {
    s.CheckTimeouts(s.last_sack_time_us + 101'000);
  }
  int64_t rate_during_drain = s.pacing_rate_bps;
  EXPECT_TRUE(s.timeout_was_draining);
  EXPECT_LT(rate_during_drain, 50'000'000);

  // SACK arrives! recv_rate = 48 Mbps (congestion clearing)
  s.OnSackReceived(2'000'000, 48'000'000);

  // Recovery should restore rate to recv_rate
  EXPECT_EQ(s.congestion_count, 0);
  EXPECT_FALSE(s.spike_applied);
  EXPECT_FALSE(s.timeout_was_draining);
  EXPECT_EQ(s.pacing_rate_bps, 48'000'000);
  printf("  Recovery: rate %.1f → %.1f Mbps (recv_rate restore)\n",
         rate_during_drain / 1e6, s.pacing_rate_bps / 1e6);
}

// ============================================================
// Test 6: Idle guard — no timeout when SCTP is idle
// ============================================================
TEST(IdleGuard) {
  CoordinatorState s;
  s.pacing_rate_bps = 50'000'000;
  s.receiving_rate_bps = 50'000'000;
  s.last_sack_time_us = 1'000'000;
  // NO ack_samples → GetAckRateBps() == 0 (idle)

  bool fired = s.CheckTimeouts(1'200'000);  // 200ms after last SACK
  EXPECT_FALSE(fired);
  EXPECT_EQ(s.pacing_rate_bps, 50'000'000);  // rate unchanged
  printf("  Idle guard: no timeout, rate unchanged at %.1f Mbps\n",
         s.pacing_rate_bps / 1e6);
}

// ============================================================
// Test 7: Startup guard — no timeout before first SACK
// ============================================================
TEST(StartupGuard) {
  CoordinatorState s;
  s.pacing_rate_bps = 15'000'000;  // initial ramp-up
  s.receiving_rate_bps = 0;
  s.last_sack_time_us = 0;  // no SACK yet
  s.AddAckSample(100, 50'000);  // some ack samples

  bool fired = s.CheckTimeouts(500'000);  // 500ms into session
  EXPECT_FALSE(fired);
  EXPECT_EQ(s.pacing_rate_bps, 15'000'000);  // rate unchanged
  printf("  Startup guard: no timeout before first SACK\n");
}

// ============================================================
// Main
// ============================================================
int main() {
  printf("=== Feedback Timeout Standalone Tests ===\n\n");
  int pass = 0, fail = 0;
  for (auto& t : tests) {
    failed_count = 0;
    printf("[TEST] %s\n", t.name);
    t.fn();
    if (failed_count == 0) { printf("  PASS\n"); pass++; }
    else { printf("  FAILED (%d assertions)\n", failed_count); fail++; }
  }
  printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);
  return fail > 0 ? 1 : 0;
}
