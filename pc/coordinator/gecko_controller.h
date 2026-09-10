/*
 *  Copyright 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree.
 */
#ifndef PC_COORDINATOR_GECKO_CONTROLLER_H_
#define PC_COORDINATOR_GECKO_CONTROLLER_H_

// [Gecko S3/S4/S5/S7 2026-09-10] The sender half of Gecko (Zhang, Meng et
// al., INFOCOM'26), one process-wide controller that the four touch points
// call into (docs/GECKO_PLAN.md in open-ran-emulator):
//
//   pc/rtp_transport.cc            OnRtcpDatagram()   echo detection, BEFORE
//                                                     SRTP unprotect (a copy
//                                                     is a replay there)
//   goog_cc/delay_based_bwe.cc     OnPacketFeedback() per-frame delay -> N_bad
//   rtp_sender_video.cc            FlagForFrame()     the 2-bit answer, on the
//                                                     keyframe (flush) or the
//                                                     next frame (no-flush)
//   video/video_stream_encoder.cc  SetKeyFrameRequester()  flush -> keyframe now
//
// Header-only (inline singleton), one mutex, env-configured (GECKO_*), off
// unless GECKO_ENABLED=1: every entry point then returns after one bool test.
// Logs "[GECKO] ..." lines to stderr (sender.log) and gecko_sender.csv into
// UNIFIED_CSV_DIR, the same directory the other sender CSVs use.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

#include "pc/coordinator/gecko_decider.h"

namespace webrtc {
namespace gecko {

// Flag values on the wire (GeckoFlagExtension, low 2 bits).
constexpr uint8_t kFlagNone = 0;
constexpr uint8_t kFlagNoFlush = 1;
constexpr uint8_t kFlagFlush = 2;

struct ControllerConfig {
  bool enabled = false;
  double tolerance_ms = 200.0;         // paper §III-E: 200 ms
  int64_t echo_window_us = 5000;       // two identical RTCP within this = alert
  bool kf_on_flush = true;             // decision: flush -> keyframe now, flag rides on it
  int64_t min_kf_interval_us = 500000; // I-frame guard: no flush this soon after a keyframe
  int force_every = 5;                 // paper: 5 identical commands force the opposite
  double decay = 0.5;                  // decayed-average weight
  int64_t pending_kf_max_us = 1000000; // a keyframe that never comes: give the flag up

  static ControllerConfig FromEnv() {
    auto envInt = [](const char* k, long long d) -> long long {
      const char* v = std::getenv(k);
      if (!v || !*v) return d;
      char* e = nullptr;
      const long long n = std::strtoll(v, &e, 10);
      return (e && *e == '\0') ? n : d;
    };
    auto envDbl = [](const char* k, double d) -> double {
      const char* v = std::getenv(k);
      if (!v || !*v) return d;
      char* e = nullptr;
      const double x = std::strtod(v, &e);
      return (e && *e == '\0') ? x : d;
    };
    ControllerConfig c;
    c.enabled = envInt("GECKO_ENABLED", 0) != 0;
    c.tolerance_ms = envDbl("GECKO_TOLERANCE_MS", 200.0);
    c.echo_window_us = envInt("GECKO_ECHO_WINDOW_MS", 5) * 1000;
    c.kf_on_flush = envInt("GECKO_KF_ON_FLUSH", 1) != 0;
    c.min_kf_interval_us = envInt("GECKO_MIN_KF_INTERVAL_MS", 500) * 1000;
    c.force_every = static_cast<int>(envInt("GECKO_FORCE_EVERY", 5));
    c.decay = envDbl("GECKO_DECAY", 0.5);
    c.pending_kf_max_us = envInt("GECKO_PENDING_KF_MAX_MS", 1000) * 1000;
    return c;
  }
};

class GeckoController {
 public:
  static GeckoController& Get() {
    static GeckoController* const c = new GeckoController();
    return *c;
  }

  bool Enabled() const { return cfg_.enabled; }
  const ControllerConfig& config() const { return cfg_; }

