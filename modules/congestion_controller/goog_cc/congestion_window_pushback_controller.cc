/*
 *  Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/congestion_controller/goog_cc/congestion_window_pushback_controller.h"

#include <algorithm>
#include <cstdint>

#include <cstdio>
#include <cstdlib>

#include "api/field_trials_view.h"
#include "api/units/data_size.h"
#include "rtc_base/experiments/rate_control_settings.h"
#include "rtc_base/time_utils.h"

namespace webrtc {
namespace {
// [T11] Congestion-window pushback forensics. Off unless KFT_CWND_FORENSICS=1.
//
// This module is the only brake on the encoder that reacts to the QUEUE rather
// than to the rate, and it is invisible in getStats: the frames it kills are
// dropped BEFORE encoding (video_stream_encoder.cc:1567), so
// quality_limitation_reason stays "none". Measured on p5_f2_att100 the encoder
// produced 0 fps at 0 Mb/s for ~4 s with reason "none", which is the signature
// this line exists to confirm or refute.
bool KftCwndForensics() {
  static const bool on = [] {
    const char* e = getenv("KFT_CWND_FORENSICS");
    return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
  }();
  return on;
}
}  // namespace

CongestionWindowPushbackController::CongestionWindowPushbackController(
    const FieldTrialsView& key_value_config)
    : add_pacing_(key_value_config.IsEnabled(
          "WebRTC-AddPacingToCongestionWindowPushback")),
      min_pushback_target_bitrate_bps_(
          RateControlSettings(key_value_config)
              .CongestionWindowMinPushbackTargetBitrateBps()),
      current_data_window_(RateControlSettings(key_value_config)
                               .CongestionWindowInitialDataWindow()) {
  // [T11] One line per construction. If this never appears, the controller was
  // never built (UseCongestionWindowPushback() false) and pushback cannot be
  // the mechanism stopping the encoder — which is the whole question this
  // instrumentation exists to settle.
  if (KftCwndForensics()) {
    fprintf(stderr, "KFTF CWNDCTOR add_pacing=%d min_pushback_kbps=%u init_win=%lld\n",
            add_pacing_ ? 1 : 0, min_pushback_target_bitrate_bps_ / 1000,
            current_data_window_.has_value()
                ? static_cast<long long>(current_data_window_->bytes())
                : -1);
  }
}

void CongestionWindowPushbackController::UpdateOutstandingData(
    int64_t outstanding_bytes) {
  outstanding_bytes_ = outstanding_bytes;
}
void CongestionWindowPushbackController::UpdatePacingQueue(
    int64_t pacing_bytes) {
  pacing_bytes_ = pacing_bytes;
}

void CongestionWindowPushbackController::SetDataWindow(DataSize data_window) {
  current_data_window_ = data_window;
}

uint32_t CongestionWindowPushbackController::UpdateTargetBitrate(
    uint32_t bitrate_bps) {
  if (!current_data_window_ || current_data_window_->IsZero()) {
    // [T11] The early return. If this is where every call lands, pushback is
    // inert and cannot be what stops the encoder — SetDataWindow() is only
    // reached once GoogCcNetworkController::UpdateCongestionWindowSize() has
    // run, which needs feedback_max_rtts_ to be non-empty
    // (goog_cc_network_control.cc:245).
    if (KftCwndForensics()) {
      static int64_t n = 0;
      if ((n++ % 200) == 0)
        fprintf(stderr, "KFTF CWNDOFF t_ms=%lld n=%lld window_set=%d\n",
                static_cast<long long>(rtc::TimeMillis()),
                static_cast<long long>(n),
                current_data_window_.has_value() ? 1 : 0);
    }
    return bitrate_bps;
  }
  int64_t total_bytes = outstanding_bytes_;
  if (add_pacing_)
    total_bytes += pacing_bytes_;
  double fill_ratio =
      total_bytes / static_cast<double>(current_data_window_->bytes());
  if (fill_ratio > 1.5) {
    encoding_rate_ratio_ *= 0.9;
  } else if (fill_ratio > 1) {
    encoding_rate_ratio_ *= 0.95;
  } else if (fill_ratio < 0.1) {
    encoding_rate_ratio_ = 1.0;
  } else {
    encoding_rate_ratio_ *= 1.05;
    encoding_rate_ratio_ = std::min(encoding_rate_ratio_, 1.0);
  }
  uint32_t adjusted_target_bitrate_bps =
      static_cast<uint32_t>(bitrate_bps * encoding_rate_ratio_);

  if (KftCwndForensics()) {
    // in = the transport target before pushback, out = what the encoder is
    // actually told. ratio is the multiplicative state that decays 0.9 per
    // update above fill 1.5 and recovers only 1.05 below it — a 20x asymmetry,
    // which is why `out` can sit at the 30 kbps floor long after the queue has
    // drained.
    fprintf(stderr,
            "KFTF CWND t_ms=%lld outstanding=%lld window=%lld fill=%.3f "
            "ratio=%.4f in_kbps=%u out_kbps=%u\n",
            static_cast<long long>(rtc::TimeMillis()),
            static_cast<long long>(total_bytes),
            static_cast<long long>(current_data_window_->bytes()), fill_ratio,
            encoding_rate_ratio_, bitrate_bps / 1000,
            adjusted_target_bitrate_bps / 1000);
  }

  // Do not adjust below the minimum pushback bitrate but do obey if the
  // original estimate is below it.
  bitrate_bps = adjusted_target_bitrate_bps < min_pushback_target_bitrate_bps_
                    ? std::min(bitrate_bps, min_pushback_target_bitrate_bps_)
                    : adjusted_target_bitrate_bps;
  return bitrate_bps;
}

}  // namespace webrtc
