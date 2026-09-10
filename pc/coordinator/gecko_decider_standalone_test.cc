// [Gecko S8] Standalone unit for pc/coordinator/gecko_decider.h.
//
//  Build: g++ -std=c++17 -O2 -I. -o /tmp/gecko_decider_test \
//             pc/coordinator/gecko_decider_standalone_test.cc && /tmp/gecko_decider_test
#include "pc/coordinator/gecko_decider.h"

#include <cstdio>

using webrtc::gecko::AlertOutcome;
using webrtc::gecko::Decider;
using webrtc::gecko::DeciderConfig;
using webrtc::gecko::Decision;

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, what)                                                   \
  do {                                                                      \
    g_checks++;                                                             \
    if (!(cond)) {                                                          \
      g_fails++;                                                            \
      std::fprintf(stderr, "gecko_decider_test: FAIL %s (line %d)\n", what, \
                   __LINE__);                                               \
    }                                                                       \
  } while (0)

int main() {
  DeciderConfig cfg;   // force_every 5, decay 0.5

  // 1. First alert -> NO_FLUSH, second -> FLUSH (the paper's opening moves).
  {
    Decider d(cfg);
    AlertOutcome a = d.OnAlert(false);
    CHECK(a.decision == Decision::NO_FLUSH, "first alert is no-flush");
    CHECK(a.cycle == 1 && a.last_bad == -1, "no cycle closed on the first alert");
    d.OnBadFrame(); d.OnBadFrame(); d.OnBadFrame();          // waiting cost 3
    AlertOutcome b = d.OnAlert(false);
    CHECK(b.decision == Decision::FLUSH, "second alert tries flush");
    CHECK(b.last_bad == 3 && b.avg_nf == 3.0 && b.avg_f < 0, "no-flush cycle closed with 3");
  }

  // 2. Comparison: flush cost more than the no-flush average -> back to no-flush;
  //    no-flush cost more than the flush average -> flush.
  {
    Decider d(cfg);
    d.OnAlert(false);                                          // NF
    d.OnBadFrame(); d.OnBadFrame();                            // NF cost 2
    AlertOutcome f = d.OnAlert(false);                         // F
    CHECK(f.decision == Decision::FLUSH, "second is flush");
    d.OnFrameSkipped(); d.OnFrameSkipped(); d.OnFrameSkipped(); d.OnBadFrame();   // F cost 4 > 2
    AlertOutcome n = d.OnAlert(false);
    CHECK(n.decision == Decision::NO_FLUSH, "flush cost 4 > avg_nf 2 -> no-flush");
    CHECK(n.avg_f == 4.0, "avg_f seeded with 4");
    d.OnBadFrame();                                            // NF cost 1 <= avg_f 4
    AlertOutcome n2 = d.OnAlert(false);
    CHECK(n2.decision == Decision::NO_FLUSH, "no-flush cost 1 <= avg_f 4 -> stay no-flush");
    CHECK(n2.avg_nf == 1.5, "avg_nf decayed: 0.5*2 + 0.5*1");
    for (int i = 0; i < 6; ++i) d.OnBadFrame();               // NF cost 6 > avg_f 4
    AlertOutcome f2 = d.OnAlert(false);
    CHECK(f2.decision == Decision::FLUSH, "no-flush cost 6 > avg_f 4 -> flush");
    CHECK(f2.consecutive == 1, "consecutive resets on a switch");
  }

  // 3. I-frame guard: a FLUSH verdict becomes NO_FLUSH with blocked=true, and
  //    the blocked no-flush is what the next comparison closes.
  {
    Decider d(cfg);
    d.OnAlert(false);
    AlertOutcome b = d.OnAlert(true);
    CHECK(b.decision == Decision::NO_FLUSH && b.blocked, "flush blocked by the keyframe guard");
    AlertOutcome c = d.OnAlert(false);
    CHECK(c.decision == Decision::FLUSH && !c.blocked, "unblocked: flush still untried -> flush");
  }

  // 4. Forced exploration: five identical commands in a row force the opposite.
  {
    Decider d(cfg);
    d.OnAlert(false);                       // NF #1
    d.OnAlert(false);                       // F  #1 (avg_nf 0)
    // flush costs 0 every cycle: comparison keeps FLUSH (0 > 0 is false)
    AlertOutcome o;
    int flush_run = 1;
    for (int i = 0; i < 4; ++i) {
      o = d.OnAlert(false);
      CHECK(o.decision == Decision::FLUSH && !o.forced, "flush repeats while it costs nothing");
      ++flush_run;
    }
    CHECK(flush_run == 5 && o.consecutive == 5, "five consecutive flushes");
    o = d.OnAlert(false);
    CHECK(o.decision == Decision::NO_FLUSH && o.forced, "sixth is forced to no-flush");
    CHECK(o.consecutive == 1, "forced switch restarts the run");
    o = d.OnAlert(false);
    CHECK(o.decision == Decision::NO_FLUSH && !o.forced, "no-flush cost 0 is not > avg_f 0 -> no-flush repeats");
  }

  std::printf("gecko_decider_test: %d checks, %d failed\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