  // --- S3. An RTCP datagram arrived at the transport (raw bytes, possibly
  // SRTCP). Returns true when the datagram must be DROPPED: it is a copy of
  // one already received. The k-th copy inside the echo window (k = 2) is
  // the router's alert. Legitimate RTCP is never byte-identical (timestamps,
  // counters and the SRTCP index all move), so a duplicate is unambiguous.
  bool OnRtcpDatagram(const uint8_t* data, size_t len, int64_t now_us) {
    if (!cfg_.enabled) return false;
    const uint64_t h = Fnv1a(data, len);
    std::lock_guard<std::mutex> lk(mu_);
    ++rtcp_in_;
    while (!seen_.empty() && now_us - seen_.front().t_last_us > kHistoryUs)
      seen_.pop_front();
    for (Seen& s : seen_) {
      if (s.hash != h || s.len != len) continue;
      ++s.copies;
      ++dups_dropped_;
      const bool in_window = now_us - s.t_last_us <= cfg_.echo_window_us;
      s.t_last_us = now_us;
      if (in_window && !s.alerted) {
        s.alerted = true;
        OnAlertLocked(now_us);
      }
      return true;
    }
    seen_.push_back(Seen{h, static_cast<uint32_t>(len), now_us, 0, false});
    if (seen_.size() > kHistoryMax) seen_.pop_front();
    return false;
  }

  // --- S4. TWCC feedback for one ORIGINAL video media packet. Frames are
  // grouped by RTP timestamp; a frame's delay is (last packet's receive time
  // - first packet's send time) - D_min, D_min being the 10 s minimum of the
  // per-packet OWD (the same construction Pudica uses), so clock offset
  // cancels. Above the tolerance = one bad frame for the decider.
  void OnPacketFeedback(int64_t send_us, int64_t recv_us, uint32_t rtp_ts, bool /*group_last*/) {
    if (!cfg_.enabled) return;
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t owd = recv_us - send_us;
    // monotonic min-queue over the last 10 s of per-packet OWD
    while (!dmin_.empty() && dmin_.back().second >= owd) dmin_.pop_back();
    dmin_.push_back({recv_us, owd});
    while (!dmin_.empty() && recv_us - dmin_.front().first > kDminWindowUs) dmin_.pop_front();
    if (!have_cur_ || rtp_ts != cur_ts_) {
      if (have_cur_) CloseFrameLocked();
      have_cur_ = true;
      cur_ts_ = rtp_ts;
      cur_first_send_us_ = send_us;
      cur_last_recv_us_ = recv_us;
      cur_pkts_ = 1;
    } else {
      cur_last_recv_us_ = std::max(cur_last_recv_us_, recv_us);
      ++cur_pkts_;
    }
  }

  // --- S5. Called ONCE per encoded frame by RTPSenderVideo::SendVideo before
  // the packets go to the pacer. Returns the flag to stamp on every packet of
  // this frame (kFlagNone = nothing). A pending FLUSH rides on the next
  // KEYFRAME (the frames before it are doomed and counted as such); a pending
  // NO_FLUSH rides on the next frame of any kind.
  uint8_t FlagForFrame(bool is_keyframe, uint32_t rtp_ts, int64_t now_us) {
    if (!cfg_.enabled) return kFlagNone;
    std::lock_guard<std::mutex> lk(mu_);
    ++frames_sent_;
    if (is_keyframe) last_kf_sent_us_ = now_us;
    switch (pending_) {
      case Pending::kNone:
        return kFlagNone;
      case Pending::kNoFlushNext:
        pending_ = Pending::kNone;
        ++flags_noflush_;
        LogEvent(now_us, "flag", "noflush", now_us - pending_since_us_, rtp_ts);
        return kFlagNoFlush;
      case Pending::kFlushNext:
        pending_ = Pending::kNone;
        ++flags_flush_;
        LogEvent(now_us, "flag", "flush", now_us - pending_since_us_, rtp_ts);
        return kFlagFlush;
      case Pending::kFlushOnKeyframe:
        if (is_keyframe) {
          pending_ = Pending::kNone;
          ++flags_flush_;
          LogEvent(now_us, "flag", "flush_kf", now_us - pending_since_us_, rtp_ts);
          return kFlagFlush;
        }
        // a delta frame sent while the keyframe is pending: doomed by the flush
        decider_.OnFrameSkipped();
        ++doomed_;
        if (now_us - pending_since_us_ > cfg_.pending_kf_max_us) {
          pending_ = Pending::kNone;
          ++stale_;
          LogEvent(now_us, "stale", "no_keyframe", now_us - pending_since_us_, rtp_ts);
        }
        return kFlagNone;
    }
    return kFlagNone;
  }

