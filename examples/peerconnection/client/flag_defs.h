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
ABSL_FLAG(int, test_duration, 10, "Test duration in seconds.");
ABSL_FLAG(int,
          vp8_kf_max_dist,
          0,
          "VP8 keyframe interval (libvpx kf_max_dist). 0 keeps whatever "
          "WEBRTC_VP8_KF_MAX_DIST holds, or the library default.");
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
ABSL_FLAG(bool,
          verbose_log,
          false,
          "Enable INFO-level WebRTC internal logging to stderr. Release builds "
          "default to no logging (rtc_base/logging.cc: LS_NONE); this flips it "
          "on at runtime via LogMessage::LogToDebug without needing a debug "
          "(-O0) build.");

ABSL_FLAG(std::string,
          experiment_mode,
          "real",
          "Operation mode: 'real' for normal bidirectional WebRTC, "
          "'emulation' for network emulation (restricts ICE candidates to "
          "--network_interface).");
ABSL_FLAG(std::string,
          network_interface,
          "",
          "Network interface to restrict ICE candidate gathering to; "
          "required when --experiment_mode=emulation.");
ABSL_FLAG(std::string,
          server_scheme,
          "https",
          "Signalling URL scheme: 'https' (external server, no port in "
          "URL) or 'http' (local signalling_server.py; --port is appended "
          "to the URL). Unused unless --server/--port name the signalling "
          "host instead of --signaling_server.");

#endif  // EXAMPLES_PEERCONNECTION_CLIENT_FLAG_DEFS_H_
