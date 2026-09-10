/*
 *  Copyright 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree.
 */
#ifndef PC_COORDINATOR_GECKO_DECIDER_H_
#define PC_COORDINATOR_GECKO_DECIDER_H_

// [Gecko S2 2026-09-10] The sender's flush / no-flush decision of Gecko
// (Zhang, Meng et al., INFOCOM'26 §III-E, "illustrative end-host
// implementation"), pure logic so it can be pinned by
// gecko_decider_standalone_test.cc without the WebRTC tree:
//
//   * the first alert is answered NO_FLUSH, the second FLUSH (unless an
//     I-frame is at risk);
//   * between alerts the caller counts N_bad: frames whose delay exceeded the
//     application tolerance, plus (after a flush) the frames the flush doomed;
//   * time-decayed averages N_F_bad / N_NF_bad are kept per decision kind;
//     "if the current decision was flush and N_F_bad > N_NF_bad the next is
//     no-flush; conversely ...", otherwise the current decision is repeated;
//   * the same command five times in a row forces the opposite one, so both
//     arms keep being measured.
//
// No WebRTC dependency: <cstdint> only.

#include <cstdint>

namespace webrtc {
namespace gecko {

enum class Decision : uint8_t { NO_FLUSH = 0, FLUSH = 1 };

struct DeciderConfig {
  int force_every = 5;    // paper: 5 consecutive identical commands
  double decay = 0.5;     // EWMA weight of the newest cycle (paper: "time-decayed")
};

struct AlertOutcome {
  Decision decision = Decision::NO_FLUSH;
  bool forced = false;     // the exploration rule flipped it
  bool blocked = false;    // FLUSH refused because an I-frame is at risk
  int cycle = 0;           // 1-based index of this alert
  int last_bad = -1;       // N_bad of the cycle just closed (-1: no cycle yet)
  double avg_f = -1.0;     // decayed N_F_bad (-1: never measured)
  double avg_nf = -1.0;    // decayed N_NF_bad
  int consecutive = 0;     // how many times in a row `decision` has now been issued
};

class Decider {
 public:
  explicit Decider(const DeciderConfig& c) : cfg_(c) {}

  // A frame whose delay exceeded the tolerance since the last alert.
  void OnBadFrame() { ++n_bad_; }
  // A frame doomed by an outstanding flush (paper: counted in N_F_bad).
  void OnFrameSkipped() { ++n_bad_; }
  int pendingBad() const { return n_bad_; }

  AlertOutcome OnAlert(bool kf_blocked) {
    AlertOutcome o;
    o.cycle = ++cycle_;
    // Close the cycle of the previous decision with what it cost.
    if (have_last_) {
      o.last_bad = n_bad_;
      if (last_ == Decision::FLUSH) {
        avg_f_ = have_f_ ? (1.0 - cfg_.decay) * avg_f_ + cfg_.decay * n_bad_ : n_bad_;
        have_f_ = true;
      } else {
        avg_nf_ = have_nf_ ? (1.0 - cfg_.decay) * avg_nf_ + cfg_.decay * n_bad_ : n_bad_;
        have_nf_ = true;
      }
    }
    Decision next;
    if (!have_last_) {
      next = Decision::NO_FLUSH;                       // first alert
    } else if (!have_f_) {
      next = Decision::FLUSH;                          // second: try the other arm
    } else if (!have_nf_) {
      next = Decision::NO_FLUSH;
    } else if (last_ == Decision::FLUSH && n_bad_ > avg_nf_) {
      next = Decision::NO_FLUSH;                       // flushing cost more than waiting did
    } else if (last_ == Decision::NO_FLUSH && n_bad_ > avg_f_) {
      next = Decision::FLUSH;                          // waiting cost more than flushing did
    } else {
      next = last_;
    }
    if (have_last_ && next == last_ && consecutive_ >= cfg_.force_every) {
      next = (last_ == Decision::FLUSH) ? Decision::NO_FLUSH : Decision::FLUSH;
      o.forced = true;
    }
    if (next == Decision::FLUSH && kf_blocked) {
      next = Decision::NO_FLUSH;
      o.blocked = true;
    }
    consecutive_ = (have_last_ && next == last_) ? consecutive_ + 1 : 1;
    last_ = next;
    have_last_ = true;
    n_bad_ = 0;
    o.decision = next;
    o.avg_f = have_f_ ? avg_f_ : -1.0;
    o.avg_nf = have_nf_ ? avg_nf_ : -1.0;
    o.consecutive = consecutive_;
    return o;
  }

 private:
  DeciderConfig cfg_;
  int n_bad_ = 0;
  int cycle_ = 0;
  bool have_last_ = false;
  Decision last_ = Decision::NO_FLUSH;
  int consecutive_ = 0;
  bool have_f_ = false, have_nf_ = false;
  double avg_f_ = 0.0, avg_nf_ = 0.0;
};

}  // namespace gecko
}  // namespace webrtc

#endif  // PC_COORDINATOR_GECKO_DECIDER_H_