  // --- S7. The encoder registers how to force a keyframe (VideoStreamEncoder
  // ::SendKeyFrame posts to its own queue, so this may be called from any
  // thread). Cleared in the encoder's destructor.
  void SetKeyFrameRequester(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(kf_mu_);
    kf_fn_ = std::move(fn);
  }
  void ClearKeyFrameRequester() {
    std::lock_guard<std::mutex> lk(kf_mu_);
    kf_fn_ = nullptr;
  }

  // Counters for a final line / tests.
  uint64_t alerts() const { return alerts_; }
  uint64_t dupsDropped() const { return dups_dropped_; }

 private:
  static constexpr int64_t kHistoryUs = 2000000;   // duplicates of anything seen within 2 s are dropped
  static constexpr size_t kHistoryMax = 64;
  static constexpr int64_t kDminWindowUs = 10000000;

  struct Seen {
    uint64_t hash;
    uint32_t len;
    int64_t t_last_us;
    int copies;
    bool alerted;
  };
  enum class Pending : uint8_t { kNone, kNoFlushNext, kFlushNext, kFlushOnKeyframe };

  GeckoController()
      : cfg_(ControllerConfig::FromEnv()),
        decider_(DeciderConfig{ControllerConfig::FromEnv().force_every,
                               ControllerConfig::FromEnv().decay}) {
    if (!cfg_.enabled) return;
    const char* dir = std::getenv("UNIFIED_CSV_DIR");
    if (dir && *dir) {
      const std::string path = std::string(dir) + "/gecko_sender.csv";
      csv_ = std::fopen(path.c_str(), "w");
      if (csv_) {
        std::fprintf(csv_, "t_ms,event,detail,decision,forced,blocked,last_bad,avg_f,avg_nf,"
                           "consecutive,lat_ms,rtp_ts,frames_fb,bad_frames,doomed,dups\n");
        std::fflush(csv_);
      }
    }
    std::fprintf(stderr,
                 "[GECKO] ON tolerance_ms=%.0f echo_window_ms=%.1f kf_on_flush=%d "
                 "min_kf_interval_ms=%lld force_every=%d decay=%.2f csv=%s\n",
                 cfg_.tolerance_ms, cfg_.echo_window_us / 1000.0, cfg_.kf_on_flush ? 1 : 0,
                 static_cast<long long>(cfg_.min_kf_interval_us / 1000), cfg_.force_every,
                 cfg_.decay, csv_ ? "yes" : "no");
  }

  static uint64_t Fnv1a(const uint8_t* d, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
      h ^= d[i];
      h *= 1099511628211ull;
    }
    return h;
  }

  static int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  void CloseFrameLocked() {
    const int64_t dmin = dmin_.empty() ? 0 : dmin_.front().second;
    const int64_t delay_us = (cur_last_recv_us_ - cur_first_send_us_) - dmin;
    ++frames_fb_;
    if (delay_us > static_cast<int64_t>(cfg_.tolerance_ms * 1000.0)) {
      ++bad_frames_;
      decider_.OnBadFrame();
    }
  }

