/*
 *  [FINESSE W1b] Sender-side per-frame egress ground truth.
 *
 *  ONE LINE PER FRAME, written where the frame's packets are actually handed
 *  to the transport, i.e. AFTER the pacer. It exists to close a gap that no
 *  other file in the rig can see.
 *
 *  Two numbers are already measured and they do not add up. Inside
 *  sender_stats.csv -- one file, one clock -- PLI received to keyframe ENCODED
 *  is 0-200 ms. In the emulator's session clock, PLI received to keyframe
 *  FULLY ARRIVED at the RAN is 1.4-1.9 s. So 1.2-1.9 s elapses after the
 *  encoder has already produced the bytes, and nothing recorded when those
 *  bytes left this process. The standing hypothesis is that the keyframe waits
 *  in the pacer queue behind a backlog built when capacity fell (the sender
 *  was emitting 124 Mbps in the 200 ms before the link dropped to 8 Mbps), but
 *  it is a hypothesis: encode time and RAN arrival time are the only two
 *  points we hold, and the whole 1.2-1.9 s lives between them. This file is
 *  the missing point.
 *
 *  IT IS A SECOND FILE ON PURPOSE. The obvious move is a column on
 *  sender_frames.csv, and it is wrong. That file is written at encode time and
 *  flushed per line because it defines the SET OF FRAMES that J divides by;
 *  holding its line back until egress would mean either buffering it -- and a
 *  buffered tail is simply lost to the harness's SIGKILL, which has already
 *  deleted 26 frames from the denominator once -- or keeping frame state alive
 *  indefinitely for frames whose packets never leave, which is exactly the
 *  population we are here to count. Two files joined on `rtp_timestamp` cost
 *  one merge in the analysis and keep both writes unconditional.
 *
 *  CLOCK. `first_packet_sent_ms` and `last_packet_sent_ms` are CLOCK_MONOTONIC
 *  milliseconds, the same clock as `capture_time_ms` in sender_frames.csv,
 *  because the point of the pair of files is to subtract them. The chain on
 *  the frame-log side is capturer -> VideoFrame::timestamp_us (documented in
 *  api/video/video_frame.h as "same timebase as rtc::TimeMicros()", and
 *  file_video_source.cc sets it to literally rtc::TimeMicros()) ->
 *  FrameEncodeMetadataWriter copies it to EncodedImage::capture_time_ms_. The
 *  chain on this side is RtpSenderEgress's Environment clock, which in
 *  production is RealTimeClock, whose CurrentTime() is also rtc::TimeMicros().
 *  rtc::TimeMicros() is clock_gettime(CLOCK_MONOTONIC) on POSIX. No offset
 *  estimation, no conversion, and no place for one to hide.
 *
 *  Truncated with us/1000 rather than Timestamp::ms(), which rounds. Rounding
 *  here and truncating there would put a systematic half-millisecond into
 *  every difference, and the differences we care about at the head of the
 *  distribution are single-digit milliseconds.
 *
 *  ORIGINAL TRANSMISSIONS ONLY. The caller passes media packets whose type is
 *  kVideo; retransmissions, padding and FEC never reach here. Counting a
 *  retransmission as part of the frame's egress would be actively misleading:
 *  a NACK answered 400 ms later would stretch that frame's
 *  first-to-last span by 400 ms of time during which the frame was not being
 *  sent at all, and the run where the rig collapsed produced 3,064
 *  retransmitted packets in three seconds, so this is not a rare corner.
 *  Audio is excluded by the same filter, and must be: it lives in a different
 *  RTP timestamp space and would join against video frames at random.
 *
 *  FRAMES THAT NEVER FINISH GET A LINE TOO, with `last_packet_sent_ms` empty.
 *  They are not an error case, they are the case the measurement exists for --
 *  the pacer drops a stream's queued packets when the first packet of a
 *  keyframe is enqueued (PacingController::EnqueuePacket, keyframe flushing),
 *  so during a dip the frames sitting in front of a keyframe are precisely the
 *  ones that get truncated. Dropping them silently would make the backlog look
 *  like it drained.
 *
 *  Such a frame is retired when the next frame's first packet leaves, not at
 *  shutdown: the harness stops the rig with SIGKILL and SIGKILL cannot be
 *  caught, so there is no shutdown hook to write from. The pacer is FIFO
 *  within a stream at a given priority, so once a packet of a newer frame has
 *  gone out, the older frame will get nothing more and can be closed on the
 *  spot. The cost of this is that the one frame still open per SSRC at the
 *  instant of the kill is missing from the file. That is one frame, and it is
 *  by construction the newest one.
 *
 *  Accumulation is keyed on SSRC and not on `rtp_timestamp` alone, because
 *  simulcast layers interleave packet-by-packet at this point and a single
 *  slot would retire each layer against the other every packet. The FILE is
 *  still keyed on `rtp_timestamp` alone and that is safe: each RTPSender seeds
 *  its own timestamp offset from random_.Rand<uint32_t>(), so two layers of
 *  the same capture carry different wire timestamps.
 *
 *  Join key is `rtp_timestamp`, and it is the POST-StartTimestamp value, the
 *  same one sender_frames.csv writes -- this is what is on the wire, and it is
 *  what RtpPacketToSend::Timestamp() already holds here.
 *
 *  Off unless an output directory is configured, and identical to upstream
 *  when off. Same env contract as FrameDelaySink and SenderFrameLog so one run
 *  configures all three.
 */

