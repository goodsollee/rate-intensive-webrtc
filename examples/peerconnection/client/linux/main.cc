/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 */

#include <gtk/gtk.h>

#include <csignal>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <poll.h>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "api/make_ref_counted.h"
#include "api/scoped_refptr.h"
#include "api/units/time_delta.h"
#include "examples/peerconnection/client/conductor.h"
#include "examples/peerconnection/client/flag_defs.h"
#include "examples/peerconnection/client/linux/main_wnd.h"
#include "examples/peerconnection/client/peer_connection_client.h"
#include "rtc_base/logging.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

class CustomSocketServer : public rtc::PhysicalSocketServer {
 public:
  explicit CustomSocketServer(GtkMainWnd* wnd, bool headless, bool demo_mode)
      : wnd_(wnd), conductor_(nullptr), client_(nullptr),
        headless_(headless), demo_mode_(demo_mode) {}
  ~CustomSocketServer() override {}

  void SetMessageQueue(rtc::Thread* queue) override {
    message_queue_ = queue;
  }

  void set_client(PeerConnectionClient* client) { client_ = client; }
  void set_conductor(Conductor* conductor) { conductor_ = conductor; }

  bool Wait(webrtc::TimeDelta max_wait_duration, bool process_io) override {
    // Only pump GTK events if NOT in headless mode
    if (!headless_) {
      while (gtk_events_pending())
        gtk_main_iteration();
    }

    // Headless demo mode: poll stdin for queries
    if (headless_ && demo_mode_ && conductor_) {
      struct pollfd pfd = {0, POLLIN, 0};
      if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        char buf[4096];
        if (fgets(buf, sizeof(buf), stdin)) {
          std::string query(buf);
          // Trim trailing newline
          while (!query.empty() && (query.back() == '\n' || query.back() == '\r'))
            query.pop_back();
          if (!query.empty()) {
            printf("[DEMO] stdin query: %s\n", query.c_str());
            fflush(stdout);
            wnd_->AppendChatMessage("You", query);
            // Call through MainWndCallback interface (public)
            static_cast<MainWndCallback*>(conductor_)->OnQuerySubmitted(query);
          }
        }
      }
    }

    // Service WebSocket if conductor has one
    if (conductor_) {
      conductor_->ServiceWebSocket();
    }

    if (!wnd_->IsWindow() && !conductor_->connection_active() &&
        client_ != nullptr && !client_->is_connected()) {
      message_queue_->Quit();
    }

    webrtc::TimeDelta wait_time = headless_ ? webrtc::TimeDelta::Millis(10)
                                            : webrtc::TimeDelta::Zero();
    return rtc::PhysicalSocketServer::Wait(wait_time, process_io);
  }

 protected:
  rtc::Thread* message_queue_;
  GtkMainWnd* wnd_;
  Conductor* conductor_;
  PeerConnectionClient* client_;
  bool headless_;
  bool demo_mode_;
};

// Global conductor for signal handler (writes partial flow_completion.csv on SIGTERM)
static Conductor* g_conductor_for_signal = nullptr;