  void OnAlertLocked(int64_t now_us) {
    ++alerts_;
    const bool kf_blocked =
        cfg_.kf_on_flush &&
        (pending_ == Pending::kFlushOnKeyframe ||
         (last_kf_req_us_ > 0 && now_us - last_kf_req_us_ < cfg_.min_kf_interval_us));
    const AlertOutcome o = decider_.OnAlert(kf_blocked);
    last_outcome_ = o;
    pending_since_us_ = now_us;
    const char* detail = "noflush";
    if (o.decision == Decision::FLUSH) {
      if (cfg_.kf_on_flush) {
        pending_ = Pending::kFlushOnKeyframe;
        last_kf_req_us_ = now_us;
        ++kf_requests_;
        RequestKeyFrame();
        detail = "flush_kf";
      } else {
        pending_ = Pending::kFlushNext;
        detail = "flush";
      }
    } else {
      pending_ = Pending::kNoFlushNext;
    }
    std::fprintf(stderr,
                 "[GECKO] ALERT n=%llu decision=%s forced=%d blocked=%d last_bad=%d "
                 "avg_f=%.2f avg_nf=%.2f consecutive=%d frames_fb=%llu bad=%llu doomed=%llu\n",
                 static_cast<unsigned long long>(alerts_), detail, o.forced ? 1 : 0,
                 o.blocked ? 1 : 0, o.last_bad, o.avg_f, o.avg_nf, o.consecutive,
                 static_cast<unsigned long long>(frames_fb_),
                 static_cast<unsigned long long>(bad_frames_),
                 static_cast<unsigned long long>(doomed_));
    LogEvent(now_us, "alert", detail, 0, 0);
  }

  void RequestKeyFrame() {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lk(kf_mu_);
      fn = kf_fn_;
    }
    if (fn) fn();
    else std::fprintf(stderr, "[GECKO] keyframe requested but no encoder registered\n");
  }

  // mu_ held.
  void LogEvent(int64_t now_us, const char* event, const char* detail, int64_t lat_us,
                uint32_t rtp_ts) {
    if (std::strcmp(event, "flag") == 0 || std::strcmp(event, "stale") == 0) {
      std::fprintf(stderr, "[GECKO] %s %s lat_ms=%.1f rtp_ts=%u\n", event, detail,
                   lat_us / 1000.0, rtp_ts);
    }
    if (!csv_) return;
    const AlertOutcome& o = last_outcome_;
    std::fprintf(csv_, "%.3f,%s,%s,%d,%d,%d,%d,%.3f,%.3f,%d,%.3f,%u,%llu,%llu,%llu,%llu\n",
                 now_us / 1000.0, event, detail, o.decision == Decision::FLUSH ? 1 : 0,
                 o.forced ? 1 : 0, o.blocked ? 1 : 0, o.last_bad, o.avg_f, o.avg_nf,
                 o.consecutive, lat_us / 1000.0, rtp_ts,
                 static_cast<unsigned long long>(frames_fb_),
                 static_cast<unsigned long long>(bad_frames_),
                 static_cast<unsigned long long>(doomed_),
                 static_cast<unsigned long long>(dups_dropped_));
    std::fflush(csv_);
  }

  const ControllerConfig cfg_;
  std::mutex mu_;
  Decider decider_;
  std::deque<Seen> seen_;
  std::deque<std::pair<int64_t, int64_t>> dmin_;   // (recv_us, owd_us), min-queue
  bool have_cur_ = false;
  uint32_t cur_ts_ = 0;
  int64_t cur_first_send_us_ = 0, cur_last_recv_us_ = 0;
  int cur_pkts_ = 0;
  Pending pending_ = Pending::kNone;
  int64_t pending_since_us_ = 0;
  int64_t last_kf_req_us_ = 0, last_kf_sent_us_ = 0;
  AlertOutcome last_outcome_;
  uint64_t rtcp_in_ = 0, dups_dropped_ = 0, alerts_ = 0, kf_requests_ = 0;
  uint64_t flags_flush_ = 0, flags_noflush_ = 0, doomed_ = 0, stale_ = 0;
  uint64_t frames_sent_ = 0, frames_fb_ = 0, bad_frames_ = 0;
  std::mutex kf_mu_;
  std::function<void()> kf_fn_;
  std::FILE* csv_ = nullptr;
};

}  // namespace gecko
}  // namespace webrtc

#endif  // PC_COORDINATOR_GECKO_CONTROLLER_H_
