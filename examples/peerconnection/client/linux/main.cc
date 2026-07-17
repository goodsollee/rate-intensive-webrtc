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
#include <filesystem>
#include <system_error>

#include "absl/flags/parse.h"
#include "absl/flags/usage.h"
#include "api/scoped_refptr.h"
#include "examples/peerconnection/client/conductor.h"
#include "examples/peerconnection/client/flag_defs.h"
#include "examples/peerconnection/client/linux/main_wnd.h"
#include "examples/peerconnection/client/peer_connection_client.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/field_trial.h"
#include "test/field_trial.h"

#include "absl/flags/flag.h"

// Define flags without the help flag (it's built into absl)
ABSL_FLAG(std::string, experiment_mode, "real", 
    "Experiment mode: 'real' for real-world or 'emulation' for network emulation");
ABSL_FLAG(bool, is_sender, true, 
    "Whether this peer is sender (true) or receiver (false)");
ABSL_FLAG(std::string, network_interface, "",
    "Network interface to use (empty for default)");
ABSL_FLAG(std::string, y4m_path, "",
    "Path to Y4M file to use as video source (empty for test pattern)");
ABSL_FLAG(std::string, log_date, "", "Date for log folder (YYYY-MM-DD)");
ABSL_FLAG(std::string,
          log_root,
          "webrtc_logs",
          "Root directory used to store experiment logs");
ABSL_FLAG(bool, record_remote, false,
    "Record remote video track to an MP4 file");
ABSL_FLAG(std::string, record_path, "remote.mp4",
    "Destination path for remote MP4 recording");

ABSL_FLAG(bool, headless, false,
    "Whether to run in headless or not");

ABSL_FLAG(std::string, server_scheme, "https",
    "Signalling URL scheme: 'https' (external server, no port in URL) or "
    "'http' (local signalling_server.py; --port is appended to the URL)");

class CustomSocketServer : public rtc::PhysicalSocketServer {
 public:
  explicit CustomSocketServer(GtkMainWnd* wnd)
      : wnd_(wnd), conductor_(NULL), client_(NULL) {}
  virtual ~CustomSocketServer() {}

  void SetMessageQueue(rtc::Thread* queue) override { message_queue_ = queue; }

  void set_client(PeerConnectionClient* client) { client_ = client; }
  void set_conductor(Conductor* conductor) { conductor_ = conductor; }

  bool Wait(webrtc::TimeDelta max_wait_duration, bool process_io) override {
    while (gtk_events_pending())
      gtk_main_iteration();

    if (conductor_) {
      conductor_->ServiceWebSocket();
    }

    if (!wnd_->IsWindow() && !conductor_->connection_active() &&
        client_ != NULL && !client_->is_connected()) {
      message_queue_->Quit();
    }
    return rtc::PhysicalSocketServer::Wait(webrtc::TimeDelta::Zero(),
                                           process_io);
  }

 protected:
  rtc::Thread* message_queue_;
  GtkMainWnd* wnd_;
  Conductor* conductor_;
  PeerConnectionClient* client_;
};