#ifndef MODULES_RTP_RTCP_SOURCE_FINESSE_EGRESS_LOG_H_
#define MODULES_RTP_RTCP_SOURCE_FINESSE_EGRESS_LOG_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <map>
#include <mutex>
#include <string>

namespace webrtc {
namespace finesse {

class SenderEgressLog {
 public:
  static SenderEgressLog& Get() {
    static SenderEgressLog* instance = new SenderEgressLog();
    return *instance;
  }

  // Called from the egress worker queue with the packet already accepted by
  // the transport. One RtpSenderEgress exists per SSRC and each has its own
  // queue, so this object takes its own lock rather than borrowing anyone's.
  // At 30 fps and a few hundred packets a frame the lock is uncontended in
  // practice, and a torn line would be worse than the cost.
  void OnMediaPacketSent(uint32_t ssrc,
                         uint32_t rtp_timestamp,
                         int64_t sent_ms,
                         size_t size_bytes,
                         bool is_last_packet_of_frame) {
    if (!enabled_)
      return;
    std::lock_guard<std::mutex> guard(mu_);
    if (!file_)
      return;

    OpenFrame& open = open_by_ssrc_[ssrc];
    if (open.packets_sent > 0 && open.rtp_timestamp != rtp_timestamp) {
      Emit(open, /*complete=*/false);
      open = OpenFrame();
    }
    if (open.packets_sent == 0) {
      open.rtp_timestamp = rtp_timestamp;
      open.first_ms = sent_ms;
    }
    open.last_ms = sent_ms;
    ++open.packets_sent;
    open.bytes_sent += size_bytes;

    // The marker bit is set by the packetizer on the last packet of the frame
    // (rtp_format_h264.cc and the other packetizers all do
    // SetMarker(packets_.empty())), so it is the frame boundary as the
    // receiver will see it, not a boundary we invented here.
    if (is_last_packet_of_frame) {
      Emit(open, /*complete=*/true);
      open = OpenFrame();
    }
  }

 private:
  struct OpenFrame {
    uint32_t rtp_timestamp = 0;
    int64_t first_ms = 0;
    int64_t last_ms = 0;
    uint32_t packets_sent = 0;
    size_t bytes_sent = 0;
  };

  SenderEgressLog() {
    const char* dir = getenv("FRAME_DELAY_CSV_DIR");
    if (!dir || !*dir)
      dir = getenv("UNIFIED_CSV_DIR");
    if (!dir || !*dir)
      return;
    std::string path = std::string(dir) + "/sender_egress.csv";
    file_ = fopen(path.c_str(), "w");
    if (!file_)
      return;
    fprintf(file_,
            "rtp_timestamp,first_packet_sent_ms,last_packet_sent_ms,"
            "packets_sent,bytes_sent\n");
    fflush(file_);
    enabled_ = true;
  }

  // Never destroyed on purpose (leaked singleton): a static destructor racing
  // with senders still running would close the file out from under them.

  void Emit(const OpenFrame& frame, bool complete) {
    fprintf(file_, "%u,%lld,", frame.rtp_timestamp,
            static_cast<long long>(frame.first_ms));
    // Empty rather than 0 or -1 for a frame that never finished: 0 is a
    // legitimate reading of a monotonic clock and -1 would be silently
    // averaged in by anything that does not know to look for it, whereas an
    // empty cell parses as NaN and forces the question.
    if (complete)
      fprintf(file_, "%lld", static_cast<long long>(frame.last_ms));
    fprintf(file_, ",%u,%zu\n", frame.packets_sent, frame.bytes_sent);
    // Flushed every line, for the reason SenderFrameLog is: the harness stops
    // the rig with SIGKILL and a buffered tail is lost. Here the tail is the
    // last second of the run, which during a dip is where every frame we are
    // arguing about lives -- and a frame present in sender_frames.csv but
    // absent here reads as "never sent", the exact conclusion this file is
    // supposed to establish or refute.
    fflush(file_);
  }

  std::mutex mu_;
  FILE* file_ = nullptr;
  bool enabled_ = false;
  std::map<uint32_t, OpenFrame> open_by_ssrc_;
};

}  // namespace finesse
}  // namespace webrtc

#endif  // MODULES_RTP_RTCP_SOURCE_FINESSE_EGRESS_LOG_H_
