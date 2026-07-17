/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <glib.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <fstream>
#include <sstream>
#include <string>

#include "absl/flags/parse.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "api/scoped_refptr.h"
#include "examples/peerconnection/client/conductor.h"
#include "examples/peerconnection/client/flag_defs.h"
#include "examples/peerconnection/client/linux/headless_wnd.h"
#include "examples/peerconnection/client/peer_connection_client.h"
#include "pc/rtp_sctp_coordinator.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/field_trial.h"
#include "test/field_trial.h"

class CustomSocketServer : public rtc::PhysicalSocketServer {
 public:
  explicit CustomSocketServer(HeadlessWnd* wnd)
      : wnd_(wnd), conductor_(NULL), client_(NULL) {}
  virtual ~CustomSocketServer() {}

  void SetMessageQueue(rtc::Thread* queue) override { message_queue_ = queue; }

  void set_client(PeerConnectionClient* client) { client_ = client; }
  void set_conductor(Conductor* conductor) { conductor_ = conductor; }

  bool Wait(webrtc::TimeDelta max_wait_duration, bool process_io) override {
    // Service WebSocket client
    if (conductor_) {
      conductor_->ServiceWebSocket();
    }

    // Check for disconnection
    if (!conductor_->connection_active() &&
        client_ != NULL && !client_->is_connected()) {
      RTC_LOG(LS_INFO) << "Connection ended, quitting message loop";
      message_queue_->Quit();
    }

    // Process socket events with a small timeout
    return rtc::PhysicalSocketServer::Wait(webrtc::TimeDelta::Millis(100), process_io);
  }

 protected:
  rtc::Thread* message_queue_;
  HeadlessWnd* wnd_;
  Conductor* conductor_;
  PeerConnectionClient* client_;
};


int main(int argc, char* argv[]) {
  RTC_LOG(LS_INFO) << "Initializing headless WebRTC client...";
  
  absl::ParseCommandLine(argc, argv);

  // Initialize logging
  rtc::LogMessage::LogToDebug(rtc::LS_INFO);
  rtc::LogMessage::LogTimestamps();
  rtc::LogMessage::LogThreads();

  // Initialize field trials
  const std::string forced_field_trials = absl::GetFlag(FLAGS_force_fieldtrials);
  webrtc::field_trial::InitFieldTrialsFromString(forced_field_trials.c_str());

  // Detect and set peer role (sender/receiver) for file filtering
  std::string role = absl::GetFlag(FLAGS_role);
  if (role.empty()) {
    // Auto-detect role: if queries_csv, sctp_csv or rtp_csv is specified, assume sender
    const std::string queries_csv = absl::GetFlag(FLAGS_queries_csv);
    const std::string sctp_csv = absl::GetFlag(FLAGS_sctp_csv);
    const std::string rtp_csv = absl::GetFlag(FLAGS_rtp_csv);
    if (!queries_csv.empty() || !sctp_csv.empty() || !rtp_csv.empty()) {
      role = "sender";
      RTC_LOG(LS_INFO) << "Auto-detected role: sender (has traffic profiles)";
    } else {
      role = "receiver";
      RTC_LOG(LS_INFO) << "Auto-detected role: receiver (no traffic profiles)";
    }
  } else {
    RTC_LOG(LS_INFO) << "Role explicitly set to: " << role;
  }
  // Ported to older baseline: RtpSctpCoordinator::SetGlobalRole() does not
  // exist on this tree's pc/rtp_sctp_coordinator.h (no global role concept),
  // so the detected role is only logged above. Disabled minimally.
  // webrtc::RtpSctpCoordinator::SetGlobalRole(role);
  (void)role;

  // Validate port
  if ((absl::GetFlag(FLAGS_port) < 1) || (absl::GetFlag(FLAGS_port) > 65535)) {
    RTC_LOG(LS_ERROR) << "Error: " << absl::GetFlag(FLAGS_port) << " is not a valid port.";
    return -1;
  }

  // Create headless window
  const std::string server = absl::GetFlag(FLAGS_server);
  RTC_LOG(LS_INFO) << "Connecting to server: " << server;
  
  HeadlessWnd wnd(server.c_str(), absl::GetFlag(FLAGS_port),
                  true,  // autoconnect
                  true); // autocall
  
  if (!wnd.Create()) {
    RTC_LOG(LS_ERROR) << "Failed to create headless window";
    return -1;
  }

  // Initialize socket server
  CustomSocketServer socket_server(&wnd);
  rtc::AutoSocketServerThread thread(&socket_server);

  // Initialize SSL
  rtc::InitializeSSL();
  RTC_LOG(LS_INFO) << "SSL initialized";

  // Create peer connection client and conductor.
  // Ported: the transplanted Conductor takes (env, client, wnd) instead of
  // the old (client, wnd, headless) signature.
  webrtc::Environment env = webrtc::CreateEnvironment();
  PeerConnectionClient client;
  auto conductor = rtc::make_ref_counted<Conductor>(env, &client, &wnd);
  socket_server.set_client(&client);
  socket_server.set_conductor(conductor.get());

  // Apply max bitrate from --max_bitrate_kbps flag or rtp.csv
  int max_bitrate_kbps = absl::GetFlag(FLAGS_max_bitrate_kbps);
  const std::string rtp_csv_path = absl::GetFlag(FLAGS_rtp_csv);
  if (!rtp_csv_path.empty()) {
    std::ifstream rtp_file(rtp_csv_path);
    if (rtp_file.is_open()) {
      std::string header_line, data_line;
      std::getline(rtp_file, header_line);  // skip header
      if (std::getline(rtp_file, data_line) && !data_line.empty()) {
        // CSV col 6 (0-indexed) = "Max bitrate (If RTP)" in bps
        std::istringstream ss(data_line);
        std::string field;
        for (int col = 0; col <= 6 && std::getline(ss, field, ','); ++col) {
          if (col == 6 && !field.empty()) {
            int max_bps = std::stoi(field);
            max_bitrate_kbps = max_bps / 1000;
            fprintf(stderr, "[RTP] Max bitrate from rtp.csv: %d kbps\n",
                    max_bitrate_kbps);
          }
        }
      }
    }
  }
  conductor->SetMaxBitrateKbps(max_bitrate_kbps);
  fprintf(stderr, "[RTP] Applied max bitrate: %d kbps\n", max_bitrate_kbps);

  RTC_LOG(LS_INFO) << "Starting message loop...";
  
  // Run the message loop
  thread.Run();

  // Cleanup
  RTC_LOG(LS_INFO) << "Cleaning up...";
  wnd.Destroy();
  rtc::CleanupSSL();
  
  RTC_LOG(LS_INFO) << "Client terminated normally";
  return 0;
}