static void SignalHandler(int sig) {
  if (g_conductor_for_signal) {
    g_conductor_for_signal->WriteFlowCompletionCsv();
  }
  _exit(0);
}

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  // This main() never installed a log sink, so every RTC_LOG in the tree has
  // been silently discarded -- including the MAE on/off markers. Opt in to the
  // ERROR level only: the KFT markers are logged at LS_ERROR, and stock WebRTC
  // emits almost nothing there, so the run is not perturbed by log volume.
  if (const char* e = getenv("KFT_WEBRTC_LOG")) {
    if (e[0] != '\0' && !(e[0] == '0' && e[1] == '\0')) {
      // "info"/"verbose" opens the level up for diagnosis only. Every measured
      // run uses KFT_WEBRTC_LOG=1, which stays ERROR-only so the log volume
      // cannot perturb it.
      const std::string level(e);
      rtc::LogMessage::LogToDebug(
          (level == "info" || level == "verbose") ? rtc::LS_INFO
                                                 : rtc::LS_ERROR);
      rtc::LogMessage::LogTimestamps();
    }
  }

  bool headless = absl::GetFlag(FLAGS_headless);
  bool demo_mode = absl::GetFlag(FLAGS_demo_mode);
  std::string room_id = absl::GetFlag(FLAGS_room_id);
  bool use_websocket = !room_id.empty();
  
  // Initialize GTK only if NOT in headless mode
  if (!headless) {
    gtk_init(&argc, &argv);
  } else {
    printf("[Headless] Running without GTK UI\n");
  }

  if (use_websocket) {
    printf("[WebSocket] Using room_id: %s\n", room_id.c_str());
  }

  webrtc::Environment env =
      webrtc::CreateEnvironment(std::make_unique<webrtc::FieldTrials>(
          absl::GetFlag(FLAGS_force_fieldtrials)));

  // Abort if the user specifies a port that is outside the allowed range
  if ((absl::GetFlag(FLAGS_port) < 1) || (absl::GetFlag(FLAGS_port) > 65535)) {
    printf("Error: %i is not a valid port.\n", absl::GetFlag(FLAGS_port));
    return -1;
  }

  const std::string server = absl::GetFlag(FLAGS_server);
  GtkMainWnd wnd(server.c_str(), absl::GetFlag(FLAGS_port),
                 absl::GetFlag(FLAGS_autoconnect),
                 absl::GetFlag(FLAGS_autocall),
                 headless, demo_mode);

  CustomSocketServer socket_server(&wnd, headless, demo_mode);
  rtc::AutoSocketServerThread thread(&socket_server);

  rtc::InitializeSSL();
  
  PeerConnectionClient client;
  auto conductor = webrtc::make_ref_counted<Conductor>(env, &client, &wnd);
  socket_server.set_client(&client);
  socket_server.set_conductor(conductor.get());

  // Register signal handler to write partial flow_completion.csv on SIGTERM/SIGINT
  g_conductor_for_signal = conductor.get();
  std::signal(SIGTERM, SignalHandler);
  std::signal(SIGINT, SignalHandler);

  // Configure demo mode
  if (demo_mode) {
    conductor->SetDemoMode(true);
    conductor->SetModelPath(absl::GetFlag(FLAGS_model_path));
    conductor->SetContextPath(absl::GetFlag(FLAGS_context_path));
    conductor->SetKVCachePath(absl::GetFlag(FLAGS_kvcache_path));
    conductor->SetContextMethod(absl::GetFlag(FLAGS_context_method));
    std::string context_dir = absl::GetFlag(FLAGS_context_dir);
    if (!context_dir.empty()) {
      conductor->SetContextDir(context_dir);
    }
  }

  // Configure conductor
  if (use_websocket) {
    conductor->SetRoomId(room_id);
    conductor->SetServer(absl::GetFlag(FLAGS_signaling_server));
    conductor->SetIsSender(absl::GetFlag(FLAGS_is_sender));
    conductor->SetY4mPath(absl::GetFlag(FLAGS_y4m_path));
    // Apply max bitrate: rtp.csv overrides --max_bitrate_kbps flag
    int max_bitrate_kbps = absl::GetFlag(FLAGS_max_bitrate_kbps);
    const std::string rtp_csv_path = absl::GetFlag(FLAGS_rtp_csv);
    if (!rtp_csv_path.empty()) {
      std::ifstream rtp_file(rtp_csv_path);
      if (rtp_file.is_open()) {
        std::string header_line, data_line;
        std::getline(rtp_file, header_line);
        if (std::getline(rtp_file, data_line) && !data_line.empty()) {
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
    conductor->SetVideoFps(absl::GetFlag(FLAGS_video_fps));
    conductor->SetLogDirectory(absl::GetFlag(FLAGS_log_root));

    // MAFS multi-flow: pass traffic config to conductor
    std::string queries_csv = absl::GetFlag(FLAGS_queries_csv);
    if (!queries_csv.empty()) {
      conductor->SetTrafficConfig(queries_csv);
      printf("[MAFS] Traffic config: %s\n", queries_csv.c_str());
    }

    // Start WebSocket signaling
    printf("[WebSocket] Starting signaling to %s...\n",
           absl::GetFlag(FLAGS_signaling_server).c_str());
    conductor->StartWebSocketSignaling();
  }

  // Create window AFTER conductor registers as observer
  wnd.Create();

  // In demo mode, switch directly to streaming UI (chat panel)
  if (demo_mode && !headless) {
    wnd.SwitchToStreamingUI();
  }

  thread.Run();

  wnd.Destroy();
  rtc::CleanupSSL();
  return 0;
}
