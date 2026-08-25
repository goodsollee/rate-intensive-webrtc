/*
 *  [FINESSE W1a'] Sender-side per-frame ground truth.
 *
 *  ONE LINE PER ENCODED FRAME, written where the wire RTP timestamp becomes
 *  final. It exists to answer two questions that nothing else in the rig can.
 *
 *  1. WHAT THE ENCODER ACTUALLY PRODUCED. The objective J sums a per-frame
 *     penalty over every frame the encoder emitted, and the receiver's
 *     frame_delay.csv only has rows for frames that ARRIVED. The frames missing
 *     from it are exactly the ones J is supposed to charge for, so the set of
 *     frames cannot be recovered from the receiver alone. This file is that set.
 *
 *  2. THE TRUE CAPTURE TIME. The age of a frame is measured from capture, and
 *     the RAN can only estimate that offset (a per-session min-filter, which is
 *     biased low by the minimum one-way delay). Sender and receiver run on the
 *     same host here and capture_time_ms is on CLOCK_MONOTONIC, the same clock
 *     the receiver logs, so joining the two files gives the capture-to-delivery
 *     age with NO offset estimation at all. That turns "the min-filter bias is
 *     small and configuration-independent" from an assumption into a measured
 *     number -- which is the only thing that justifies reusing the same metric
 *     on a commercial stream, where the sender is not ours to instrument.
 *
 *  Join key is `rtp_timestamp`, and it is the POST-StartTimestamp value: the
 *  receiver logs what was on the wire, and the pre-offset value would differ by
 *  a random per-session constant and join zero rows.
 *
 *  Off unless an output directory is configured, and identical to upstream when
 *  off. Same env contract as FrameDelaySink so one run configures both.
 */

#ifndef CALL_FINESSE_FRAME_LOG_H_
#define CALL_FINESSE_FRAME_LOG_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mutex>
#include <string>

namespace webrtc {
namespace finesse {

class SenderFrameLog {
 public:
  static SenderFrameLog& Get() {
    static SenderFrameLog* instance = new SenderFrameLog();
    return *instance;
  }

  // Called with the mutex of the calling sender already held, but this object
  // is shared across simulcast streams and (in principle) across senders, so it
  // takes its own. One line per frame is far too little traffic for the lock to
  // matter, and a torn line would be worse than the cost.
  void OnFrame(uint32_t rtp_timestamp,
               int64_t capture_time_ms,
               size_t size_bytes,
               bool is_keyframe,
               size_t simulcast_index) {
    if (!enabled_)
      return;
    std::lock_guard<std::mutex> guard(mu_);
    if (!file_)
      return;
    fprintf(file_, "%u,%lld,%zu,%d,%zu\n", rtp_timestamp,
            static_cast<long long>(capture_time_ms), size_bytes,
            is_keyframe ? 1 : 0, simulcast_index);
    // Flushed every line, not every 30 as the receiver's sink does. The harness
    // stops the rig with SIGKILL, so a buffered tail is simply lost -- and this
    // file defines the SET OF FRAMES, so losing its tail does not make the score
    // slightly noisy, it deletes frames from the denominator and hides whatever
    // happened in the last second of the run. Measured: 26 frames vanished this
    // way, and they reappeared as frames the RAN had seen but the sender had no
    // record of, which reads as a join failure. One line per frame at 30 fps is
    // 30 writes a second; there is nothing to buy here.
    fflush(file_);
  }

 private:
  SenderFrameLog() {
    const char* dir = getenv("FRAME_DELAY_CSV_DIR");
    if (!dir || !*dir)
      dir = getenv("UNIFIED_CSV_DIR");
    if (!dir || !*dir)
      return;
    std::string path = std::string(dir) + "/sender_frames.csv";
    file_ = fopen(path.c_str(), "w");
    if (!file_)
      return;
    fprintf(file_,
            "rtp_timestamp,capture_time_ms,size_bytes,is_keyframe,"
            "simulcast_index\n");
    fflush(file_);
    enabled_ = true;
  }

  // Never destroyed on purpose (leaked singleton): a static destructor racing
  // with senders still running would close the file out from under them.

  std::mutex mu_;
  FILE* file_ = nullptr;
  bool enabled_ = false;
};

}  // namespace finesse
}  // namespace webrtc

#endif  // CALL_FINESSE_FRAME_LOG_H_