int main(int argc, char* argv[]) {
  // Set the program usage message
  std::string usage_str = R"(WebRTC Peer Connection Client

Basic Options:
  --help                      Display this help message (built-in Abseil flag)
  --server=<hostname>         Signaling server hostname (default: localhost)
  --port=<port>              Server port (default: 8888)
  --room_id=<id>             Room ID for the session

Experiment Mode Options:
  --experiment_mode=<mode>    Operation mode (default: real)
                             - 'real': Normal bidirectional WebRTC
                             - 'emulation': Network emulation mode

  --is_sender=<bool>         Role in emulation mode (default: true)
                             - true: Send video only
                             - false: Receive video only

  --network_interface=<name>  Network interface to use (required in emulation mode)
                             Example: eth0, wlan0

Video Source Options:
  --y4m_path=<path>         Path to Y4M file to use as video source
                            If not specified, uses test pattern

Recording Options:
  --record_remote=<bool>    Enable remote video recording (default: false)
  --record_path=<path>      MP4 output path for remote recording (default: remote.mp4)

SCTP Traffic Options:
  --sctp_csv=<path>         Path to SCTP traffic CSV to drive traffic

  --traffic_csv=<path>      Legacy alias for --sctp_csv

RTP Traffic Options:
  --rtp_csv=<path>          Path to RTP traffic CSV to configure media

Example Commands:
  # Run as video sender using Y4M file:
  ./peerconnection_client --experiment_mode=emulation --is_sender=true \
    --network_interface=eth0 --y4m_path=/path/to/video.y4m \
    --sctp_csv=/path/to/traffic.csv \
    --server=localhost --port=8888

  # Run as video receiver:
  ./peerconnection_client --experiment_mode=emulation --is_sender=false \
      --sctp_csv=/path/to/traffic.csv \
      --network_interface=eth0 --server=localhost --port=8888 
)";

  // Set the usage message
  absl::SetProgramUsageMessage(usage_str);

  absl::ParseCommandLine(argc, argv);

  gtk_init(&argc, &argv);
  // juheon added: skip in headless mode
  if(absl::GetFlag(FLAGS_headless)){
    printf("headless mode, skip gtk init!\n");
  }else{
    printf("init gtk!\n");
    gtk_init(&argc, &argv);
  // g_type_init API is deprecated (and does nothing) since glib 2.35.0, see:
  // https://mail.gnome.org/archives/commits-list/2012-November/msg07809.html
  #if !GLIB_CHECK_VERSION(2, 35, 0)
    g_type_init();
  #endif
  // g_thread_init API is deprecated since glib 2.31.0, see release note:
  // http://mail.gnome.org/archives/gnome-announce-list/2011-October/msg00041.html
  #if !GLIB_CHECK_VERSION(2, 31, 0)
    g_thread_init(NULL);
  #endif
  }

  // Parse command line flags
  std::vector<char*> remaining_args = absl::ParseCommandLine(argc, argv);

  const std::string forced_field_trials = absl::GetFlag(FLAGS_force_fieldtrials);
  webrtc::field_trial::InitFieldTrialsFromString(forced_field_trials.c_str());

  // Validate port number
  if ((absl::GetFlag(FLAGS_port) < 1) || (absl::GetFlag(FLAGS_port) > 65535)) {
    printf("Error: %i is not a valid port.\n", absl::GetFlag(FLAGS_port));
    printf("Use --help for usage information.\n");
    return -1;
  }

  // Validate emulation mode settings
  std::string experiment_mode = absl::GetFlag(FLAGS_experiment_mode);
  bool is_emulation = (experiment_mode == "emulation");
  
  if (is_emulation && absl::GetFlag(FLAGS_network_interface).empty()) {
    printf("Error: Network interface (--network_interface) is required in emulation mode.\n");
    printf("Use --help for usage information.\n");
    return -1;
  }

  const std::string server = absl::GetFlag(FLAGS_server);
  GtkMainWnd wnd(server.c_str(), absl::GetFlag(FLAGS_port),
                 absl::GetFlag(FLAGS_autoconnect),
                 absl::GetFlag(FLAGS_autocall),
                 absl::GetFlag(FLAGS_headless));

  wnd.Create();

  CustomSocketServer socket_server(&wnd);
  rtc::AutoSocketServerThread thread(&socket_server);

  rtc::InitializeSSL();
  PeerConnectionClient client;
  auto conductor = rtc::make_ref_counted<Conductor>(&client, &wnd, absl::GetFlag(FLAGS_headless));
  conductor->SetSignaling(absl::GetFlag(FLAGS_server_scheme), absl::GetFlag(FLAGS_port));

  if (absl::GetFlag(FLAGS_record_remote)) {
    std::string record_path = absl::GetFlag(FLAGS_record_path);
    bool enable_recording = true;
    if (record_path.empty()) {
      printf("Remote recording enabled but no output path provided. Disabling.\n");
      enable_recording = false;
    } else {
      std::filesystem::path output_path(record_path);
      std::error_code ec;
      if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path(), ec);
      }
      if (ec) {
        printf("Failed to create directories for %s: %s. Recording disabled.\n",
               record_path.c_str(), ec.message().c_str());
        enable_recording = false;
      } else {
        conductor->SetRecordingPath(record_path);
      }
    }
    conductor->EnableRecording(enable_recording);
  }

  std::string sctp_csv = absl::GetFlag(FLAGS_sctp_csv);
  if (sctp_csv.empty()) {
    sctp_csv = absl::GetFlag(FLAGS_traffic_csv);
  }
  if (!sctp_csv.empty()) {
    conductor->SetSctpTrafficProfile(sctp_csv);
  }

  std::string rtp_csv = absl::GetFlag(FLAGS_rtp_csv);
  if (!rtp_csv.empty()) {
    conductor->SetRtpTrafficProfile(rtp_csv);
  }

  conductor->SetRoomId(absl::GetFlag(FLAGS_room_id));

  // Get log date - if empty, use current date
  std::string date = absl::GetFlag(FLAGS_log_date);
  if (date.empty()) {
      std::time_t now = std::time(nullptr);
      char date_buf[20];  // Increased buffer size for full timestamp
      std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
      date = date_buf;
  }
    
  // Create log directory path
  std::string room_id = absl::GetFlag(FLAGS_room_id);
  bool is_sender = absl::GetFlag(FLAGS_is_sender);
  std::string role = is_sender ? "sender" : "receiver";
  std::filesystem::path log_root = absl::GetFlag(FLAGS_log_root);
  if (log_root.empty()) {
    log_root = std::filesystem::path("webrtc_logs");
  }

  std::filesystem::path log_dir =
      log_root / std::filesystem::path(date) / room_id / role;

  // Create directory
  std::filesystem::create_directories(log_dir);

  // Pass log_dir to conductor
  conductor->SetLogDirectory(log_dir.string());

  // Configure experiment mode
  conductor->SetEmulationMode(is_emulation, is_sender);
  conductor->SetY4mPath(absl::GetFlag(FLAGS_y4m_path));

  if (is_emulation) {
    conductor->SetNetInterface(absl::GetFlag(FLAGS_network_interface));
  }

  socket_server.set_client(&client);
  socket_server.set_conductor(conductor.get());

  conductor->Start();

  thread.Run();
  wnd.Destroy();

  rtc::CleanupSSL();
  return 0;
}