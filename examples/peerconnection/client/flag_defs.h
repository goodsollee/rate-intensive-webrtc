/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef EXAMPLES_PEERCONNECTION_CLIENT_FLAG_DEFS_H_
#define EXAMPLES_PEERCONNECTION_CLIENT_FLAG_DEFS_H_

#include <string>

#include "absl/flags/flag.h"

extern const uint16_t kDefaultServerPort;  // From defaults.[h|cc]

// Define flags for the peerconnect_client testing tool, in a separate
// header file so that they can be shared across the different main.cc's
// for each platform.

ABSL_FLAG(bool,
          autoconnect,
          false,
          "Connect to the server without user "
          "intervention.");
ABSL_FLAG(std::string, server, "localhost", "The server to connect to.");
ABSL_FLAG(int,
          port,
          kDefaultServerPort,
          "The port on which the server is listening.");
ABSL_FLAG(
    bool,
    autocall,
    false,
    "Call the first available other client on "
    "the server without user intervention.  Note: this flag should only be set "
    "to true on one of the two clients.");

ABSL_FLAG(std::string,
          room_id,
          "",  // default value
          "The room ID to join on the server.");

ABSL_FLAG(
    std::string,
    force_fieldtrials,
    "",
    "Field trials control experimental features. This flag specifies the field "
    "trials in effect. E.g. running with "
    "--force_fieldtrials=WebRTC-FooFeature/Enabled/ "
    "will assign the group Enabled to field trial WebRTC-FooFeature. Multiple "
    "trials are separated by \"/\"");

ABSL_FLAG(std::string,
          traffic_csv,
          "",
          "CSV file describing traffic profiles");

ABSL_FLAG(std::string,
          sctp_csv,
          "",
          "CSV file describing SCTP traffic profiles");

ABSL_FLAG(std::string,
          rtp_csv,
          "",
          "CSV file describing RTP traffic profile");

// --- Research testbed flags (transplanted client) -------------------------
// Definitions for flags the transplanted conductor.cc / main.cc /
// main_headless.cc reference via ABSL_DECLARE_FLAG or directly.

ABSL_FLAG(bool, headless, false, "Run without a GTK UI.");
ABSL_FLAG(bool, demo_mode, false, "Enable LLM demo chat UI/flow.");
ABSL_FLAG(std::string,
          role,
          "",
          "Peer role: 'sender' or 'receiver'. Empty = auto-detect.");
ABSL_FLAG(std::string,
          signaling_server,
          "goodsol.overlinkapp.org",
          "WebSocket signaling server host.");
ABSL_FLAG(bool, is_sender, false, "This peer sends media/data.");
ABSL_FLAG(std::string, y4m_path, "", "Y4M file used as the video source.");
ABSL_FLAG(int, max_bitrate_kbps, 20000, "Max video bitrate in kbps.");
ABSL_FLAG(int, video_fps, 30, "Video frame rate.");
ABSL_FLAG(std::string, log_root, "", "Directory for CSV logs.");
ABSL_FLAG(std::string,
          queries_csv,
          "",
          "CSV describing MAFS multi-flow queries.");
ABSL_FLAG(bool, datachannel_test, false, "DataChannel throughput test mode.");
ABSL_FLAG(bool, rtp_sctp_mode, false, "Combined RTP+SCTP test mode.");
ABSL_FLAG(bool, rtp_only_mode, false, "RTP-only test mode (no SCTP).");
// The research modes skip audio to avoid depending on a real capture device.
// This adds it back deliberately, on a synthetic device, for experiments that
// need a second media stream on the wire -- e.g. proving a RAN-side video hold
// leaves audio flowing. Off by default so every existing run is unchanged.
//
// The audio track is published under its OWN stream id, which keeps A/V
// synchronization OFF: WebRTC pairs streams for lip-sync by matching sync_group,
// and sync_group comes from the stream id (webrtc_video_engine.cc /
// webrtc_voice_engine.cc). Sharing one id would silently turn sync on and couple
// the video playout clock to audio -- exactly what a video-hold experiment must
// not have.
ABSL_FLAG(bool, with_audio, false,
          "Add an audio track from a synthetic device, on its own stream id "
          "(no A/V sync). Research modes omit audio unless this is set.");
ABSL_FLAG(int, test_duration, 10, "Test duration in seconds.");
ABSL_FLAG(int,
          vp8_kf_max_dist,
          3000,
          "VP8 keyframe interval (libvpx kf_max_dist).");
ABSL_FLAG(std::string, model_path, "", "LLM model path (demo mode).");
ABSL_FLAG(std::string, context_path, "", "Raw-text context file (demo mode).");
ABSL_FLAG(std::string, kvcache_path, "", "KV-cache file (demo mode).");
ABSL_FLAG(std::string,
          context_method,
          "raw_text",
          "Context transfer method: raw_text, kv_cache, or kvzip.");
ABSL_FLAG(std::string,
          context_dir,
          "",
          "Directory with context documents 00.txt/01.txt/... (demo mode).");

#endif  // EXAMPLES_PEERCONNECTION_CLIENT_FLAG_DEFS_H_
