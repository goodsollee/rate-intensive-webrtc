/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "examples/peerconnection/client/conductor.h"
#include "rtc_base/time_utils.h"
#include <curl/curl.h>
#include <sstream>
#include <ctime>
#include <cstdlib>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sys/stat.h>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <zstd.h>

#include "absl/base/nullability.h"
#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/memory/memory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_options.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/environment/environment.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_sender_interface.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/task_queue_factory.h"
#include "api/test/create_frame_generator.h"
#include "api/video/video_frame.h"
#include "api/video/video_source_interface.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_dav1d_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "examples/peerconnection/client/defaults.h"
#include "examples/peerconnection/client/main_wnd.h"
#include "examples/peerconnection/client/peer_connection_client.h"
#include "json/reader.h"
#include "json/value.h"
#include "json/writer.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/strings/json.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/clock.h"
#include "test/frame_generator_capturer.h"
#include "test/testsupport/y4m_frame_generator.h"
#include "test/platform_video_capturer.h"
#include "test/test_video_capturer.h"

ABSL_DECLARE_FLAG(bool, datachannel_test);
ABSL_DECLARE_FLAG(bool, rtp_sctp_mode);
ABSL_DECLARE_FLAG(bool, rtp_only_mode);
ABSL_DECLARE_FLAG(int, test_duration);
ABSL_DECLARE_FLAG(int, vp8_kf_max_dist);
ABSL_DECLARE_FLAG(bool, demo_mode);
ABSL_DECLARE_FLAG(std::string, context_path);
ABSL_DECLARE_FLAG(std::string, kvcache_path);
ABSL_DECLARE_FLAG(std::string, context_method);
ABSL_FLAG(bool,
          strip_audio_m_section,
          false,
          "Remove the audio m= section from SDP before applying it.");

// Shared measured BW for RACE mode: updated by sender after each flow completes,
// used by pre-send race simulation as fallback when RACE_EST_BW_MBPS not set.
static std::atomic<double> g_race_measured_bw_mbps{0.0};

// Returns true when running in HAFS (MAFS) coordinator mode.
// Overlapped prefill is always active for HAFS and never active for NC/FSE.
static bool IsHafsMode() {
  const char* e = getenv("MAFS_ENABLE");
  if (e && std::string(e) == "1") return true;
  e = getenv("HAFS_ENABLE");
  return e && std::string(e) == "1";
}

namespace {
using webrtc::test::TestVideoCapturer;

// Names used for a IceCandidate JSON object.
const char kCandidateSdpMidName[] = "sdpMid";
const char kCandidateSdpMlineIndexName[] = "sdpMLineIndex";
const char kCandidateSdpName[] = "candidate";

// Names used for a SessionDescription JSON object.
const char kSessionDescriptionTypeName[] = "type";
const char kSessionDescriptionSdpName[] = "sdp";

class DummySetSessionDescriptionObserver
    : public webrtc::SetSessionDescriptionObserver {
 public:
  static webrtc::scoped_refptr<DummySetSessionDescriptionObserver> Create() {
    return webrtc::make_ref_counted<DummySetSessionDescriptionObserver>();
  }
  void OnSuccess() override { RTC_LOG(LS_INFO) << __FUNCTION__; }
  void OnFailure(webrtc::RTCError error) override {
    RTC_LOG(LS_INFO) << __FUNCTION__ << " " << ToString(error.type()) << ": "
                     << error.message();
  }
};

std::unique_ptr<TestVideoCapturer> CreateCapturer(
    webrtc::TaskQueueFactory& task_queue_factory) {
  const size_t kWidth = 640;
  const size_t kHeight = 480;
  const size_t kFps = 30;
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (info) {
    int num_devices = info->NumberOfDevices();
    for (int i = 0; i < num_devices; ++i) {
      std::unique_ptr<TestVideoCapturer> capturer =
          webrtc::test::CreateVideoCapturer(kWidth, kHeight, kFps, i);
      if (capturer) {
        return capturer;
      }
    }
  }
  RTC_LOG(LS_WARNING)
      << "No video capture device found; using synthetic video.";
  auto frame_generator = webrtc::test::CreateSquareFrameGenerator(
      kWidth, kHeight, std::nullopt, std::nullopt);
  return std::make_unique<webrtc::test::FrameGeneratorCapturer>(
      webrtc::Clock::GetRealTimeClock(), std::move(frame_generator), kFps,
      task_queue_factory);
}
class CapturerTrackSource : public webrtc::VideoTrackSource {
 public:
  static webrtc::scoped_refptr<CapturerTrackSource> Create(
      webrtc::TaskQueueFactory& task_queue_factory) {
    std::unique_ptr<TestVideoCapturer> capturer =
        CreateCapturer(task_queue_factory);
    if (capturer) {
      capturer->Start();
      return webrtc::make_ref_counted<CapturerTrackSource>(std::move(capturer));
    }
    return nullptr;
  }

 protected:
  explicit CapturerTrackSource(std::unique_ptr<TestVideoCapturer> capturer)
      : VideoTrackSource(/*remote=*/false), capturer_(std::move(capturer)) {}

  ~CapturerTrackSource() override = default;

 private:
  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return capturer_.get();
  }

  std::unique_ptr<TestVideoCapturer> capturer_;
};

// Rewrite any m=audio section in the SDP to use port 0, which (per RFC 3264
// §8.2) marks the section as rejected. Needed because the headless
// PeerConnectionFactory here is created with adm=nullptr, so
// WebRtcVoiceEngine skips AudioState creation; an incoming audio m-line
// would otherwise reach Call::CreateAudioReceiveStream with a null
// audio_state and segfault. Unified-plan handles port=0 by creating a
// stopped transceiver, so the data/video channels negotiate normally.
std::string DisableAudioMediaSection(const std::string& sdp) {
  std::istringstream iss(sdp);
  std::ostringstream oss;
  std::string line;
  bool modified = false;
  while (std::getline(iss, line)) {
    const bool had_cr = !line.empty() && line.back() == '\r';
    if (had_cr) line.pop_back();
    if (line.rfind("m=audio ", 0) == 0) {
      // Format: "m=audio <port> <proto> <fmt...>" — rewrite port to 0.
      const size_t p1 = line.find(' ');                  // after "m=audio"
      const size_t p2 = line.find(' ', p1 + 1);          // after port
      if (p1 != std::string::npos && p2 != std::string::npos) {
        line = "m=audio 0" + line.substr(p2);
        modified = true;
      }
    }
    oss << line;
    if (had_cr) oss << '\r';
    oss << '\n';
  }
  if (modified) {
    RTC_LOG(LS_INFO) << "DisableAudioMediaSection: rewrote m=audio port to 0";
  }
  return oss.str();
}

}  // namespace

Conductor::Conductor(const webrtc::Environment& env,
                     PeerConnectionClient* absl_nonnull client,
                     MainWindow* absl_nonnull main_wnd)
    : peer_id_(-1),
      loopback_(false),
      env_(env),
      client_(client),
      main_wnd_(main_wnd) {
  client_->RegisterObserver(this);
  main_wnd->RegisterObserver(this);
  datachannel_test_mode_ = absl::GetFlag(FLAGS_datachannel_test);
  rtp_sctp_mode_ = absl::GetFlag(FLAGS_rtp_sctp_mode);
  rtp_only_mode_ = absl::GetFlag(FLAGS_rtp_only_mode);
  test_duration_sec_ = absl::GetFlag(FLAGS_test_duration);

  // Pin the VP8 keyframe interval (libvpx kf_max_dist). The encoder settings
  // are created inside the WebRTC library (VideoEncoder::GetDefaultVp8Settings)
  // with no app-level hook, so the flag is transported via an env var that
  // the library reads when building the default VP8 settings.
  const int vp8_kf_max_dist = absl::GetFlag(FLAGS_vp8_kf_max_dist);
  setenv("WEBRTC_VP8_KF_MAX_DIST", std::to_string(vp8_kf_max_dist).c_str(),
         /*overwrite=*/1);
  RTC_LOG(LS_INFO) << "VP8 kf_max_dist pinned to " << vp8_kf_max_dist;
}

Conductor::~Conductor() {
  RTC_DCHECK(!peer_connection_);
}

bool Conductor::connection_active() const {
  return peer_connection_ != nullptr;
}

void Conductor::Close() {
  client_->SignOut();
  DeletePeerConnection();
}

bool Conductor::InitializePeerConnection() {
  RTC_DCHECK(!peer_connection_factory_);
  RTC_DCHECK(!peer_connection_);

  if (!signaling_thread_) {
    signaling_thread_ = webrtc::Thread::CreateWithSocketServer();
    signaling_thread_->Start();
  }

  webrtc::PeerConnectionFactoryDependencies deps;
  deps.signaling_thread = signaling_thread_.get();
  deps.env = env_;

  deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  deps.video_encoder_factory =
      std::make_unique<webrtc::VideoEncoderFactoryTemplate<
          webrtc::LibvpxVp8EncoderTemplateAdapter,
          webrtc::LibvpxVp9EncoderTemplateAdapter,
          webrtc::OpenH264EncoderTemplateAdapter,
          webrtc::LibaomAv1EncoderTemplateAdapter>>();
  deps.video_decoder_factory =
      std::make_unique<webrtc::VideoDecoderFactoryTemplate<
          webrtc::LibvpxVp8DecoderTemplateAdapter,
          webrtc::LibvpxVp9DecoderTemplateAdapter,
          webrtc::OpenH264DecoderTemplateAdapter,
          webrtc::Dav1dDecoderTemplateAdapter>>();

  // Don't create ADM - this will work without audio devices
  // Same approach as Modified WebRTC
  deps.audio_mixer = nullptr;
  deps.adm = nullptr;
  deps.audio_processing_builder = nullptr;

  webrtc::EnableMedia(deps);
  
  peer_connection_factory_ =
      webrtc::CreateModularPeerConnectionFactory(std::move(deps));

  if (!peer_connection_factory_) {
    main_wnd_->MessageBox("Error", "Failed to initialize PeerConnectionFactory",
                          true);
    DeletePeerConnection();
    return false;
  }

  if (!CreatePeerConnection()) {
    main_wnd_->MessageBox("Error", "CreatePeerConnection failed", true);
    DeletePeerConnection();
  }

  // AddTracks();  // Commented out for DataChannel-only test

  return peer_connection_ != nullptr;
}

bool Conductor::ReinitializePeerConnectionForLoopback() {
  loopback_ = true;
  std::vector<webrtc::scoped_refptr<webrtc::RtpSenderInterface>> senders =
      peer_connection_->GetSenders();
  peer_connection_ = nullptr;
  // Loopback is only possible if encryption is disabled.
  webrtc::PeerConnectionFactoryInterface::Options options;
  options.disable_encryption = true;
  peer_connection_factory_->SetOptions(options);
  if (CreatePeerConnection()) {
    for (const auto& sender : senders) {
      peer_connection_->AddTrack(sender->track(), sender->stream_ids());
    }
    peer_connection_->CreateOffer(
        this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  }
  options.disable_encryption = false;
  peer_connection_factory_->SetOptions(options);
  return peer_connection_ != nullptr;
}

bool Conductor::CreatePeerConnection() {
  RTC_DCHECK(peer_connection_factory_);
  RTC_DCHECK(!peer_connection_);

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  webrtc::PeerConnectionInterface::IceServer server;
  server.uri = GetPeerConnectionString();
  server.username = GetTurnUserName();
  server.password = GetTurnPassword();
  config.servers.push_back(server);

  webrtc::PeerConnectionDependencies pc_dependencies(this);
  auto error_or_peer_connection =
      peer_connection_factory_->CreatePeerConnectionOrError(
          config, std::move(pc_dependencies));
  if (error_or_peer_connection.ok()) {
    peer_connection_ = std::move(error_or_peer_connection.value());
  }

  // Apollo: in RTP-only mode there is no SCTP transport, so the
  // RtpSctpCoordinator that drives Pudica/AgentRtc/FSE (normally owned by
  // DcSctpTransport) is never created and every GCC-side static callback
  // no-ops -> the coordinator silently falls back to stock GCC. Create an
  // owned instance here so the BUR/Pudica RTP-rate controller engages.
  // Skipped for "disabled" (NC baseline must stay pure stock GCC) and when an
  // instance already exists (RTP+SCTP path owns its own).
  if (peer_connection_ && rtp_only_mode_ &&
      !webrtc::RtpSctpCoordinator::GetActiveInstance()) {
    const char* cm = std::getenv("COORDINATOR_MODE");
    std::string mode = cm ? cm : "";
    if (mode == "pudica" || mode == "agentrtc" || mode == "fse" ||
        mode == "fse_v2") {
      rtp_only_coordinator_ =
          std::make_unique<webrtc::RtpSctpCoordinator>(nullptr);
      RTC_LOG(LS_INFO) << "[APOLLO] Created RTP-only RtpSctpCoordinator (mode="
                       << mode << ")";
    }
  }

  return peer_connection_ != nullptr;
}

void Conductor::DeletePeerConnection() {
  // Stop stats collection before closing peer connection
  if (stats_collector_) {
    stats_collector_->Stop();
    stats_collector_.reset();
  }

  // Clean up demo mode channels
  if (context_data_observer_) {
    if (context_data_channel_)
      context_data_channel_->UnregisterObserver();
    context_data_observer_.reset();
  }
  context_data_channel_ = nullptr;

  if (control_dc_observer_) {
    if (control_channel_)
      control_channel_->UnregisterObserver();
    control_dc_observer_.reset();
  }
  control_channel_ = nullptr;

#if defined(ENABLE_LLAMA_INFERENCE)
  llm_adapter_.reset();
#endif

  main_wnd_->StopLocalRenderer();
  main_wnd_->StopRemoteRenderer();
  peer_connection_ = nullptr;
  rtp_only_coordinator_.reset();  // Apollo: release RTP-only coordinator
  peer_connection_factory_ = nullptr;
  local_video_source_ = nullptr;
  peer_id_ = -1;
  loopback_ = false;
}

void Conductor::EnsureStreamingUI() {
  RTC_DCHECK(peer_connection_);
  if (main_wnd_->IsWindow()) {
    if (main_wnd_->current_ui() != MainWindow::STREAMING) {
      main_wnd_->QueueUIThreadCallback(SWITCH_TO_STREAMING_UI, nullptr);
    }
  }
}

//
// PeerConnectionObserver implementation.
//

void Conductor::OnAddTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&
        streams) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " " << receiver->id();
  main_wnd_->QueueUIThreadCallback(NEW_TRACK_ADDED,
                                   receiver->track().release());
}

void Conductor::OnRemoveTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " " << receiver->id();
  main_wnd_->QueueUIThreadCallback(TRACK_REMOVED, receiver->track().release());
}

void Conductor::OnIceCandidate(const webrtc::IceCandidate* candidate) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " " << candidate->sdp_mline_index();
  // For loopback test. To save some connecting delay.
  if (loopback_) {
    if (!peer_connection_->AddIceCandidate(candidate)) {
      RTC_LOG(LS_WARNING) << "Failed to apply the received candidate";
    }
    return;
  }

  Json::Value jmessage;
  jmessage[kCandidateSdpMidName] = candidate->sdp_mid();
  jmessage[kCandidateSdpMlineIndexName] = candidate->sdp_mline_index();
  jmessage[kCandidateSdpName] = candidate->ToString();

  Json::StreamWriterBuilder factory;
  SendMessage(Json::writeString(factory, jmessage));
}

//
// PeerConnectionClientObserver implementation.
//

void Conductor::OnSignedIn() {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  main_wnd_->SwitchToPeerList(client_->peers());
}

void Conductor::OnDisconnected() {
  RTC_LOG(LS_INFO) << __FUNCTION__;

  DeletePeerConnection();

  if (main_wnd_->IsWindow())
    main_wnd_->SwitchToConnectUI();
}

void Conductor::OnPeerConnected(int id, const std::string& name) {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  // Refresh the list if we're showing it.
  if (main_wnd_->current_ui() == MainWindow::LIST_PEERS)
    main_wnd_->SwitchToPeerList(client_->peers());
}

void Conductor::OnPeerDisconnected(int id) {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  if (id == peer_id_) {
    RTC_LOG(LS_INFO) << "Our peer disconnected";
    main_wnd_->QueueUIThreadCallback(PEER_CONNECTION_CLOSED, nullptr);
  } else {
    // Refresh the list if we're showing it.
    if (main_wnd_->current_ui() == MainWindow::LIST_PEERS)
      main_wnd_->SwitchToPeerList(client_->peers());
  }
}

void Conductor::OnMessageFromPeer(int peer_id, const std::string& message) {
  RTC_DCHECK(peer_id_ == peer_id || peer_id_ == -1);
  RTC_DCHECK(!message.empty());

  if (!peer_connection_) {
    RTC_DCHECK(peer_id_ == -1);
    peer_id_ = peer_id;

    if (!InitializePeerConnection()) {
      RTC_LOG(LS_ERROR) << "Failed to initialize our PeerConnection instance";
      client_->SignOut();
      return;
    }
    // For DataChannel-only test, skip AddTracks() on non-initiator
    // AddTracks();  // Commented out for datachannel-only test

    // Create negotiated channels for demo mode (must be done on both sides)
    if (demo_mode_) {
      CreateControlChannel();
    }
  } else if (peer_id != peer_id_) {
    RTC_DCHECK(peer_id_ != -1);
    RTC_LOG(LS_WARNING)
        << "Received a message from unknown peer while already in a "
           "conversation with a different peer.";
    return;
  }

  Json::CharReaderBuilder factory;
  std::unique_ptr<Json::CharReader> reader =
      absl::WrapUnique(factory.newCharReader());
  Json::Value jmessage;
  if (!reader->parse(message.data(), message.data() + message.length(),
                     &jmessage, nullptr)) {
    RTC_LOG(LS_WARNING) << "Received unknown message. " << message;
    return;
  }
  std::string type_str;
  std::string json_object;

  webrtc::GetStringFromJsonObject(jmessage, kSessionDescriptionTypeName,
                                  &type_str);
  if (!type_str.empty()) {
    if (type_str == "offer-loopback") {
      // This is a loopback call.
      // Recreate the peerconnection with DTLS disabled.
      if (!ReinitializePeerConnectionForLoopback()) {
        RTC_LOG(LS_ERROR) << "Failed to initialize our PeerConnection instance";
        DeletePeerConnection();
        client_->SignOut();
      }
      return;
    }
    std::optional<webrtc::SdpType> type_maybe =
        webrtc::SdpTypeFromString(type_str);
    if (!type_maybe) {
      RTC_LOG(LS_ERROR) << "Unknown SDP type: " << type_str;
      return;
    }
    webrtc::SdpType type = *type_maybe;
    std::string sdp;
    if (!webrtc::GetStringFromJsonObject(jmessage, kSessionDescriptionSdpName,
                                         &sdp)) {
      RTC_LOG(LS_WARNING)
          << "Can't parse received session description message.";
      return;
    }
    // Optionally reject any audio media section. Required when the headless
    // factory is built with adm=nullptr (e.g. for Android AppRTCDemo peers
    // that include an audio m-line by default) — otherwise
    // Call::CreateAudioReceiveStream would segfault on a null AudioState.
    // Off by default to preserve upstream behavior.
    const std::string sanitized_sdp = absl::GetFlag(FLAGS_strip_audio_m_section)
                                          ? DisableAudioMediaSection(sdp)
                                          : sdp;
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(type, sanitized_sdp, &error);
    if (!session_description) {
      RTC_LOG(LS_WARNING)
          << "Can't parse received session description message. "
             "SdpParseError was: "
          << error.description;
      return;
    }
    RTC_LOG(LS_INFO) << " Received session description :" << message;
    peer_connection_->SetRemoteDescription(
        DummySetSessionDescriptionObserver::Create().get(),
        session_description.release());
    if (type == webrtc::SdpType::kOffer) {
      // Non-initiator: create matching negotiated DataChannels for MAFS multi-flow
      if ((rtp_sctp_mode_ || datachannel_test_mode_) &&
          !queries_csv_path_.empty() && sctp_flows_.empty()) {
        if (ParseTrafficConfig()) {
          CreateReceiverFlowChannels();

          // Initialize compute emulation on receiver side.
          // Active when either COMPUTE_EMULATION_ENABLE=true (legacy emulated
          // compute profile path) or COMPUTE_BACKEND=llama (real inference
          // via LlamaCppBackend — still goes through ComputeEmulationIntegration
          // which picks the backend internally).
          const char* compute_env = std::getenv("COMPUTE_EMULATION_ENABLE");
          const char* backend_env = std::getenv("COMPUTE_BACKEND");
          const bool emu_enabled =
              compute_env && std::string(compute_env) == "true";
          const bool llm_enabled =
              backend_env && std::string(backend_env) == "llama";
          if (emu_enabled || llm_enabled) {
            auto gpu_config = webrtc::coordinator::GpuConfig::FromEnvironment();
            compute_emulation_ = std::make_unique<webrtc::coordinator::ComputeEmulationIntegration>(
                log_dir_, gpu_config);
            for (auto& flow : sctp_flows_) {
              compute_emulation_->RegisterFlowFull(
                  flow->label, flow->stream_id,
                  1 /*query_id*/, flow->total_bytes,
                  flow->context_tokens, flow->decode_tokens,
                  flow->actual_decode_tokens, final_decode_tokens_,
                  flow->parallel_compute_ms,
                  flow->front_context_tokens, flow->front_text_content);
            }
            fprintf(stderr, "[COMPUTE-EMULATION] Initialized on receiver with %zu flows\n",
                    sctp_flows_.size());

            // Compute path is warmed up synchronously inside the backend Init
            // (LlamaCppBackend::Init → LLMEngine::load_model → GPU warm-up).
            // Mark ready now so OnControlChannelOpen (or this path, if the
            // control channel opened first) can emit READY to the sender.
            compute_ready_ = true;
            if (control_channel_ &&
                control_channel_->state() ==
                    webrtc::DataChannelInterface::kOpen &&
                !receiver_ready_sent_ &&
                (sequence_entries_.size() > 1 || max_queries_ > 0)) {
              SendControlMessage(std::string("READY"));
              receiver_ready_sent_ = true;
            }

            // Wire query completion callback for receiver-driven mode.
            // Active when either (a) the CSV has multiple sequence entries,
            // or (b) MAX_QUERIES > 0 (single entry replayed N times with
            // real receiver-side compute gating each iteration; the N=1
            // case still goes through this path so the compute-aware exit
            // runs and query_response_time.csv gets its row).
            if (sequence_entries_.size() > 1 || max_queries_ > 0) {
              compute_emulation_->SetExternalQueryCompleteCallback(
                  [this](uint64_t query_id) {
                    OnReceiverQueryComplete(query_id);
                  });
              fprintf(stderr,
                      "[CTRL] Receiver-driven mode: %zu sets, max_queries=%d, "
                      "callback wired\n",
                      sequence_entries_.size(), max_queries_);
            }
          }
        }
      }

      // Non-initiator: add video tracks before creating answer (RTP+SCTP only)
      // In demo mode, receiver doesn't need to send video — just receive
      if (rtp_sctp_mode_) {
        printf("[RTP+SCTP] Non-initiator adding video track before CreateAnswer\n");
        fflush(stdout);
        AddTracks();
      }
      peer_connection_->CreateAnswer(
          this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
    }
  } else {
    std::string sdp_mid;
    int sdp_mlineindex = 0;
    std::string sdp;
    if (!webrtc::GetStringFromJsonObject(jmessage, kCandidateSdpMidName,
                                         &sdp_mid) ||
        !webrtc::GetIntFromJsonObject(jmessage, kCandidateSdpMlineIndexName,
                                      &sdp_mlineindex) ||
        !webrtc::GetStringFromJsonObject(jmessage, kCandidateSdpName, &sdp)) {
      RTC_LOG(LS_WARNING) << "Can't parse received message.";
      return;
    }
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidate> candidate(
        webrtc::CreateIceCandidate(sdp_mid, sdp_mlineindex, sdp, &error));
    if (!candidate) {
      RTC_LOG(LS_WARNING) << "Can't parse received candidate message. "
                             "SdpParseError was: "
                          << error.description;
      return;
    }
    if (!peer_connection_->AddIceCandidate(candidate.get())) {
      RTC_LOG(LS_WARNING) << "Failed to apply the received candidate";
      return;
    }
    RTC_LOG(LS_INFO) << " Received candidate :" << message;
  }
}

void Conductor::OnMessageSent(int err) {
  // Process the next pending message if any.
  main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, nullptr);
}

void Conductor::OnServerConnectionFailure() {
  main_wnd_->MessageBox("Error", ("Failed to connect to " + server_).c_str(),
                        true);
}

//
// MainWndCallback implementation.
//

void Conductor::StartLogin(const std::string& server, int port) {
  if (client_->is_connected())
    return;
  server_ = server;
  client_->Connect(server, port, GetPeerName());
}

void Conductor::DisconnectFromServer() {
  if (client_->is_connected())
    client_->SignOut();
}

void Conductor::ConnectToPeer(int peer_id) {
  RTC_DCHECK(peer_id_ == -1);
  RTC_DCHECK(peer_id != -1);

  if (peer_connection_) {
    main_wnd_->MessageBox(
        "Error", "We only support connecting to one peer at a time", true);
    return;
  }

  if (InitializePeerConnection()) {
    peer_id_ = peer_id;
    is_caller_ = true;

    // MAFS multi-flow: parse traffic config and create per-flow DataChannels
    if ((rtp_sctp_mode_ || datachannel_test_mode_) &&
        !queries_csv_path_.empty() && ParseTrafficConfig()) {
      CreateMultiFlowDataChannels();
    } else if (datachannel_test_mode_ || rtp_sctp_mode_) {
      // Fallback: single DataChannel
      CreateDataChannel();
    }

    // Create control channel for demo mode
    if (demo_mode_) {
      CreateControlChannel();
    }

    // Add RTP tracks for RTP+SCTP mode, RTP-only mode, or demo mode with Y4M
    if (rtp_sctp_mode_ || rtp_only_mode_ ||
        (demo_mode_ && !y4m_path_.empty())) {
      printf("[%s] Adding audio/video tracks...\n",
             rtp_only_mode_ ? "RTP-ONLY" :
             demo_mode_ ? "DEMO" : "RTP+SCTP");
      fflush(stdout);
      AddTracks();
    }

    peer_connection_->CreateOffer(
        this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  } else {
    main_wnd_->MessageBox("Error", "Failed to initialize PeerConnection", true);
  }
}

void Conductor::AddTracks() {
  // In DataChannel test mode, skip adding audio/video tracks UNLESS video is also needed
  // Temporarily commented out to test RTP+DataChannel together
  // if (datachannel_test_mode_) {
  //   RTC_LOG(LS_INFO) << "DataChannel test mode: skipping audio/video tracks";
  //   main_wnd_->SwitchToStreamingUI();
  //   return;
  // }

  if (!peer_connection_->GetSenders().empty()) {
    return;  // Already added tracks.
  }

  // In headless/demo modes, skip audio to avoid audio device issues
  if (!rtp_sctp_mode_ && !rtp_only_mode_ && !demo_mode_) {
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track(
        peer_connection_factory_->CreateAudioTrack(
            kAudioLabel,
            peer_connection_factory_->CreateAudioSource(webrtc::AudioOptions())
                .get()));
    auto result_or_error = peer_connection_->AddTrack(audio_track, {kStreamId});
    if (!result_or_error.ok()) {
      RTC_LOG(LS_ERROR) << "Failed to add audio track to PeerConnection: "
                        << result_or_error.error().message();
    }
  } else {
    printf("[%s] Skipping audio track\n",
           demo_mode_ ? "DEMO" : "RTP+SCTP");
    fflush(stdout);
  }

  // Try Y4M file if specified
  if (!y4m_path_.empty()) {
    printf("[VIDEO] Using Y4M file: %s\n", y4m_path_.c_str());
    fflush(stdout);
    
    std::unique_ptr<webrtc::test::Y4mFrameGenerator> frame_generator(
        new webrtc::test::Y4mFrameGenerator(
            y4m_path_,
            webrtc::test::Y4mFrameGenerator::RepeatMode::kLoop));
    
    if (frame_generator) {
      int fps = video_fps_ > 0 ? video_fps_ : 30;
      auto video_capturer = std::make_unique<webrtc::test::FrameGeneratorCapturer>(
          webrtc::Clock::GetRealTimeClock(),
          std::move(frame_generator),
          fps,
          env_.task_queue_factory());
      
      if (video_capturer) {
        video_capturer->Start();
        printf("[VIDEO] Y4M capturer started at %d fps\n", fps);
        fflush(stdout);
        
        // Create a simple VideoTrackSource wrapper
        local_video_source_ = webrtc::make_ref_counted<CapturerTrackSource>(
            std::move(video_capturer));
        
        webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_(
            peer_connection_factory_->CreateVideoTrack(local_video_source_,
                                                       kVideoLabel));
        if (!rtp_sctp_mode_) {  // Skip local renderer in headless mode
          main_wnd_->StartLocalRenderer(video_track_.get());
        }
        
        auto result_or_error = peer_connection_->AddTrack(video_track_, {kStreamId});
        if (result_or_error.ok()) {
          printf("[VIDEO] Y4M video track added successfully!\n");
          fflush(stdout);
          
          // Set bitrate settings on PeerConnection for faster ramp-up
          if (max_bitrate_kbps_ > 0) {
            webrtc::BitrateSettings bitrate_settings;
            int max_bps = max_bitrate_kbps_ * 1000;
            int start_bps = std::min(max_bps, 10000000);  // Start at 10 Mbps
            int min_bps = std::min(max_bps, std::min(start_bps, 500000));  // Min 500 kbps
            bitrate_settings.min_bitrate_bps = min_bps;
            bitrate_settings.start_bitrate_bps = start_bps;
            bitrate_settings.max_bitrate_bps = max_bps;
            peer_connection_->SetBitrate(bitrate_settings);
            printf("[VIDEO] Set PeerConnection bitrate: min=%d, start=%d, max=%d kbps\n", 
                   min_bps/1000, start_bps/1000, max_bps/1000);
            fflush(stdout);
            
            // Also set per-sender encoding parameters
            auto senders = peer_connection_->GetSenders();
            for (const auto& sender : senders) {
              if (sender->track() && sender->track()->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
                webrtc::RtpParameters params = sender->GetParameters();
                for (auto& encoding : params.encodings) {
                  encoding.max_bitrate_bps = max_bps;
                  encoding.min_bitrate_bps = min_bps;
                }
                sender->SetParameters(params);
                printf("[VIDEO] Set sender encoding: max=%d kbps\n", max_bitrate_kbps_);
                fflush(stdout);
              }
            }
          }
        } else {
          RTC_LOG(LS_ERROR) << "Failed to add Y4M video track: " << result_or_error.error().message();
        }
      }
    } else {
      RTC_LOG(LS_WARNING) << "Failed to create Y4M frame generator";
    }
  } else {
    // Fallback to camera or synthetic video
    local_video_source_ = CapturerTrackSource::Create(env_.task_queue_factory());
    if (local_video_source_) {
      webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_(
          peer_connection_factory_->CreateVideoTrack(local_video_source_,
                                                     kVideoLabel));
      if (!rtp_sctp_mode_) {  // Skip local renderer in headless mode
        main_wnd_->StartLocalRenderer(video_track_.get());
      }

      auto result_or_error = peer_connection_->AddTrack(video_track_, {kStreamId});
      if (!result_or_error.ok()) {
        RTC_LOG(LS_ERROR) << "Failed to add video track to PeerConnection: "
                          << result_or_error.error().message();
      }
    } else {
      RTC_LOG(LS_WARNING)
          << "No local video track; proceeding without local video";
    }
  }

  main_wnd_->SwitchToStreamingUI();
}

void Conductor::DisconnectFromCurrentPeer() {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  if (peer_connection_) {
    client_->SendHangUp(peer_id_);
    DeletePeerConnection();
  }

  if (main_wnd_->IsWindow())
    main_wnd_->SwitchToPeerList(client_->peers());
}

void Conductor::UIThreadCallback(int msg_id, void* data) {
  switch (msg_id) {
    case PEER_CONNECTION_CLOSED:
      RTC_LOG(LS_INFO) << "PEER_CONNECTION_CLOSED";
      DeletePeerConnection();

      if (main_wnd_->IsWindow()) {
        if (client_->is_connected()) {
          main_wnd_->SwitchToPeerList(client_->peers());
        } else {
          main_wnd_->SwitchToConnectUI();
        }
      } else {
        DisconnectFromServer();
      }
      break;

    case SEND_MESSAGE_TO_PEER: {
      RTC_LOG(LS_INFO) << "SEND_MESSAGE_TO_PEER";
      std::string* msg = reinterpret_cast<std::string*>(data);
      if (msg) {
        // For convenience, we always run the message through the queue.
        // This way we can be sure that messages are sent to the server
        // in the same order they were signaled without much hassle.
        pending_messages_.push_back(msg);
      }

      if (!pending_messages_.empty() && !client_->IsSendingMessage()) {
        msg = pending_messages_.front();
        pending_messages_.pop_front();

        if (!client_->SendToPeer(peer_id_, *msg) && peer_id_ != -1) {
          RTC_LOG(LS_ERROR) << "SendToPeer failed";
          DisconnectFromServer();
        }
        delete msg;
      }

      if (!peer_connection_)
        peer_id_ = -1;

      break;
    }

    case NEW_TRACK_ADDED: {
      auto* track = reinterpret_cast<webrtc::MediaStreamTrackInterface*>(data);
      if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track);
        printf("[VIDEO] Remote video track received, starting renderer\n");
        fflush(stdout);
        EnsureStreamingUI();
        main_wnd_->StartRemoteRenderer(video_track);
      }
      track->Release();
      break;
    }

    case TRACK_REMOVED: {
      // Remote peer stopped sending a track.
      auto* track = reinterpret_cast<webrtc::MediaStreamTrackInterface*>(data);
      // Ensure we detach our renderer before releasing the track to avoid
      // referencing a destroyed track from the renderer.
      main_wnd_->StopRemoteRenderer();
      track->Release();
      break;
    }

    case SWITCH_TO_STREAMING_UI:
      if (main_wnd_->IsWindow() &&
          main_wnd_->current_ui() != MainWindow::STREAMING) {
        main_wnd_->SwitchToStreamingUI();
      }
      break;

    default:
      RTC_DCHECK_NOTREACHED();
      break;
  }
}

void Conductor::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
  peer_connection_->SetLocalDescription(
      DummySetSessionDescriptionObserver::Create().get(), desc);

  std::string sdp;
  desc->ToString(&sdp);

  // For loopback test. To save some connecting delay.
  if (loopback_) {
    // Replace message type from "offer" to "answer"
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp);
    peer_connection_->SetRemoteDescription(
        DummySetSessionDescriptionObserver::Create().get(),
        session_description.release());
    return;
  }

  Json::Value jmessage;
  jmessage[kSessionDescriptionTypeName] =
      webrtc::SdpTypeToString(desc->GetType());
  jmessage[kSessionDescriptionSdpName] = sdp;

  Json::StreamWriterBuilder factory;
  SendMessage(Json::writeString(factory, jmessage));
}

void Conductor::OnFailure(webrtc::RTCError error) {
  RTC_LOG(LS_ERROR) << ToString(error.type()) << ": " << error.message();
}

void Conductor::SendMessage(const std::string& json_object) {
  // Use HTTP POST for signaling (like Modified implementation)
  if (!room_id_.empty() && !client_id_.empty()) {
    CURL* curl = curl_easy_init();
    if (!curl) {
      RTC_LOG(LS_ERROR) << "Failed to init CURL for SendMessage";
      return;
    }
    
    std::string post_url = "https://" + signaling_server_ + "/message/" + room_id_ + "/" + client_id_;
    std::string response;
    
    struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, post_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_object.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, json_object.length());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
      ((std::string*)userdata)->append(ptr, size * nmemb);
      return size * nmemb;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    
    printf("[WS] POST %s: %s\n", post_url.c_str(), json_object.substr(0, 80).c_str());
    fflush(stdout);
    
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      RTC_LOG(LS_ERROR) << "POST failed: " << curl_easy_strerror(res);
    }
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
  } else {
    // Fall back to UI-based signaling
    std::string* msg = new std::string(json_object);
    main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, msg);
  }
}

//
// DataChannel throughput test implementation.
//

void Conductor::DCObserver::OnStateChange() {
  if (!conductor_->data_channel_) return;
  
  webrtc::DataChannelInterface::DataState state = 
      conductor_->data_channel_->state();
  
  if (state == webrtc::DataChannelInterface::kOpen) {
    printf("[DC] DataChannel OPEN\n");
    fflush(stdout);
    if (conductor_->is_caller_) {
      // Caller starts bulk send - run in separate thread with delay
      std::thread([conductor = conductor_]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        conductor->StartBulkSend();
      }).detach();
    }
  }
}

void Conductor::DCObserver::OnBufferedAmountChange(uint64_t sent_data_size) {
  // Event-driven: when buffer drains below threshold, pump more data
  if (!conductor_->bulk_send_active_.load()) return;
  
  uint64_t buffered = conductor_->data_channel_->buffered_amount();
  constexpr uint64_t kLowThreshold = 512 * 1024;  // 512KB threshold
  
  if (buffered < kLowThreshold) {
    conductor_->PumpData();
  }
}

void Conductor::DCObserver::OnMessage(const webrtc::DataBuffer& buffer) {
  // Receiver side: measure throughput (minimal overhead)
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  
  if (!conductor_->transfer_started_) {
    conductor_->transfer_started_ = true;
    conductor_->receive_start_time_ms_ = now_ms;
    conductor_->last_report_time_ms_ = now_ms;
    conductor_->last_report_bytes_ = 0;
    conductor_->bytes_received_ = 0;
  }
  
  conductor_->bytes_received_ += buffer.size();
  conductor_->main_wnd_->OnSctpDataReceived(buffer.size());
  if (auto* coord = webrtc::RtpSctpCoordinator::GetActiveInstance()) {
    coord->OnSctpDataReceived(buffer.size());
  }
  
  // Report every second (low overhead - no printf in hot path)
  if (now_ms - conductor_->last_report_time_ms_ >= 1000) {
    uint64_t current_bytes = conductor_->bytes_received_.load();
    uint64_t delta_bytes = current_bytes - conductor_->last_report_bytes_;
    int64_t delta_time_ms = now_ms - conductor_->last_report_time_ms_;
    double throughput_mbps = (delta_bytes * 8.0) / (delta_time_ms * 1000.0);
    
    // Use RTC_LOG instead of printf for lower overhead
    RTC_LOG(LS_INFO) << "[RX] " << (now_ms - conductor_->receive_start_time_ms_) 
                     << "ms: " << throughput_mbps << " Mbps";
    
    conductor_->last_report_time_ms_ = now_ms;
    conductor_->last_report_bytes_ = current_bytes;
  }
}

void Conductor::OnDataChannel(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  RTC_LOG(LS_INFO) << "[DC] OnDataChannel: label=" << channel->label()
                   << " id=" << channel->id();

  // Accept negotiated control channel in demo mode
  if (demo_mode_ && channel->label() == "control") {
    control_channel_ = channel;
    control_dc_observer_ = std::make_unique<ControlDCObserver>(this);
    control_channel_->RegisterObserver(control_dc_observer_.get());
    RTC_LOG(LS_INFO) << "[DEMO] Accepted control channel";
    return;
  }

  // Need to handle DataChannel in both datachannel_test_mode and rtp_sctp_mode
  if (!datachannel_test_mode_ && !rtp_sctp_mode_) return;

  RTC_LOG(LS_INFO) << "[DC] OnDataChannel: label=" << channel->label()
                    << " id=" << channel->id();

  // For MAFS multi-flow (negotiated channels): receiver also creates matching channels
  // negotiated=true means both sides create channels independently, so OnDataChannel
  // is only called for non-negotiated channels. For negotiated multi-flow,
  // receiver must also call CreateMultiFlowDataChannels.

  // Default: single channel fallback
  data_channel_ = channel;
  dc_observer_ = std::make_unique<DCObserver>(this);
  data_channel_->RegisterObserver(dc_observer_.get());
}

void Conductor::OnIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  RTC_LOG(LS_INFO) << "[ICE] Connection state changed to " << (int)new_state;
  if (new_state == webrtc::PeerConnectionInterface::kIceConnectionConnected) {
    printf("[ICE] Connected!\n");
    fflush(stdout);

    // Ensure UI switches to streaming mode when ICE connects
    EnsureStreamingUI();

    // Start RTP stats collection (only once)
    if (!log_dir_.empty() && peer_connection_ && !stats_collector_) {
      stats_collector_ = std::make_unique<RTCStatsCollector>();
      if (main_wnd_) {
        stats_collector_->SetExternalSctpBytesGetter(
            [wnd = main_wnd_]() { return wnd->GetSctpBytesReceived(); });
      }
      if (stats_collector_->Start(log_dir_, peer_connection_)) {
        RTC_LOG(LS_INFO) << "[STATS] Started stats collection to " << log_dir_;
      }
    }

    // Sender: load context files on connection
    if (demo_mode_ && is_sender_) {
      LoadDemoContext();
    }

#if defined(ENABLE_LLAMA_INFERENCE)
    // LLM adapter is initialized lazily in ProcessReceivedContext() to avoid
    // race conditions with auto-query and to ensure model loads only when needed.
    // Don't eagerly load here — it blocks the signaling thread during model load.
#endif
  }
}

void Conductor::CreateDataChannel() {
  if (!peer_connection_ || (!datachannel_test_mode_ && !rtp_sctp_mode_)) return;
  
  webrtc::DataChannelInit config;
  config.ordered = true;
  config.reliable = true;
  
  auto result = peer_connection_->CreateDataChannelOrError("throughput_test", &config);
  if (result.ok()) {
    data_channel_ = result.MoveValue();
    dc_observer_ = std::make_unique<DCObserver>(this);
    data_channel_->RegisterObserver(dc_observer_.get());
    RTC_LOG(LS_INFO) << "[DC] DataChannel created";
  } else {
    RTC_LOG(LS_ERROR) << "[DC] Failed to create DataChannel";
  }
}

void Conductor::StartBulkSend() {
  if (!data_channel_ || data_channel_->state() != webrtc::DataChannelInterface::kOpen) {
    RTC_LOG(LS_ERROR) << "[DC] StartBulkSend: channel not ready";
    return;
  }
  
  // Initialize event-driven bulk send state
  constexpr size_t kChunkSize = 256 * 1024;  // 256KB chunks for efficiency
  bulk_send_chunk_.resize(kChunkSize, 'X');
  
  bulk_send_start_time_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  bulk_send_end_time_ms_ = bulk_send_start_time_ms_ + (test_duration_sec_ * 1000);
  bulk_send_total_sent_ = 0;
  bulk_send_last_stats_time_ms_ = bulk_send_start_time_ms_;
  bulk_send_last_bytes_sent_ = 0;
  
  // Open CSV log file if log_dir is set
  if (!log_dir_.empty()) {
    std::string csv_path = log_dir_ + "/sctp_transport.csv";
    bulk_send_csv_file_ = std::make_unique<std::ofstream>(csv_path);
    if (bulk_send_csv_file_->is_open()) {
      *bulk_send_csv_file_ << "timestamp_ms,elapsed_ms,bytes_sent,buffered_amount,instant_mbps,avg_mbps\n";
      printf("[DC] SCTP stats logging to: %s\n", csv_path.c_str());
      fflush(stdout);
    }
  }
  
  printf("[DC] Event-driven bulk send started (%ds) - NO POLLING!\n", test_duration_sec_);
  fflush(stdout);
  
  // Activate event-driven sending
  bulk_send_active_ = true;
  
  // Prime the pump - fill buffer initially
  PumpData();
  
  // NOTE: This returns immediately! OnBufferedAmountChange will drive further sends.
}

void Conductor::PumpData() {
  if (!bulk_send_active_.load() || !data_channel_) return;
  
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  
  // Check if time is up
  if (now_ms >= bulk_send_end_time_ms_) {
    FinishBulkSend();
    return;
  }
  
  constexpr uint64_t kHighWaterMark = 1536 * 1024;  // 1.5MB max buffer
  
  // Fill buffer up to high water mark
  while (data_channel_->buffered_amount() < kHighWaterMark && bulk_send_active_.load()) {
    webrtc::DataBuffer buffer(
        webrtc::CopyOnWriteBuffer(bulk_send_chunk_.data(), bulk_send_chunk_.size()), true);
    data_channel_->SendAsync(std::move(buffer), nullptr);
    bulk_send_total_sent_ += bulk_send_chunk_.size();
  }
  
  // Log stats periodically (every 100ms)
  constexpr int64_t kStatsIntervalMs = 100;
  if (bulk_send_csv_file_ && bulk_send_csv_file_->is_open() && 
      (now_ms - bulk_send_last_stats_time_ms_ >= kStatsIntervalMs)) {
    int64_t elapsed_ms = now_ms - bulk_send_start_time_ms_;
    uint64_t buffered = data_channel_->buffered_amount();
    uint64_t total = bulk_send_total_sent_.load();
    int64_t delta_bytes = total - bulk_send_last_bytes_sent_;
    int64_t delta_time_ms = now_ms - bulk_send_last_stats_time_ms_;
    double instant_mbps = (delta_bytes * 8.0) / (delta_time_ms * 1000.0);
    double avg_mbps = (total * 8.0) / (elapsed_ms * 1000.0);
    
    *bulk_send_csv_file_ << now_ms << "," << elapsed_ms << "," << total << ","
              << buffered << "," << instant_mbps << "," << avg_mbps << "\n";
    
    bulk_send_last_stats_time_ms_ = now_ms;
    bulk_send_last_bytes_sent_ = total;
  }
}

void Conductor::FinishBulkSend() {
  if (!bulk_send_active_.exchange(false)) return;  // Already finished
  
  // Send end marker
  std::vector<uint8_t> end_marker = {'E', 'N', 'D'};
  webrtc::DataBuffer end_buffer(
      webrtc::CopyOnWriteBuffer(end_marker.data(), end_marker.size()), true);
  data_channel_->Send(end_buffer);
  
  int64_t actual_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count() - bulk_send_start_time_ms_;
  
  uint64_t total = bulk_send_total_sent_.load();
  double avg_throughput_mbps = (total * 8.0) / (actual_duration_ms * 1000.0);
  
  // Close CSV file
  if (bulk_send_csv_file_ && bulk_send_csv_file_->is_open()) {
    bulk_send_csv_file_->close();
  }
  
  printf("[DC] Event-driven send complete. Total: %lu bytes, Throughput: %.2f Mbps, Duration: %ld ms\n",
         total, avg_throughput_mbps, actual_duration_ms);
  fflush(stdout);
}

void Conductor::PrintThroughputStats() {
  if (!transfer_started_) {
    RTC_LOG(LS_INFO) << "No data received yet";
    return;
  }
  
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t duration_ms = now_ms - receive_start_time_ms_;
  uint64_t total_bytes = bytes_received_.load();
  
  double avg_throughput_mbps = (total_bytes * 8.0) / (duration_ms * 1000.0);
  
  RTC_LOG(LS_INFO) << "Final stats - Total received: " << total_bytes 
                   << " bytes, Duration: " << duration_ms << " ms"
                   << ", Avg throughput: " << avg_throughput_mbps << " Mbps";
}

// ============================================================================
// Demo Mode Implementation
// ============================================================================

void Conductor::CreateControlChannel() {
  if (!peer_connection_)
    return;

  if (demo_mode_) {
    // Demo mode: Control channel (id=99) for JSON messages
    webrtc::DataChannelInit config;
    config.negotiated = true;
    config.ordered = true;
    config.id = 99;

    auto result =
        peer_connection_->CreateDataChannelOrError("control", &config);
    if (result.ok()) {
      control_channel_ = result.MoveValue();
      control_dc_observer_ = std::make_unique<ControlDCObserver>(this);
      control_channel_->RegisterObserver(control_dc_observer_.get());
      RTC_LOG(LS_INFO) << "[DEMO] Control channel created (id=99)";
    } else {
      RTC_LOG(LS_ERROR) << "[DEMO] Failed to create control channel";
    }

    // Context data channel (id=100) for bulk binary transfer
    webrtc::DataChannelInit data_config;
    data_config.negotiated = true;
    data_config.ordered = true;
    data_config.id = 100;

    auto data_result =
        peer_connection_->CreateDataChannelOrError("context_data", &data_config);
    if (data_result.ok()) {
      context_data_channel_ = data_result.MoveValue();
      context_data_observer_ = std::make_unique<ContextDataObserver>(this);
      context_data_channel_->RegisterObserver(context_data_observer_.get());
      RTC_LOG(LS_INFO) << "[DEMO] Context data channel created (id=100)";
    } else {
      RTC_LOG(LS_ERROR) << "[DEMO] Failed to create context data channel";
    }
  } else {
    // MAFS mode: Control channel (stream_id=0) for plain-text messages
    webrtc::DataChannelInit config;
    config.ordered = true;
    config.reliable = true;
    config.id = 0;            // stream_id 0 = control
    config.negotiated = true;
    auto result = peer_connection_->CreateDataChannelOrError("_control", &config);
    if (result.ok()) {
      control_channel_ = result.MoveValue();
      control_observer_ = std::make_unique<ControlObserver>(this);
      control_channel_->RegisterObserver(control_observer_.get());
      fprintf(stderr, "[CTRL] Created control channel (stream_id=0, negotiated)\n");
    } else {
      fprintf(stderr, "[CTRL] WARNING: Failed to create control channel\n");
    }
  }
}

void Conductor::ControlDCObserver::OnStateChange() {
  if (!conductor_->control_channel_)
    return;

  auto state = conductor_->control_channel_->state();
  printf("[DEMO] Control channel state: %d\n", static_cast<int>(state));
  fflush(stdout);

  if (state == webrtc::DataChannelInterface::kOpen) {
    printf("[DEMO] Control channel OPEN\n");
    fflush(stdout);

    // Non-demo receiver: emit READY once the control channel opens, provided
    // the compute-path warmup has already completed (compute_ready_ is set
    // synchronously when compute_emulation_ is constructed in the OFFER
    // handler). The sender gates its first flow send on this message.
    if (!conductor_->demo_mode_ && !conductor_->is_caller_ &&
        conductor_->compute_ready_ && !conductor_->receiver_ready_sent_ &&
        (conductor_->sequence_entries_.size() > 1 ||
         conductor_->max_queries_ > 0)) {
      conductor_->SendControlMessage(std::string("READY"));
      conductor_->receiver_ready_sent_ = true;
    }

    // Auto-query: send first query after a delay when control channel opens
    // (receiver side only, when demo_mode is active)
    if (conductor_->demo_mode_ && !conductor_->is_sender_) {
      std::thread([conductor = conductor_]() {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::string auto_query = "Summarize the key points of this document.";
        printf("[DEMO] Auto-query: %s\n", auto_query.c_str());
        fflush(stdout);
        if (conductor->main_wnd_) {
          conductor->main_wnd_->AppendChatMessage("You", auto_query);
          conductor->main_wnd_->OnQueryStarted();
        }
        conductor->OnQuerySubmitted(auto_query);
      }).detach();
    }
  }
}

void Conductor::ControlDCObserver::OnMessage(
    const webrtc::DataBuffer& buffer) {
  std::string msg(reinterpret_cast<const char*>(buffer.data.data()),
                  buffer.data.size());
  conductor_->OnControlMessage(msg);
}

// ContextDataObserver: accumulates binary chunks from sender
void Conductor::ContextDataObserver::OnStateChange() {
  if (!conductor_->context_data_channel_)
    return;
  auto state = conductor_->context_data_channel_->state();
  printf("[DEMO] Context data channel state: %d\n", static_cast<int>(state));
  fflush(stdout);
}

void Conductor::ContextDataObserver::OnMessage(
    const webrtc::DataBuffer& buffer) {
  const uint8_t* data = buffer.data.data<uint8_t>();
  size_t size = buffer.data.size();
  conductor_->recv_context_buffer_.insert(
      conductor_->recv_context_buffer_.end(), data, data + size);

  // Notify UI of SCTP data for performance graph
  conductor_->main_wnd_->OnSctpDataReceived(size);
  if (auto* coord = webrtc::RtpSctpCoordinator::GetActiveInstance()) {
    coord->OnSctpDataReceived(size);
  }

  // If context_done already arrived and we've now received all data, process it
  conductor_->TryProcessPendingContext();
}

// Helper: send JSON on control channel
void Conductor::SendControlMessage(const Json::Value& msg) {
  if (!control_channel_ ||
      control_channel_->state() != webrtc::DataChannelInterface::kOpen) {
    RTC_LOG(LS_WARNING) << "[DEMO] Control channel not open, cannot send";
    return;
  }
  Json::StreamWriterBuilder builder;
  std::string json = Json::writeString(builder, msg);
  webrtc::DataBuffer buffer(
      webrtc::CopyOnWriteBuffer(json.data(), json.size()), false);
  control_channel_->Send(buffer);
}

// Sender: load context files from disk
void Conductor::LoadDemoContext() {
  if (!is_sender_ || !demo_mode_)
    return;

  // Load context documents from context_dir (00.txt, 01.txt, 02.txt)
  if (!context_dir_.empty()) {
    context_texts_.clear();
    for (int i = 0; i < 3; ++i) {
      char fname[32];
      snprintf(fname, sizeof(fname), "%02d.txt", i);
      std::string path = context_dir_ + "/" + fname;
      std::ifstream f(path);
      if (f.good()) {
        std::string text((std::istreambuf_iterator<char>(f)), {});
        context_texts_.push_back(std::move(text));
        printf("[DEMO] Loaded context text[%d]: %zu bytes from %s\n",
               i, context_texts_.back().size(), path.c_str());
      } else {
        printf("[DEMO] Context file not found (ok): %s\n", path.c_str());
      }
    }
    printf("[DEMO] Total context documents loaded: %zu\n", context_texts_.size());
    fflush(stdout);
  }

  // Load raw text context
  if (!context_path_.empty()) {
    std::ifstream f(context_path_);
    if (f.good()) {
      context_text_.assign(std::istreambuf_iterator<char>(f), {});
      printf("[DEMO] Loaded context text: %zu bytes from %s\n",
             context_text_.size(), context_path_.c_str());
    } else {
      printf("[DEMO] WARNING: Cannot open context_path: %s\n",
             context_path_.c_str());
    }
    fflush(stdout);
  }

  // Load pre-built KV cache
  if (!kvcache_path_.empty()) {
    std::ifstream f(kvcache_path_, std::ios::binary | std::ios::ate);
    if (f.good()) {
      auto size = f.tellg();
      f.seekg(0);
      kvcache_data_.resize(size);
      f.read(reinterpret_cast<char*>(kvcache_data_.data()), size);
      printf("[DEMO] Loaded KV cache: %zu bytes from %s\n",
             kvcache_data_.size(), kvcache_path_.c_str());

      // Pre-compress KV cache with zstd for kvzip mode
      if (context_method_ == "kvzip" && !kvcache_data_.empty()) {
        size_t src_size = kvcache_data_.size();
        size_t bound = ZSTD_compressBound(src_size);
        kvcache_compressed_.resize(bound);
        size_t compressed_size = ZSTD_compress(
            kvcache_compressed_.data(), bound,
            kvcache_data_.data(), src_size, 3);  // level 3 = good speed/ratio
        if (ZSTD_isError(compressed_size)) {
          printf("[DEMO] WARNING: zstd compression failed: %s\n",
                 ZSTD_getErrorName(compressed_size));
          kvcache_compressed_.clear();
        } else {
          kvcache_compressed_.resize(compressed_size);
          printf("[DEMO] KV cache compressed: %zu -> %zu bytes (%.1f%% of original)\n",
                 src_size, compressed_size,
                 compressed_size * 100.0 / src_size);
        }
      }
    } else {
      printf("[DEMO] WARNING: Cannot open kvcache_path: %s\n",
             kvcache_path_.c_str());
    }
    fflush(stdout);
  }
}

void Conductor::OnControlMessage(const std::string& json) {
  // MAFS plain-text control protocol (non-demo mode)
  if (!demo_mode_) {
    if (json.rfind("NEXT:", 0) == 0 && is_caller_) {
      // Parse set_index from "NEXT:set_index=N"
      int set_index = 0;
      auto pos = json.find("set_index=");
      if (pos != std::string::npos) {
        set_index = std::stoi(json.substr(pos + 10));
      }
      fprintf(stderr, "[CTRL] Sender: received NEXT for set %d\n", set_index);
      waiting_for_receiver_ = false;
      current_set_index_ = set_index;

      // Load new set metadata and restart send on a separate thread
      std::thread([this, set_index]() {
        if (LoadSetByIndex(set_index)) {
          StartMultiFlowSend();
        } else {
          fprintf(stderr, "[CTRL] ERROR: LoadSetByIndex(%d) failed\n", set_index);
        }
      }).detach();
    } else if (json == "READY" && is_caller_) {
      fprintf(stderr, "[CTRL] Sender: received READY from receiver\n");
      ready_received_ = true;
      // If all flow channels are already open, kick off the first send now.
      // Otherwise OnFlowChannelOpen will start it when the last channel opens.
      if (!multi_flow_send_started_ &&
          !sctp_flows_.empty() &&
          CountOpenFlowChannels() == static_cast<int>(sctp_flows_.size())) {
        multi_flow_send_started_ = true;
        int total_delay_ms = 2000 + start_delay_ms_;
        fprintf(stderr,
                "[MAFS] READY received after channels open, starting send in %dms\n",
                total_delay_ms);
        std::thread([this, total_delay_ms]() {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(total_delay_ms));
          StartMultiFlowSend();
        }).detach();
      }
      return;
    } else if (json.rfind("BW_UPDATE:", 0) == 0 && !is_caller_) {
      // Sender measured actual BW after flow completion — sync receiver estimate
      double bw = std::atof(json.substr(10).c_str());
      if (bw > 0) {
        g_race_measured_bw_mbps.store(bw, std::memory_order_relaxed);
        fprintf(stderr, "[CTRL] Receiver: updated race BW estimate to %.1f Mbps\n", bw);
      }
    } else if (json == "DONE" && is_caller_) {
      fprintf(stderr, "[CTRL] Sender: received DONE — all sets complete, "
              "scheduling graceful shutdown\n");
      waiting_for_receiver_ = false;
      // Graceful shutdown on the sender side. Matches the receiver's
      // MAX_QUERIES exit path in OnReceiverQueryComplete. The
      // flow_completion.csv for the current query was already flushed
      // inside FinishFlowSend's all_done branch (pre-increment), so we
      // do NOT write it again here — doing so would append the same
      // flow rows under the next (unused) query_iteration value.
      std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        DisconnectFromServer();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        _exit(0);
      }).detach();
    }
    return;
  }

  // Demo mode JSON control protocol
  RTC_LOG(LS_INFO) << "[DEMO] Control message: " << json;

  Json::CharReaderBuilder factory;
  std::unique_ptr<Json::CharReader> reader(factory.newCharReader());
  Json::Value jmessage;
  if (!reader->parse(json.data(), json.data() + json.length(), &jmessage,
                     nullptr)) {
    RTC_LOG(LS_WARNING) << "[DEMO] Failed to parse control message";
    return;
  }

  std::string type;
  if (jmessage.isMember("type")) {
    type = jmessage["type"].asString();
  }

  // === Sender side: handle incoming query ===
  if (type == "query" && is_sender_) {
    std::string query_text = jmessage["text"].asString();
    std::string method = jmessage.isMember("method")
                             ? jmessage["method"].asString()
                             : context_method_;
    printf("[DEMO] Sender received query: \"%s\" method=%s\n",
           query_text.c_str(), method.c_str());
    fflush(stdout);

    // Step 1: Send raw text documents first (for Documents panel display)
    if (!context_texts_.empty()) {
      Json::Value texts_msg;
      texts_msg["type"] = "context_texts";
      Json::Value texts_arr(Json::arrayValue);
      for (const auto& t : context_texts_) {
        texts_arr.append(t);
      }
      texts_msg["texts"] = texts_arr;
      SendControlMessage(texts_msg);
      printf("[DEMO] Sent %zu context_texts documents via control channel\n",
             context_texts_.size());
      fflush(stdout);
    }

    // Step 2: Determine KV cache/binary data to send
    const uint8_t* data_ptr = nullptr;
    size_t data_size = 0;

    if (method == "raw_text" && !context_text_.empty()) {
      data_ptr = reinterpret_cast<const uint8_t*>(context_text_.data());
      data_size = context_text_.size();
    } else if (method == "kv_cache" && !kvcache_data_.empty()) {
      data_ptr = kvcache_data_.data();
      data_size = kvcache_data_.size();
    } else if (method == "kvzip" && !kvcache_compressed_.empty()) {
      data_ptr = kvcache_compressed_.data();
      data_size = kvcache_compressed_.size();
    }

    // Send metadata on control channel
    Json::Value meta;
    meta["type"] = "context_meta";
    meta["method"] = method;
    meta["size"] = (Json::UInt64)data_size;
    SendControlMessage(meta);

    // Send binary data in chunks on context_data channel
    if (data_size > 0 && context_data_channel_ &&
        context_data_channel_->state() ==
            webrtc::DataChannelInterface::kOpen) {
      int64_t start_ms = webrtc::TimeMillis();
      constexpr size_t kChunkSize = 256 * 1024;  // 256KB chunks
      for (size_t offset = 0; offset < data_size; offset += kChunkSize) {
        size_t chunk = std::min(kChunkSize, data_size - offset);
        webrtc::DataBuffer buf(
            webrtc::CopyOnWriteBuffer(data_ptr + offset, chunk), true);
        context_data_channel_->Send(buf);
      }
      int64_t elapsed_ms = webrtc::TimeMillis() - start_ms;

      // Send completion signal
      Json::Value done;
      done["type"] = "context_done";
      done["transfer_ms"] = (Json::Int64)elapsed_ms;
      done["query_text"] = query_text;
      SendControlMessage(done);

      printf("[DEMO] Sent %zu bytes (%s) in %lld ms\n", data_size,
             method.c_str(), (long long)elapsed_ms);
    } else {
      // No context data to send — send done with size 0
      Json::Value done;
      done["type"] = "context_done";
      done["transfer_ms"] = (Json::Int64)0;
      done["query_text"] = query_text;
      SendControlMessage(done);

      printf("[DEMO] No context data for method=%s, sent empty done\n",
             method.c_str());
    }
    fflush(stdout);
  }

  // === Receiver side: context_texts — display documents immediately ===
  if (type == "context_texts" && !is_sender_) {
    std::vector<std::string> texts;
    if (jmessage.isMember("texts") && jmessage["texts"].isArray()) {
      for (const auto& t : jmessage["texts"]) {
        texts.push_back(t.asString());
      }
    }
    printf("[DEMO] Received %zu context_texts for Documents panel\n",
           texts.size());
    fflush(stdout);
    main_wnd_->LoadContextDocuments(texts);
  }

  // === Receiver side: context_meta — prepare for incoming data ===
  if (type == "context_meta" && !is_sender_) {
    recv_context_buffer_.clear();
    context_done_received_ = false;
    transfer_start_ms_ = webrtc::TimeMillis();
    context_method_ = jmessage["method"].asString();
    recv_expected_size_ = jmessage["size"].asUInt64();
    recv_context_buffer_.reserve(recv_expected_size_);
    printf("[DEMO] Expecting %zu bytes of %s context\n", recv_expected_size_,
           context_method_.c_str());
    fflush(stdout);

    // Phase: show what's being transferred
    char phase_buf[128];
    snprintf(phase_buf, sizeof(phase_buf), "Receiving %s (%.1f MB)...",
             context_method_.c_str(), recv_expected_size_ / 1e6);
    main_wnd_->SetQueryPhase(phase_buf);
  }

  // === Receiver side: context_done — mark done, process if all data arrived ===
  if (type == "context_done" && !is_sender_) {
    pending_query_ = jmessage["query_text"].asString();
    context_done_received_ = true;
    printf("[DEMO] context_done received, have %zu / %zu bytes\n",
           recv_context_buffer_.size(), recv_expected_size_);
    fflush(stdout);

    // Try to process now (might already have all data)
    TryProcessPendingContext();
  }
}

void Conductor::TryProcessPendingContext() {
  if (!context_done_received_)
    return;
  if (recv_expected_size_ > 0 &&
      recv_context_buffer_.size() < recv_expected_size_)
    return;  // Still waiting for more data chunks

  // All data received — process
  context_done_received_ = false;
  int64_t transfer_ms = webrtc::TimeMillis() - transfer_start_ms_;
  size_t received = recv_context_buffer_.size();

  // Show transfer metrics
  char metrics[256];
  snprintf(metrics, sizeof(metrics),
           "[Transfer] %s: %.1f KB in %lld ms (%.1f MB/s)",
           context_method_.c_str(), received / 1024.0,
           (long long)transfer_ms,
           transfer_ms > 0 ? (received / 1e6) / (transfer_ms / 1e3) : 0.0);
  main_wnd_->AppendChatMessage("System", metrics);
  printf("[DEMO] %s\n", metrics);
  fflush(stdout);

  // Process based on method
  if (received > 0) {
    main_wnd_->SetQueryPhase("Loading context...");
    ProcessReceivedContext(pending_query_);
  } else {
    // No context sent — fall back to direct inference
    main_wnd_->SetQueryPhase("No context — direct inference");
    main_wnd_->AppendChatMessage("System",
                                 "No context from sender, using direct inference");
#if defined(ENABLE_LLAMA_INFERENCE)
    if (llm_adapter_ && llm_adapter_->IsInitialized()) {
      main_wnd_->AppendChatMessage("System", "Thinking...");
      std::string q = pending_query_;
      std::thread([this, q]() {
        llm_adapter_->ResetForNewQuery();
        llm_adapter_->RunDirectInference(q, 256);
      }).detach();
    }
#endif
  }
}

void Conductor::ProcessReceivedContext(const std::string& query) {
#if defined(ENABLE_LLAMA_INFERENCE)
  if (!llm_adapter_) {
    InitializeLlmAdapter();
  }
  if (!llm_adapter_ || !llm_adapter_->IsInitialized()) {
    main_wnd_->AppendChatMessage("System", "LLM not ready");
    recv_context_buffer_.clear();
    return;
  }

  main_wnd_->AppendChatMessage("System", "Thinking...");
  inference_start_ms_ = webrtc::TimeMillis();

  if (context_method_ == "raw_text") {
    main_wnd_->SetQueryPhase("Encoding context + generating...");
    // Build full prompt: context + query
    std::string context_text(recv_context_buffer_.begin(),
                             recv_context_buffer_.end());
    std::string full_prompt =
        "Context:\n" + context_text + "\n\nQuestion: " + query + "\nAnswer:";

    // Copy buffer before clearing (thread needs it)
    recv_context_buffer_.clear();

    std::thread([this, full_prompt]() {
      llm_adapter_->ResetForNewQuery();
      llm_adapter_->RunDirectInference(full_prompt, 256);
    }).detach();

  } else if (context_method_ == "kv_cache") {
    main_wnd_->SetQueryPhase("Generating response...");
    // Load KV cache state, then decode with query suffix
    std::vector<uint8_t> kv_data = std::move(recv_context_buffer_);
    recv_context_buffer_.clear();

    std::string suffix = "\nQuestion: " + query + "\nAnswer:";
    std::thread([this, kv_data = std::move(kv_data), suffix]() {
      llm_adapter_->RunFromKVCache(kv_data, suffix, 256);
    }).detach();

  } else if (context_method_ == "kvzip") {
    // Decompress zstd-compressed KV cache, then load
    std::vector<uint8_t> compressed = std::move(recv_context_buffer_);
    recv_context_buffer_.clear();

    std::string suffix = "\nQuestion: " + query + "\nAnswer:";
    std::thread([this, compressed = std::move(compressed), suffix]() {
      // Decompress
      unsigned long long decompressed_size =
          ZSTD_getFrameContentSize(compressed.data(), compressed.size());
      if (decompressed_size == ZSTD_CONTENTSIZE_ERROR ||
          decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        printf("[DEMO] ERROR: Invalid zstd compressed data\n");
        fflush(stdout);
        main_wnd_->AppendChatMessage("System", "Error: Invalid compressed data");
        return;
      }

      std::vector<uint8_t> kv_data(decompressed_size);
      size_t actual = ZSTD_decompress(
          kv_data.data(), kv_data.size(),
          compressed.data(), compressed.size());
      if (ZSTD_isError(actual)) {
        printf("[DEMO] ERROR: zstd decompression failed: %s\n",
               ZSTD_getErrorName(actual));
        fflush(stdout);
        main_wnd_->AppendChatMessage("System",
            std::string("Decompress error: ") + ZSTD_getErrorName(actual));
        return;
      }

      printf("[DEMO] Decompressed: %zu -> %zu bytes\n",
             compressed.size(), actual);
      fflush(stdout);

      kv_data.resize(actual);
      llm_adapter_->RunFromKVCache(kv_data, suffix, 256);
    }).detach();

  } else {
    main_wnd_->AppendChatMessage("System",
                                 "Unknown context method: " + context_method_);
    recv_context_buffer_.clear();
  }
#else
  main_wnd_->AppendChatMessage("System", "LLM inference not compiled in");
  recv_context_buffer_.clear();
#endif
}

void Conductor::OnQuerySubmitted(const std::string& query) {
  if (!demo_mode_)
    return;

  query_start_ms_ = webrtc::TimeMillis();
  printf("[DEMO] Query submitted: %s\n", query.c_str());
  fflush(stdout);

  // If control channel is open, send query to sender and wait for context
  if (control_channel_ &&
      control_channel_->state() == webrtc::DataChannelInterface::kOpen) {
    Json::Value jmessage;
    jmessage["type"] = "query";
    jmessage["text"] = query;
    jmessage["method"] = context_method_.empty() ? "raw_text" : context_method_;
    SendControlMessage(jmessage);
    RTC_LOG(LS_INFO) << "[DEMO] Sent query to sender via control channel";
    // Don't run local inference — wait for context_done from sender
    return;
  }

#if defined(ENABLE_LLAMA_INFERENCE)
  // No sender connected: local-only inference
  if (!llm_adapter_ && !model_path_.empty()) {
    InitializeLlmAdapter();
  }

  if (llm_adapter_ && llm_adapter_->IsInitialized()) {
    main_wnd_->AppendChatMessage("System", "Thinking...");
    inference_start_ms_ = webrtc::TimeMillis();

    // Run inference in background thread to not block UI
    std::string query_copy = query;
    std::thread([this, query_copy]() {
      llm_adapter_->ResetForNewQuery();
      std::string result =
          llm_adapter_->RunDirectInference(query_copy, 256);

      // Final message already streamed via callback,
      // but show if no streaming callback was set
      if (result.empty()) {
        main_wnd_->AppendChatMessage("System", "(No response generated)");
      }
    }).detach();
  } else {
    main_wnd_->AppendChatMessage("System",
                                 "LLM not available. Provide --model_path.");
  }
#else
  main_wnd_->AppendChatMessage("System",
                               "LLM not compiled. Build with "
                               "enable_llama_inference=true.");
#endif
}

#if defined(ENABLE_LLAMA_INFERENCE)
void Conductor::InitializeLlmAdapter() {
  if (model_path_.empty()) {
    printf("[DEMO] No model_path specified, LLM disabled\n");
    fflush(stdout);
    return;
  }

  llm_adapter_ = std::make_unique<LlmReceiverAdapter>();

  // Set streaming callback to push tokens to chat UI + timing logs
  llm_adapter_->SetStreamingCallback(
      [this](int ctx_id, const std::string& token, int idx, bool is_final) {
        if (idx == 0) {
          // First token: log TTFT
          int64_t now = webrtc::TimeMillis();
          int64_t ttft_ms = now - inference_start_ms_;
          int64_t e2e_ttft_ms = now - query_start_ms_;
          printf("[TIMING] TTFT: %lld ms (inference), %lld ms (E2E from query)\n",
                 (long long)ttft_ms, (long long)e2e_ttft_ms);
          fflush(stdout);
          char phase_buf[128];
          snprintf(phase_buf, sizeof(phase_buf),
                   "Generating... (TTFT: %.1fs)", e2e_ttft_ms / 1000.0);
          main_wnd_->SetQueryPhase(phase_buf);
          main_wnd_->AppendChatMessage("Assistant", token);
        } else if (is_final) {
          // Final token: log total response time with tok/s
          int64_t now = webrtc::TimeMillis();
          int64_t total_ms = now - query_start_ms_;
          int64_t inference_ms = now - inference_start_ms_;
          int64_t ttft_ms = now - inference_start_ms_;  // approximation for decode-only time
          int n_tokens = idx + 1;

          // Calculate decode tok/s (exclude first token latency for pure decode rate)
          // TTFT was logged at idx==0, so decode time = inference_ms
          double decode_tok_s = (inference_ms > 0)
              ? (n_tokens * 1000.0 / inference_ms)
              : 0.0;

          printf("[TIMING] Total: %lld ms (E2E), %lld ms (inference), %d tokens, %.1f tok/s\n",
                 (long long)total_ms, (long long)inference_ms, n_tokens, decode_tok_s);
          fflush(stdout);

          char timing_msg[512];
          snprintf(timing_msg, sizeof(timing_msg),
                   "[Timing] E2E: %lld ms | Inference: %lld ms | Tokens: %d | %.1f tok/s",
                   (long long)total_ms, (long long)inference_ms, n_tokens, decode_tok_s);
          main_wnd_->AppendChatMessage("System", timing_msg);
          main_wnd_->AppendChatMessage("", "\n");

          char done_buf[128];
          snprintf(done_buf, sizeof(done_buf),
                   "Complete (%d tok, %.1f tok/s)", n_tokens, decode_tok_s);
          main_wnd_->SetQueryPhase(done_buf);
        } else {
          main_wnd_->AppendChatMessage("", token);
        }
      });

  if (!llm_adapter_->Init(model_path_)) {
    printf("[DEMO] ERROR: Failed to load model: %s\n", model_path_.c_str());
    fflush(stdout);
    main_wnd_->AppendChatMessage("System",
                                 "Failed to load model: " + model_path_);
    llm_adapter_.reset();
    return;
  }

  printf("[DEMO] LLM adapter initialized with model: %s\n",
         model_path_.c_str());
  fflush(stdout);
  main_wnd_->AppendChatMessage("System", "LLM ready. Type a query below.");
}
#endif

// ============================================================================
// WebSocket Signaling Implementation
// ============================================================================

void Conductor::ServiceWebSocket() {
  if (ws_client_) {
    ws_client_->Service();
  }
  // Push latest throughput from RTCStats to UI for performance graph
  if (stats_collector_ && stats_collector_->IsRunning() && main_wnd_) {
    main_wnd_->UpdateThroughput(stats_collector_->GetLatestVideoMbps(),
                                stats_collector_->GetLatestSctpMbps());
  }
}

void Conductor::StartWebSocketSignaling() {
  if (room_id_.empty()) {
    RTC_LOG(LS_ERROR) << "Room ID not set, cannot start WebSocket signaling";
    return;
  }

  // Generate client ID
  std::srand(std::time(nullptr));
  client_id_ = std::to_string(std::rand());

  // First, call the room join API
  ConnectToRoom();
}

void Conductor::ConnectToRoom() {
  // Build the join URL
  std::string join_url = "https://" + signaling_server_ + "/join/" + room_id_;
  RTC_LOG(LS_INFO) << "Joining room: " << join_url;
  printf("[WS] Joining room: %s\n", join_url.c_str()); fflush(stdout);

  // Use curl to join the room
  CURL* curl = curl_easy_init();
  if (!curl) {
    RTC_LOG(LS_ERROR) << "Failed to init curl";
    return;
  }

  // Build JSON payload
  Json::Value join_payload;
  join_payload["room_id"] = room_id_;
  Json::StreamWriterBuilder writer;
  std::string payload = Json::writeString(writer, join_payload);
  printf("[WS] POST payload: %s\n", payload.c_str()); fflush(stdout);

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, join_url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, payload.length());
  
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "User-Agent: peerconnection-client/1.0");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, 
    +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
      ((std::string*)userdata)->append(ptr, size * nmemb);
      return size * nmemb;
    });
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  
  CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    RTC_LOG(LS_ERROR) << "Failed to join room: " << curl_easy_strerror(res);
    printf("[WS] ERROR: Failed to join room: %s\n", curl_easy_strerror(res)); fflush(stdout);
    return;
  }

  RTC_LOG(LS_INFO) << "Room join response: " << response;
  printf("[WS] Room join response: %s\n", response.c_str()); fflush(stdout);

  // Parse response
  Json::CharReaderBuilder reader;
  Json::Value root;
  std::string parse_errors;
  std::istringstream response_stream(response);

  if (!Json::parseFromStream(reader, response_stream, &root, &parse_errors)) {
    RTC_LOG(LS_ERROR) << "Failed to parse room response: " << parse_errors;
    printf("[WS] ERROR: Parse error: %s\n", parse_errors.c_str()); fflush(stdout);
    return;
  }

  // Check result
  if (root["result"].asString() != "SUCCESS") {
    printf("[WS] ERROR: Join failed: %s\n", root["result"].asString().c_str()); fflush(stdout);
    return;
  }

  // Extract params
  Json::Value params = root["params"];
  
  // Get client_id from response
  if (params.isMember("client_id")) {
    client_id_ = params["client_id"].asString();
  }
  
  // Get room_id from response (may be different from what we sent)
  if (params.isMember("room_id")) {
    room_id_ = params["room_id"].asString();
  }
  
  // Check if we are initiator
  if (params.isMember("is_initiator")) {
    std::string is_init_str = params["is_initiator"].asString();
    is_initiator_ = (is_init_str == "true");
    is_caller_ = is_initiator_;  // Sync with datachannel test
  }
  
  printf("[WS] client_id: %s, room_id: %s, is_initiator: %s\n", 
         client_id_.c_str(), room_id_.c_str(), is_initiator_ ? "true" : "false");
  fflush(stdout);

  RTC_LOG(LS_INFO) << "is_initiator: " << is_initiator_;

  // Store initial messages if any (messages from other peer before we connected)
  if (params.isMember("messages") && params["messages"].isArray()) {
    initial_messages_ = params["messages"];
    printf("[WS] Found %u initial messages\n", initial_messages_.size());
    fflush(stdout);
  }

  // Get WebSocket URL from response
  std::string wss_url;
  if (params.isMember("wss_url")) {
    wss_url = params["wss_url"].asString();
  } else {
    wss_url = "wss://" + signaling_server_ + ":8089/ws";
  }
  printf("[WS] Connecting to: %s\n", wss_url.c_str()); fflush(stdout);

  // Connect to WebSocket
  ws_client_ = std::make_unique<WebSocketClient>();
  ws_client_->SetMessageCallback(
      [this](const std::string& msg) { OnWebSocketMessage(msg); });
  ws_client_->SetConnectionCallback(
      [this](bool connected) { OnWebSocketConnection(connected); });

  RTC_LOG(LS_INFO) << "Connecting to WebSocket: " << wss_url;
  ws_client_->Connect(wss_url);
}

void Conductor::OnWebSocketConnection(bool connected) {
  if (connected) {
    RTC_LOG(LS_INFO) << "WebSocket connected, registering...";
    printf("[WS] WebSocket connected!\n"); fflush(stdout);

    // Send registration message
    Json::Value reg_message;
    reg_message["cmd"] = "register";
    reg_message["roomid"] = room_id_;
    reg_message["clientid"] = client_id_;

    Json::StreamWriterBuilder writer;
    std::string message = Json::writeString(writer, reg_message);
    ws_client_->SendMessage(message);

    // Process any initial messages (offer from initiator if we're non-initiator)
    if (!initial_messages_.empty() && initial_messages_.isArray()) {
      printf("[WS] Processing %u initial messages\n", initial_messages_.size());
      fflush(stdout);
      for (const auto& msg : initial_messages_) {
        OnMessageFromPeer(-1, msg.asString());
      }
      initial_messages_.clear();
    }

    // If we're initiator, start the peer connection
    if (is_initiator_) {
      RTC_LOG(LS_INFO) << "We are initiator, starting peer connection";
      printf("[WS] Initiator: Setting up peer connection\n");
      fflush(stdout);
      if (InitializePeerConnection()) {
        // MAFS multi-flow or single DataChannel
        if ((rtp_sctp_mode_ || datachannel_test_mode_) &&
            !queries_csv_path_.empty() && ParseTrafficConfig()) {
          CreateMultiFlowDataChannels();
        } else if (datachannel_test_mode_ || rtp_sctp_mode_) {
          CreateDataChannel();
        }

        // Create control channel for demo mode
        if (demo_mode_) {
          CreateControlChannel();
        }

        // Add RTP tracks for RTP+SCTP, RTP-only, or demo mode with Y4M
        if (rtp_sctp_mode_ || rtp_only_mode_ ||
            (demo_mode_ && !y4m_path_.empty())) {
          printf("[%s] Adding audio/video tracks...\n",
                 rtp_only_mode_ ? "RTP-ONLY" :
                 demo_mode_ ? "DEMO" : "RTP+SCTP");
          fflush(stdout);
          AddTracks();
        }

        peer_connection_->CreateOffer(
            this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
      }
    }
  } else {
    RTC_LOG(LS_WARNING) << "WebSocket disconnected";
    printf("[WS] WebSocket disconnected!\n"); fflush(stdout);
  }
}

void Conductor::OnWebSocketMessage(const std::string& message) {
  printf("[WS] Message received: %s\n", message.substr(0, 100).c_str()); fflush(stdout); RTC_LOG(LS_INFO) << "WebSocket message received: " << message;

  Json::CharReaderBuilder reader;
  Json::Value json_message;
  std::string parse_errors;
  std::istringstream message_stream(message);

  if (!Json::parseFromStream(reader, message_stream, &json_message, &parse_errors)) {
    RTC_LOG(LS_WARNING) << "Failed to parse WebSocket message: " << parse_errors;
    return;
  }

  std::string msg_data;
  if (json_message.isMember("msg")) {
    msg_data = json_message["msg"].asString();
  } else {
    msg_data = message;
  }

  // Process as peer message
  OnMessageFromPeer(-1, msg_data);
}

// ============================================================================
// MAFS Multi-Flow Implementation
// ============================================================================

bool Conductor::ParseTrafficConfig() {
  if (queries_csv_path_.empty()) return false;

  // Read hafs_sequence.csv — parse ALL lines into sequence_entries_
  std::ifstream seq_file(queries_csv_path_);
  if (!seq_file.is_open()) {
    fprintf(stderr, "[MAFS] Cannot open queries_csv: %s\n",
            queries_csv_path_.c_str());
    return false;
  }

  sequence_entries_.clear();
  std::string line;
  while (std::getline(seq_file, line)) {
    // Skip comments and header
    if (line.empty() || line[0] == '#' || line.find("sequence_id") == 0)
      continue;
    // CSV: sequence_id,query_path,start_delay_ms,repeat_count,query_complete_delay_ms
    std::istringstream iss(line);
    std::string field;
    SequenceEntry entry;
    std::getline(iss, field, ',');  // sequence_id
    if (!field.empty()) entry.sequence_id = std::stoi(field);
    std::getline(iss, entry.query_path, ',');  // query_path
    if (std::getline(iss, field, ',') && !field.empty())
      start_delay_ms_ = std::stoi(field);  // use first line's start_delay
    if (std::getline(iss, field, ',') && !field.empty())
      repeat_count_ = std::stoi(field);    // backward compat
    if (std::getline(iss, field, ',') && !field.empty())
      entry.query_complete_delay_ms = std::stoi(field);

    if (!entry.query_path.empty()) {
      fprintf(stderr, "[MAFS] Sequence entry %d: path=%s, delay=%dms\n",
              entry.sequence_id, entry.query_path.c_str(),
              entry.query_complete_delay_ms);
      sequence_entries_.push_back(std::move(entry));
    }
  }
  seq_file.close();

  if (sequence_entries_.empty()) {
    fprintf(stderr, "[MAFS] No entries found in %s\n", queries_csv_path_.c_str());
    return false;
  }

  fprintf(stderr, "[MAFS] Parsed %zu sequence entries (start_delay=%dms)\n",
          sequence_entries_.size(), start_delay_ms_);

  // MAX_QUERIES env: graceful exit after N query completions.
  // Forces receiver-driven mode even with a single sequence entry so that
  // a CSV with one row + MAX_QUERIES=N runs N compute-aware iterations.
  if (const char* mq_env = std::getenv("MAX_QUERIES")) {
    max_queries_ = std::atoi(mq_env);
    if (max_queries_ > 0) {
      fprintf(stderr, "[MAFS] MAX_QUERIES=%d — receiver-driven exit after %d "
              "completed queries\n", max_queries_, max_queries_);
    }
  }

  // Load first set
  current_set_index_ = 0;
  return LoadSetByIndex(0);
}

bool Conductor::LoadSetByIndex(int set_index) {
  if (set_index < 0 || set_index >= static_cast<int>(sequence_entries_.size())) {
    fprintf(stderr, "[MAFS] LoadSetByIndex: invalid index %d (have %zu entries)\n",
            set_index, sequence_entries_.size());
    return false;
  }

  const auto& entry = sequence_entries_[set_index];

  // Set query_complete_delay_ms_ from entry (env QUERY_INTERVAL_MS overrides)
  query_complete_delay_ms_ = entry.query_complete_delay_ms;
  const char* interval_env = std::getenv("QUERY_INTERVAL_MS");
  if (interval_env) query_complete_delay_ms_ = std::atoi(interval_env);

  // Derive base directory from queries_csv path
  std::string base_dir = queries_csv_path_;
  auto last_slash = base_dir.rfind('/');
  if (last_slash != std::string::npos) {
    base_dir = base_dir.substr(0, last_slash);
  }

  // Load metadata.json from hafs_query/<query_path>/
  std::string metadata_path = base_dir + "/hafs_query/" + entry.query_path + "/metadata.json";
  std::ifstream meta_file(metadata_path);
  if (!meta_file.is_open()) {
    fprintf(stderr, "[MAFS] Cannot open metadata: %s\n", metadata_path.c_str());
    return false;
  }

  Json::CharReaderBuilder reader;
  Json::Value root;
  std::string errors;
  if (!Json::parseFromStream(reader, meta_file, &root, &errors)) {
    fprintf(stderr, "[MAFS] JSON parse error: %s\n", errors.c_str());
    return false;
  }
  meta_file.close();

  if (!root.isMember("flows") || !root["flows"].isArray()) {
    fprintf(stderr, "[MAFS] No flows array in metadata.json\n");
    return false;
  }

  std::string rate_sched_dir = base_dir + "/hafs_query/" + entry.query_path + "/";
  const Json::Value& flows = root["flows"];

  // Validate flow count consistency for subsequent sets (including wrap-around)
  if (!sctp_flows_.empty() && flows.size() != sctp_flows_.size()) {
    fprintf(stderr, "[MAFS] ERROR: set %d has %u flows but expected %zu. Skipping.\n",
            set_index, flows.size(), sctp_flows_.size());
    return false;
  }

  if (!sctp_flows_.empty()) {
    // Subsequent set: reset existing flows and update metadata only
    ResetFlowsForNewSet();

    for (unsigned i = 0; i < flows.size(); i++) {
      const Json::Value& f = flows[i];
      auto& flow = sctp_flows_[i];
      flow->flow_id = f.get("flow_id", static_cast<int>(i)).asInt();
      flow->label = f.get("flow_label", "flow_" + std::to_string(i)).asString();
      flow->total_bytes = static_cast<size_t>(f.get("total_bytes", 0).asUInt64());
      flow->P_hat_ms = f.get("P_hat_ms", 0.0).asDouble();
      flow->sigma_P_ms = f.get("sigma_P_ms", 0.0).asDouble();
      flow->parallel_compute_ms = f.get("parallel_compute_ms", 0.0).asDouble();
      if (f.isMember("rate_schedule")) {
        flow->rate_schedule_path = rate_sched_dir + f["rate_schedule"].asString();
      }
      flow->context_tokens = static_cast<size_t>(f.get("context_tokens", 0).asUInt64());
      flow->decode_tokens = static_cast<size_t>(f.get("decode_tokens", 0).asUInt64());
      if (f.isMember("original_decode_tokens"))
        flow->actual_decode_tokens = static_cast<size_t>(f.get("original_decode_tokens", 0).asUInt64());
      else if (f.isMember("_original_decode_tokens"))
        flow->actual_decode_tokens = static_cast<size_t>(f.get("_original_decode_tokens", 0).asUInt64());
      else
        flow->actual_decode_tokens = flow->decode_tokens;

      flow->data_format = f.get("data_format", "").asString();
      flow->file_path = f.get("file_path", "").asString();

      // COMPUTE_DECODE_BASE env var: override P_hat from device-specific
      // decode rate (ms/token). Johnson scheduling uses P_hat to determine
      // flow priority — must match the actual receiver device's decode speed.
      // E.g., Jetson=36.67, MacBook=16.21, S25=54.05 (from emulation_models.csv)
      {
        const char* decode_base_env = getenv("COMPUTE_DECODE_BASE");
        if (decode_base_env && strlen(decode_base_env) > 0 && flow->decode_tokens > 0) {
          double decode_base = std::atof(decode_base_env);
          double new_P_hat = flow->decode_tokens * decode_base;
          fprintf(stderr, "[MAFS] P_hat override: flow %d (%s) → "
                  "P_hat %.0f→%.0fms (decode_tokens=%zu × %.2f ms/tok)\n",
                  i, flow->label.c_str(),
                  flow->P_hat_ms, new_P_hat, flow->decode_tokens, decode_base);
          flow->P_hat_ms = new_P_hat;
        }
      }

      // PARALLEL_COMPUTE_MS env var override
      const char* pcm_env = getenv("PARALLEL_COMPUTE_MS");
      if (pcm_env && strlen(pcm_env) > 0)
        flow->parallel_compute_ms = std::atof(pcm_env);

      // DATA_FORMAT env var override
      const char* data_format_env = getenv("DATA_FORMAT");
      if (data_format_env && strlen(data_format_env) > 0) {
        std::string format_override(data_format_env);
        if (format_override == "raw_text") {
          flow->data_format = "raw_text";
          char ctx_filename[32];
          snprintf(ctx_filename, sizeof(ctx_filename), "context/%02d.txt", i);
          flow->file_path = std::string(ctx_filename);
          if (f.isMember("text_bytes"))
            flow->total_bytes = static_cast<size_t>(f.get("text_bytes", 0).asUInt64());
        } else if (format_override == "kv_cache") {
          flow->data_format = "kv_cache";
        } else if (format_override == "kvzip") {
          flow->data_format = "kvzip";
          char ctx_filename[32];
          snprintf(ctx_filename, sizeof(ctx_filename), "kvcache_compressed/%02d.bin", i);
          flow->file_path = std::string(ctx_filename);
          if (f.isMember("kvzip_bytes"))
            flow->total_bytes = static_cast<size_t>(f.get("kvzip_bytes", 0).asUInt64());
          else if (f.isMember("kv_bytes"))
            flow->total_bytes = static_cast<size_t>(f.get("kv_bytes", 0).asUInt64() * 3 / 10);
        }
      }

      // Overlapped prefill: always active for HAFS, never for NC/FSE.
      bool race_mode = IsHafsMode();
      if (race_mode && flow->data_format == "kvzip" && flow->context_tokens > 0) {
        // Race-based overlap: compute f* from race simulation using expected bandwidth
        // Estimate avg bandwidth from previous flow or use default
        int chunk_size = 128;
        const char* cs_env = getenv("RACE_CHUNK_SIZE");
        if (cs_env) chunk_size = std::atoi(cs_env);

        const char* pa = getenv("COMPUTE_PREFILL_A");
        double prefill_a = pa ? std::atof(pa) : 2.336;
        double chunk_prefill_ms = prefill_a * chunk_size;

        // Sender compute: 0.075*ctx + 15ms
        double sender_compute_ms = 0.075 * flow->context_tokens + 15.0;

        // Estimate bandwidth: env var > previously measured > 200 Mbps default
        double measured_bw = g_race_measured_bw_mbps.load(std::memory_order_relaxed);
        double est_bw_mbps = (measured_bw > 0) ? measured_bw : 200.0;
        const char* bw_env = getenv("RACE_EST_BW_MBPS");
        if (bw_env) est_bw_mbps = std::atof(bw_env);

        int n_chunks = (static_cast<int>(flow->context_tokens) + chunk_size - 1) / chunk_size;
        double per_chunk_bytes = n_chunks > 0 ? static_cast<double>(flow->total_bytes) / n_chunks : 0;
        double chunk_xfer_ms = (est_bw_mbps > 0 && per_chunk_bytes > 0)
            ? (per_chunk_bytes * 8.0 / (est_bw_mbps * 1e6) * 1000.0) : 99999.0;

        // Race simulation: find meeting point
        struct RaceEv { bool local; int idx; double t; };
        std::vector<RaceEv> events;
        events.reserve(2 * n_chunks);
        for (int i = 0; i < n_chunks; i++)
          events.push_back({true, i, (i + 1) * chunk_prefill_ms});
        for (int j = 0; j < n_chunks; j++) {
          int ci = n_chunks - 1 - j;
          events.push_back({false, ci, sender_compute_ms + (j + 1) * chunk_xfer_ms});
        }
        std::sort(events.begin(), events.end(),
                  [](const RaceEv& a, const RaceEv& b) { return a.t < b.t; });

        int local_done = 0, remote_min = n_chunks;
        for (const auto& e : events) {
          if (e.local) local_done = e.idx + 1;
          else remote_min = std::min(remote_min, e.idx);
          if (local_done >= remote_min) break;
        }
        double f_star = static_cast<double>(local_done) / n_chunks;
        // Cap f* so sender always transmits at least 1 chunk (1/n_chunks).
        // f*=1.0 → total_bytes=0 would cause receiver to never fire OnFlowReceived.
        if (n_chunks > 0 && local_done >= n_chunks)
          f_star = static_cast<double>(n_chunks - 1) / n_chunks;

        // Apply: reduce transfer bytes, set parallel compute
        size_t original_bytes = flow->total_bytes;
        flow->total_bytes = static_cast<size_t>(flow->total_bytes * (1.0 - f_star));
        if (flow->total_bytes == 0 && original_bytes > 0)
          flow->total_bytes = 1;  // ensure at least 1 byte so receiver fires
        size_t front_tokens = static_cast<size_t>(flow->context_tokens * f_star);
        flow->parallel_compute_ms = prefill_a * front_tokens;
        flow->front_context_tokens = front_tokens;

        fprintf(stderr, "[MAFS] Race overlap (pre): flow %d, f*=%.2f (%d/%d), "
                "est_bw=%.0f Mbps, bytes %zu->%zu, front=%zu tok, "
                "parallel_compute=%.0fms, chunk_prefill=%.0fms, chunk_xfer=%.0fms\n",
                flow->flow_id, f_star, local_done, n_chunks,
                est_bw_mbps, original_bytes, flow->total_bytes, front_tokens,
                flow->parallel_compute_ms, chunk_prefill_ms, chunk_xfer_ms);
      } else if (!race_mode) {
        const char* olp_env = getenv("OVERLAPPED_PREFILL_RATIO");
        if (olp_env && strlen(olp_env) > 0 && flow->data_format == "kvzip") {
          double front_ratio = std::atof(olp_env);
          // Low-BW activation: only apply when estimated BW < threshold.
          // At high BW, transfer finishes before front prefill completes (no benefit).
          // Default threshold: 100 Mbps. Override with OVERLAPPED_PREFILL_BW_THRESH_MBPS.
          double bw_thresh_mbps = 100.0;
          const char* bw_th_env = getenv("OVERLAPPED_PREFILL_BW_THRESH_MBPS");
          if (bw_th_env) bw_thresh_mbps = std::atof(bw_th_env);
          const char* est_bw_env = getenv("RACE_EST_BW_MBPS");
          double est_bw_mbps = est_bw_env ? std::atof(est_bw_env) : 999.0;
          bool low_bw = (est_bw_mbps <= bw_thresh_mbps);
          if (front_ratio > 0.0 && front_ratio < 1.0 && flow->context_tokens > 0 && low_bw) {
            size_t original_bytes = flow->total_bytes;
            flow->total_bytes = static_cast<size_t>(flow->total_bytes * (1.0 - front_ratio));
            size_t front_tokens = static_cast<size_t>(flow->context_tokens * front_ratio);
            flow->front_context_tokens = front_tokens;
            const char* pa = getenv("COMPUTE_PREFILL_A");
            const char* pb = getenv("COMPUTE_PREFILL_B");
            if (pa && pb) {
              double a = std::atof(pa), b = std::atof(pb);
              double ctx = static_cast<double>(front_tokens);
              flow->parallel_compute_ms = a * ctx + b * ctx * ctx;
            }
            // Load front text for real GPU backends (context/*.txt)
            if (!flow->file_path.empty()) {
              std::string txt_path = flow->file_path;
              auto pos = txt_path.rfind('/');
              if (pos != std::string::npos) {
                std::string dir = txt_path.substr(0, pos);
                auto dpos = dir.rfind('/');
                std::string base = (dpos != std::string::npos) ? dir.substr(dpos+1) : dir;
                // kvcache_compressed/00.bin → context/00.txt
                std::string fname = txt_path.substr(txt_path.rfind('/')+1);
                fname = fname.substr(0, fname.rfind('.')) + ".txt";
                std::string ctx_txt = dir.substr(0, dpos != std::string::npos ? dpos+1 : 0)
                                    + "context/" + fname;
                struct stat st2;
                if (stat((rate_sched_dir + ctx_txt).c_str(), &st2) == 0) {
                  std::ifstream tf(rate_sched_dir + ctx_txt);
                  if (tf) {
                    flow->front_text_content = std::string(
                        (std::istreambuf_iterator<char>(tf)),
                        std::istreambuf_iterator<char>());
                    // Truncate to approximate front_ratio characters
                    size_t max_chars = static_cast<size_t>(
                        flow->front_text_content.size() * front_ratio);
                    if (max_chars < flow->front_text_content.size())
                      flow->front_text_content.resize(max_chars);
                  }
                }
              }
            }
            fprintf(stderr, "[MAFS] Overlapped prefill: flow %d, ratio=%.2f, "
                    "est_bw=%.0f Mbps (thresh=%.0f), "
                    "bytes %zu->%zu, front_tokens=%zu, parallel_compute=%.0fms, "
                    "front_text=%zuB\n",
                    flow->flow_id, front_ratio, est_bw_mbps, bw_thresh_mbps,
                    original_bytes, flow->total_bytes,
                    front_tokens, flow->parallel_compute_ms,
                    flow->front_text_content.size());
          } else if (!low_bw && front_ratio > 0.0) {
            fprintf(stderr, "[MAFS] Overlapped prefill SKIPPED (high BW): flow %d, "
                    "est_bw=%.0f Mbps > thresh=%.0f Mbps\n",
                    flow->flow_id, est_bw_mbps, bw_thresh_mbps);
          }
        }
      }

      if (!flow->file_path.empty()) {
        flow->file_path = rate_sched_dir + flow->file_path;
        struct stat st;
        const char* data_format_env2 = getenv("DATA_FORMAT");
        if (stat(flow->file_path.c_str(), &st) == 0) {
          if (flow->total_bytes == 0)
            flow->total_bytes = static_cast<size_t>(st.st_size);
          if (data_format_env2 && std::string(data_format_env2) == "raw_text")
            flow->total_bytes = static_cast<size_t>(st.st_size);
        }
      }

      fprintf(stderr, "[MAFS] Set %d Flow %d: label=%s, bytes=%zu, P_hat=%.0fms"
              ", parallel_compute=%.0fms, data_format=%s, file_path=%s\n",
              set_index, flow->flow_id, flow->label.c_str(), flow->total_bytes,
              flow->P_hat_ms, flow->parallel_compute_ms,
              flow->data_format.c_str(), flow->file_path.c_str());
    }
  } else {
    // First set (set_index == 0): create sctp_flows_ from scratch
    for (unsigned i = 0; i < flows.size(); i++) {
      const Json::Value& f = flows[i];
      auto flow = std::make_unique<SctpFlow>();
      flow->flow_id = f.get("flow_id", static_cast<int>(i)).asInt();
      flow->label = f.get("flow_label", "flow_" + std::to_string(i)).asString();
      flow->total_bytes = static_cast<size_t>(f.get("total_bytes", 0).asUInt64());
      flow->P_hat_ms = f.get("P_hat_ms", 0.0).asDouble();
      flow->sigma_P_ms = f.get("sigma_P_ms", 0.0).asDouble();
      flow->parallel_compute_ms = f.get("parallel_compute_ms", 0.0).asDouble();
      if (f.isMember("rate_schedule")) {
        flow->rate_schedule_path = rate_sched_dir + f["rate_schedule"].asString();
      }

      flow->context_tokens = static_cast<size_t>(f.get("context_tokens", 0).asUInt64());
      flow->decode_tokens = static_cast<size_t>(f.get("decode_tokens", 0).asUInt64());
      if (f.isMember("original_decode_tokens"))
        flow->actual_decode_tokens = static_cast<size_t>(f.get("original_decode_tokens", 0).asUInt64());
      else if (f.isMember("_original_decode_tokens"))
        flow->actual_decode_tokens = static_cast<size_t>(f.get("_original_decode_tokens", 0).asUInt64());
      else
        flow->actual_decode_tokens = flow->decode_tokens;

      flow->data_format = f.get("data_format", "").asString();
      flow->file_path = f.get("file_path", "").asString();

      // COMPUTE_DECODE_BASE env var: override P_hat from device-specific
      // decode rate (ms/token). Johnson scheduling uses P_hat to determine
      // flow priority — must match the actual receiver device's decode speed.
      // E.g., Jetson=36.67, MacBook=16.21, S25=54.05 (from emulation_models.csv)
      {
        const char* decode_base_env = getenv("COMPUTE_DECODE_BASE");
        if (decode_base_env && strlen(decode_base_env) > 0 && flow->decode_tokens > 0) {
          double decode_base = std::atof(decode_base_env);
          double new_P_hat = flow->decode_tokens * decode_base;
          fprintf(stderr, "[MAFS] P_hat override: flow %d (%s) → "
                  "P_hat %.0f→%.0fms (decode_tokens=%zu × %.2f ms/tok)\n",
                  i, flow->label.c_str(),
                  flow->P_hat_ms, new_P_hat, flow->decode_tokens, decode_base);
          flow->P_hat_ms = new_P_hat;
        }
      }

      // PARALLEL_COMPUTE_MS env var override
      const char* pcm_env = getenv("PARALLEL_COMPUTE_MS");
      if (pcm_env && strlen(pcm_env) > 0)
        flow->parallel_compute_ms = std::atof(pcm_env);

      const char* data_format_env = getenv("DATA_FORMAT");
      if (data_format_env && strlen(data_format_env) > 0) {
        std::string format_override(data_format_env);
        if (format_override == "raw_text") {
          flow->data_format = "raw_text";
          char ctx_filename[32];
          snprintf(ctx_filename, sizeof(ctx_filename), "context/%02d.txt", i);
          flow->file_path = std::string(ctx_filename);
          if (f.isMember("text_bytes"))
            flow->total_bytes = static_cast<size_t>(f.get("text_bytes", 0).asUInt64());
          fprintf(stderr, "[MAFS] DATA_FORMAT override: flow %d -> raw_text, file=%s\n",
                  flow->flow_id, flow->file_path.c_str());
        } else if (format_override == "kv_cache") {
          flow->data_format = "kv_cache";
        } else if (format_override == "kvzip") {
          flow->data_format = "kvzip";
          char ctx_filename[32];
          snprintf(ctx_filename, sizeof(ctx_filename), "kvcache_compressed/%02d.bin", i);
          flow->file_path = std::string(ctx_filename);
          if (f.isMember("kvzip_bytes"))
            flow->total_bytes = static_cast<size_t>(f.get("kvzip_bytes", 0).asUInt64());
          else if (f.isMember("kv_bytes"))
            flow->total_bytes = static_cast<size_t>(f.get("kv_bytes", 0).asUInt64() * 3 / 10);
          fprintf(stderr, "[MAFS] DATA_FORMAT override: flow %d -> kvzip, file=%s, bytes=%zu\n",
                  flow->flow_id, flow->file_path.c_str(), flow->total_bytes);
        }
      }

      // Overlapped prefill: always active for HAFS, never for NC/FSE.
      bool race_mode2 = IsHafsMode();
      if (race_mode2 && flow->data_format == "kvzip" && flow->context_tokens > 0) {
        // Race-based overlap: same logic as first LoadSetByIndex block
        int chunk_size = 128;
        const char* cs_env = getenv("RACE_CHUNK_SIZE");
        if (cs_env) chunk_size = std::atoi(cs_env);

        const char* pa = getenv("COMPUTE_PREFILL_A");
        double prefill_a = pa ? std::atof(pa) : 2.336;
        double chunk_prefill_ms = prefill_a * chunk_size;
        double sender_compute_ms = 0.075 * flow->context_tokens + 15.0;

        double measured_bw2 = g_race_measured_bw_mbps.load(std::memory_order_relaxed);
        double est_bw_mbps = (measured_bw2 > 0) ? measured_bw2 : 200.0;
        const char* bw_env = getenv("RACE_EST_BW_MBPS");
        if (bw_env) est_bw_mbps = std::atof(bw_env);

        int n_chunks = (static_cast<int>(flow->context_tokens) + chunk_size - 1) / chunk_size;
        double per_chunk_bytes = n_chunks > 0 ? static_cast<double>(flow->total_bytes) / n_chunks : 0;
        double chunk_xfer_ms = (est_bw_mbps > 0 && per_chunk_bytes > 0)
            ? (per_chunk_bytes * 8.0 / (est_bw_mbps * 1e6) * 1000.0) : 99999.0;

        struct RaceEv { bool local; int idx; double t; };
        std::vector<RaceEv> events;
        events.reserve(2 * n_chunks);
        for (int i = 0; i < n_chunks; i++)
          events.push_back({true, i, (i + 1) * chunk_prefill_ms});
        for (int j = 0; j < n_chunks; j++) {
          int ci = n_chunks - 1 - j;
          events.push_back({false, ci, sender_compute_ms + (j + 1) * chunk_xfer_ms});
        }
        std::sort(events.begin(), events.end(),
                  [](const RaceEv& a, const RaceEv& b) { return a.t < b.t; });

        int local_done = 0, remote_min = n_chunks;
        for (const auto& e : events) {
          if (e.local) local_done = e.idx + 1;
          else remote_min = std::min(remote_min, e.idx);
          if (local_done >= remote_min) break;
        }
        double f_star = static_cast<double>(local_done) / n_chunks;
        // Cap f* so sender always sends at least 1 chunk.
        if (n_chunks > 0 && local_done >= n_chunks)
          f_star = static_cast<double>(n_chunks - 1) / n_chunks;

        size_t original_bytes = flow->total_bytes;
        flow->total_bytes = static_cast<size_t>(flow->total_bytes * (1.0 - f_star));
        if (flow->total_bytes == 0 && original_bytes > 0)
          flow->total_bytes = 1;
        size_t front_tokens = static_cast<size_t>(flow->context_tokens * f_star);
        flow->parallel_compute_ms = prefill_a * front_tokens;
        flow->front_context_tokens = front_tokens;

        fprintf(stderr, "[MAFS] Race overlap (pre): flow %d, f*=%.2f (%d/%d), "
                "est_bw=%.0f Mbps, bytes %zu->%zu, front=%zu tok, "
                "parallel_compute=%.0fms\n",
                flow->flow_id, f_star, local_done, n_chunks,
                est_bw_mbps, original_bytes, flow->total_bytes, front_tokens,
                flow->parallel_compute_ms);
      } else if (!race_mode2) {
        const char* olp_env = getenv("OVERLAPPED_PREFILL_RATIO");
        if (olp_env && strlen(olp_env) > 0 && flow->data_format == "kvzip") {
          double front_ratio = std::atof(olp_env);
          double bw_thresh_mbps = 100.0;
          const char* bw_th_env = getenv("OVERLAPPED_PREFILL_BW_THRESH_MBPS");
          if (bw_th_env) bw_thresh_mbps = std::atof(bw_th_env);
          const char* est_bw_env2 = getenv("RACE_EST_BW_MBPS");
          double est_bw_mbps2 = est_bw_env2 ? std::atof(est_bw_env2) : 999.0;
          bool low_bw2 = (est_bw_mbps2 <= bw_thresh_mbps);
          if (front_ratio > 0.0 && front_ratio < 1.0 && flow->context_tokens > 0 && low_bw2) {
            size_t original_bytes = flow->total_bytes;
            flow->total_bytes = static_cast<size_t>(flow->total_bytes * (1.0 - front_ratio));
            size_t front_tokens = static_cast<size_t>(flow->context_tokens * front_ratio);
            const char* pa = getenv("COMPUTE_PREFILL_A");
            const char* pb = getenv("COMPUTE_PREFILL_B");
            if (pa && pb) {
              double a = std::atof(pa), b = std::atof(pb);
              double ctx = static_cast<double>(front_tokens);
              flow->parallel_compute_ms = a * ctx + b * ctx * ctx;
            }
            fprintf(stderr, "[MAFS] Overlapped prefill: flow %d, ratio=%.2f, "
                    "est_bw=%.0f Mbps (thresh=%.0f), "
                    "bytes %zu->%zu, front_tokens=%zu, parallel_compute=%.0fms\n",
                    flow->flow_id, front_ratio, est_bw_mbps2, bw_thresh_mbps,
                    original_bytes, flow->total_bytes,
                    front_tokens, flow->parallel_compute_ms);
          } else if (!low_bw2 && front_ratio > 0.0) {
            fprintf(stderr, "[MAFS] Overlapped prefill SKIPPED (high BW): flow %d, "
                    "est_bw=%.0f Mbps > thresh=%.0f Mbps\n",
                    flow->flow_id, est_bw_mbps2, bw_thresh_mbps);
          }
        }
      }

      if (!flow->file_path.empty()) {
        flow->file_path = rate_sched_dir + flow->file_path;
        struct stat st;
        if (stat(flow->file_path.c_str(), &st) == 0) {
          if (flow->total_bytes == 0)
            flow->total_bytes = static_cast<size_t>(st.st_size);
          if (data_format_env && std::string(data_format_env) == "raw_text")
            flow->total_bytes = static_cast<size_t>(st.st_size);
        }
      }

      // Stream ID: start at 1 (0 is reserved for control channel)
      flow->stream_id = static_cast<int>(i) + 1;
      flow->chunk_buf.resize(256 * 1024, 'X');  // 256KB chunks

      fprintf(stderr, "[MAFS] Flow %d: label=%s, bytes=%zu, P_hat=%.0fms"
              ", parallel_compute=%.0fms, stream=%d"
              ", ctx_tokens=%zu, decode_tokens=%zu, actual_decode=%zu"
              ", data_format=%s, file_path=%s\n",
              flow->flow_id, flow->label.c_str(), flow->total_bytes,
              flow->P_hat_ms, flow->parallel_compute_ms, flow->stream_id,
              flow->context_tokens, flow->decode_tokens, flow->actual_decode_tokens,
              flow->data_format.c_str(), flow->file_path.c_str());

      sctp_flows_.push_back(std::move(flow));
    }
  }

  // Parse emulation section for final_decode_tokens.
  // If metadata explicitly provides emulation.final_decode_tokens, use it.
  // Otherwise derive per-query from flows: min(256, min(actual_decode_tokens)).
  // This matches v23_build_dataset.py's intended formula and keeps the final
  // decode time varying per query (instead of falling back to a constant 256).
  if (root.isMember("emulation") && root["emulation"].isMember("final_decode_tokens")) {
    final_decode_tokens_ = static_cast<size_t>(
        root["emulation"]["final_decode_tokens"].asUInt64());
    fprintf(stderr, "[MAFS] final_decode_tokens=%zu (from metadata.emulation)\n",
            final_decode_tokens_);
  } else if (!sctp_flows_.empty()) {
    size_t min_dec = 0;
    for (const auto& flow : sctp_flows_) {
      size_t d = flow->actual_decode_tokens > 0 ? flow->actual_decode_tokens
                                                : flow->decode_tokens;
      if (d > 0 && (min_dec == 0 || d < min_dec)) min_dec = d;
    }
    if (min_dec > 0) {
      final_decode_tokens_ = std::min<size_t>(256, min_dec);
      fprintf(stderr, "[MAFS] final_decode_tokens=%zu (derived from per-flow min)\n",
              final_decode_tokens_);
    }
  }

  // Re-register compute emulation flows on receiver for subsequent sets (incl. wrap-around)
  if (!sctp_flows_.empty() && !is_caller_ && compute_emulation_) {
    compute_emulation_->ResetForNewQuery();
    uint64_t query_id = static_cast<uint64_t>(set_index + 1);
    for (auto& flow : sctp_flows_) {
      compute_emulation_->RegisterFlowFull(
          flow->label, flow->stream_id, query_id, flow->total_bytes,
          flow->context_tokens, flow->decode_tokens,
          flow->actual_decode_tokens, final_decode_tokens_,
          flow->parallel_compute_ms,
          flow->front_context_tokens, flow->front_text_content);
    }
    fprintf(stderr, "[COMPUTE-EMULATION] Re-registered %zu flows for set %d (query_id=%lu)\n",
            sctp_flows_.size(), set_index, query_id);
  }

  fprintf(stderr, "[MAFS] LoadSetByIndex(%d): loaded %zu flows from %s\n",
          set_index, sctp_flows_.size(), metadata_path.c_str());
  return !sctp_flows_.empty();
}

void Conductor::SendControlMessage(const std::string& msg) {
  if (!control_channel_ ||
      control_channel_->state() != webrtc::DataChannelInterface::kOpen) {
    fprintf(stderr, "[CTRL] WARNING: control channel not open, cannot send: %s\n",
            msg.c_str());
    return;
  }
  webrtc::DataBuffer buffer(webrtc::CopyOnWriteBuffer(
      reinterpret_cast<const uint8_t*>(msg.data()), msg.size()), false);
  control_channel_->Send(std::move(buffer));
  fprintf(stderr, "[CTRL] Sent: %s\n", msg.c_str());
}

void Conductor::ControlObserver::OnStateChange() {
  if (!conductor_->control_channel_) return;
  auto state = conductor_->control_channel_->state();
  fprintf(stderr, "[CTRL] Control channel state: %d\n", static_cast<int>(state));
  if (state != webrtc::DataChannelInterface::kOpen) return;

  if (!conductor_->demo_mode_ && !conductor_->is_caller_ &&
      conductor_->compute_ready_ && !conductor_->receiver_ready_sent_ &&
      (conductor_->sequence_entries_.size() > 1 ||
       conductor_->max_queries_ > 0)) {
    conductor_->SendControlMessage(std::string("READY"));
    conductor_->receiver_ready_sent_ = true;
  }
}

void Conductor::ControlObserver::OnMessage(const webrtc::DataBuffer& buffer) {
  std::string msg(reinterpret_cast<const char*>(buffer.data.data()),
                  buffer.data.size());
  fprintf(stderr, "[CTRL] Received: %s\n", msg.c_str());
  conductor_->OnControlMessage(msg);
}

void Conductor::CreateMultiFlowDataChannels() {
  if (!peer_connection_ || sctp_flows_.empty()) return;

  // Create control channel first (stream_id=0)
  CreateControlChannel();

  for (auto& flow : sctp_flows_) {
    webrtc::DataChannelInit config;
    config.ordered = true;
    config.reliable = true;
    config.id = flow->stream_id;  // Explicit stream ID for MAFS priority control
    config.negotiated = true;     // Both sides must agree on ID

    auto result = peer_connection_->CreateDataChannelOrError(
        flow->label, &config);
    if (result.ok()) {
      flow->channel = result.MoveValue();
      flow->observer = std::make_unique<FlowObserver>(this, flow->flow_id);
      flow->channel->RegisterObserver(flow->observer.get());

      fprintf(stderr, "[MAFS] Created DataChannel '%s' stream_id=%d\n",
              flow->label.c_str(), flow->stream_id);
    } else {
      fprintf(stderr, "[MAFS] Failed to create DataChannel '%s'\n",
              flow->label.c_str());
    }
  }
}

void Conductor::FlowObserver::OnStateChange() {
  // Find the flow
  for (auto& flow : conductor_->sctp_flows_) {
    if (flow->flow_id == flow_id_) {
      auto state = flow->channel->state();
      if (state == webrtc::DataChannelInterface::kOpen) {
        conductor_->OnFlowChannelOpen(flow_id_);
      }
      break;
    }
  }
}

void Conductor::FlowObserver::OnMessage(const webrtc::DataBuffer& buffer) {
  for (auto& flow : conductor_->sctp_flows_) {
    if (flow->flow_id == flow_id_) {
      int64_t now_ms = webrtc::TimeMillis();
      if (flow->first_recv_ms == 0) flow->first_recv_ms = now_ms;
      flow->last_recv_ms = now_ms;
      flow->bytes_sent += buffer.data.size();  // Re-use bytes_sent as bytes_received on receiver

      // Notify UI of SCTP data for performance graph
      conductor_->main_wnd_->OnSctpDataReceived(buffer.data.size());
      if (auto* coord = webrtc::RtpSctpCoordinator::GetActiveInstance()) {
        coord->OnSctpDataReceived(buffer.data.size());
      }

      // Accumulate received data for real inference backends
      if (!flow->data_format.empty()) {
        recv_buffer_.insert(recv_buffer_.end(),
            buffer.data.data<uint8_t>(),
            buffer.data.data<uint8_t>() + buffer.data.size());
      }

      // Detect flow completion (receiver side)
      if (!flow->recv_complete && flow->total_bytes > 0 &&
          flow->bytes_sent.load() >= flow->total_bytes) {
        flow->recv_complete = true;
        fprintf(stderr, "[MAFS-RECV] Flow %d (%s) recv_complete: %zu/%zu bytes, "
                "first=%lldms, last=%lldms\n",
                flow->flow_id, flow->label.c_str(),
                static_cast<size_t>(flow->bytes_sent.load()), flow->total_bytes,
                (long long)flow->first_recv_ms, (long long)now_ms);

        // Pass accumulated data to compute emulation before signaling completion
        if (!flow->data_format.empty() && conductor_->compute_emulation_) {
          if (flow->data_format == "kv_cache" || flow->data_format == "kvzip") {
            conductor_->compute_emulation_->SetFlowData(
                flow->label, std::move(recv_buffer_), "",
                flow->data_format);
          } else if (flow->data_format == "raw_text") {
            std::string text(recv_buffer_.begin(), recv_buffer_.end());
            conductor_->compute_emulation_->SetFlowData(
                flow->label, {}, std::move(text),
                flow->data_format);
          }
          recv_buffer_.shrink_to_fit();
        }

        if (conductor_->compute_emulation_) {
          conductor_->compute_emulation_->OnFlowCompleted(
              flow->label, flow->first_recv_ms, now_ms);
        }
      }
      break;
    }
  }
}

void Conductor::FlowObserver::OnBufferedAmountChange(uint64_t sent_data_size) {
  for (auto& flow : conductor_->sctp_flows_) {
    if (flow->flow_id == flow_id_ && flow->send_active.load()) {
      uint64_t buffered = flow->channel->buffered_amount();
      constexpr uint64_t kLowThreshold = 512 * 1024;
      if (buffered < kLowThreshold) {
        conductor_->PumpFlowData(flow_id_);
      }
      break;
    }
  }
}

void Conductor::OnFlowChannelOpen(int flow_id) {
  for (auto& flow : sctp_flows_) {
    if (flow->flow_id == flow_id) {
      flow->channel_open = true;
      break;
    }
  }

  int open_count = CountOpenFlowChannels();
  fprintf(stderr, "[MAFS] Flow %d channel OPEN (%d/%zu open)\n",
          flow_id, open_count, sctp_flows_.size());

  // Only the sender (caller) starts multi-flow send
  if (!is_caller_) {
    fprintf(stderr, "[MAFS] Receiver side — not starting send\n");
    return;
  }

  // Start multi-flow send when all channels are open (exactly once).
  // In receiver-driven multi-query mode, stall here until a READY arrives
  // from the receiver (emitted after compute-path warmup completes).
  if (open_count == static_cast<int>(sctp_flows_.size()) &&
      !multi_flow_send_started_) {
    const bool multi_query_mode =
        (sequence_entries_.size() > 1 || max_queries_ > 0);
    if (multi_query_mode && !ready_received_) {
      fprintf(stderr,
              "[MAFS] All %zu flow channels open, waiting for receiver READY...\n",
              sctp_flows_.size());
      return;
    }

    multi_flow_send_started_ = true;
    int total_delay_ms = 2000 + start_delay_ms_;
    fprintf(stderr, "[MAFS] All %zu flow channels open, starting send in %dms (2s + %dms start_delay)\n",
            sctp_flows_.size(), total_delay_ms, start_delay_ms_);
    std::thread([this, total_delay_ms]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(total_delay_ms));
      StartMultiFlowSend();
    }).detach();
  }
}

int Conductor::CountOpenFlowChannels() const {
  int count = 0;
  for (const auto& flow : sctp_flows_) {
    if (flow->channel_open) count++;
  }
  return count;
}

void Conductor::StartMultiFlowSend() {
  // Initialize conservative burst mode from environment
  InitConservativeBurstMode();

  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();

  // Register flows with MAFS coordinator (must be here, not in CreateMultiFlowDataChannels,
  // because RtpSctpCoordinator is created after SCTP transport setup, which happens after
  // DataChannel creation. The 2s delay after channel open guarantees coordinator exists.)
  auto* coord = webrtc::RtpSctpCoordinator::GetActiveInstance();
  if (coord && coord->IsMafsEnabled()) {
    for (auto& flow : sctp_flows_) {
      flow->mafs_flow_id = coord->RegisterFlow(
          flow->stream_id, flow->label, flow->total_bytes);
      // In RACE mode, skip P_hat so all flows get equal WFQ priority.
      // Original P_hat from metadata reflects full transfer size, not race-reduced
      // bytes, causing priority inversion that starves certain flows.
      bool race_mode_mafs = IsHafsMode();
      if (!race_mode_mafs && (flow->P_hat_ms > 0 || flow->sigma_P_ms > 0)) {
        coord->SetComputeTime(flow->mafs_flow_id,
                              flow->P_hat_ms, flow->sigma_P_ms);
      }
      fprintf(stderr, "[MAFS] Registered flow %d (mafs_id=%u) stream=%d%s\n",
              flow->flow_id, flow->mafs_flow_id, flow->stream_id,
              race_mode_mafs ? " [RACE: P_hat skipped]" : "");
    }
    fprintf(stderr, "[MAFS] MAFS enabled, triggering initial priority update\n");
    coord->ForceUpdateMafsPriorities();
  }

  // Two-pass initialization so that tiny flows (e.g. raw_text with a few
  // KB) that complete inside PumpFlowData don't trigger FinishFlowSend's
  // `all_done` branch prematurely while the remaining flows haven't
  // been marked send_active yet. First pass sets send_active=true on
  // every flow; second pass actually pumps.
  for (auto& flow : sctp_flows_) {
    if (!flow->channel || !flow->channel_open) continue;
    flow->send_active = true;
    flow->bytes_sent = 0;
    flow->send_start_ms = now_ms;
    flow->send_end_ms = now_ms + (test_duration_sec_ * 1000);
  }

  for (auto& flow : sctp_flows_) {
    if (!flow->channel || !flow->channel_open) continue;

    // V22: Load real file data if file_path is specified
    if (!flow->file_path.empty() && flow->file_data.empty()) {
      std::ifstream file(flow->file_path, std::ios::binary | std::ios::ate);
      if (file.is_open()) {
        auto file_size = file.tellg();
        file.seekg(0);
        flow->file_data.resize(static_cast<size_t>(file_size));
        file.read(reinterpret_cast<char*>(flow->file_data.data()), file_size);
        if (flow->total_bytes == 0) {
          flow->total_bytes = static_cast<size_t>(file_size);
        }
        fprintf(stderr, "[MAFS] Loaded file %s (%zu bytes) for flow %d\n",
                flow->file_path.c_str(), flow->file_data.size(), flow->flow_id);
      } else {
        fprintf(stderr, "[MAFS] WARNING: Cannot open file %s for flow %d\n",
                flow->file_path.c_str(), flow->flow_id);
      }
    }

    // send_active, bytes_sent, send_start_ms, send_end_ms were already
    // set in the first pass above; skip re-initialising here so tiny
    // flows can't race through FinishFlowSend's all_done branch before
    // their sibling flows are marked active.

    fprintf(stderr, "[MAFS] Starting flow %d (%s): %zu bytes, duration=%ds\n",
            flow->flow_id, flow->label.c_str(), flow->total_bytes,
            test_duration_sec_);

    // Prime each flow's buffer. PumpFlowData caps at kHighWaterMark and
    // the SchedulePumpTimer (below) then refills unconditionally every
    // 200 ms so WFQ-starved low-priority streams don't stall waiting on
    // OnBufferedAmountChange (see post-loop comment in PumpFlowData).
    PumpFlowData(flow->flow_id);
  }

  // Start periodic pump timer as fallback for stalled callback chains
  pump_timer_active_ = true;
  SchedulePumpTimer();
}

void Conductor::PumpFlowData(int flow_id) {
  SctpFlow* flow = nullptr;
  for (auto& f : sctp_flows_) {
    if (f->flow_id == flow_id) { flow = f.get(); break; }
  }
  if (!flow || !flow->send_active.load() || !flow->channel) return;

  // Conservative byte-burst: skip pumping during idle period. Resume timer
  // is armed exactly once per idle entry in OnBurstBytesSent (generation-
  // guarded so stale threads abort); no need to re-arm here.
  if (!IsConservativeBurstOpen()) {
    return;
  }

  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();

  // Check time limit
  if (now_ms >= flow->send_end_ms) {
    FinishFlowSend(flow_id);
    return;
  }

  // Check byte limit (if total_bytes > 0)
  if (flow->total_bytes > 0 && flow->bytes_sent.load() >= flow->total_bytes) {
    FinishFlowSend(flow_id);
    return;
  }

  constexpr uint64_t kHighWaterMark = 1536 * 1024;  // 1.5MB

  while (flow->channel->buffered_amount() < kHighWaterMark &&
         flow->send_active.load()) {
    // Conservative burst: stop pumping if idle started mid-loop
    if (!IsConservativeBurstOpen()) break;
    // Conservative pacing: cap in-burst send rate so the bottleneck queue
    // doesn't spike and GCC doesn't collapse the video bitrate. Retries come
    // from OnBufferedAmountChange + the 200ms SchedulePumpTimer.
    if (!IsConservativePaceOpen()) break;

    // Respect byte limit — use signed arithmetic to avoid underflow
    uint64_t sent_now = flow->bytes_sent.load();
    if (flow->total_bytes > 0 && sent_now >= flow->total_bytes) {
      FinishFlowSend(flow_id);
      return;
    }
    size_t remaining = flow->total_bytes > 0
        ? static_cast<size_t>(flow->total_bytes - sent_now)
        : flow->chunk_buf.size();
    size_t send_size = std::min(remaining, flow->chunk_buf.size());

    // Use real file data if available and large enough, otherwise use dummy chunk_buf
    const uint8_t* data_ptr = flow->chunk_buf.data();
    if (!flow->file_data.empty() &&
        sent_now + send_size <= flow->file_data.size()) {
      data_ptr = flow->file_data.data() + static_cast<size_t>(sent_now);
    }

    webrtc::DataBuffer buffer(
        webrtc::CopyOnWriteBuffer(data_ptr, send_size), true);
    flow->channel->SendAsync(std::move(buffer), nullptr);
    flow->bytes_sent += send_size;

    // Notify MAFS coordinator of sent data
    auto* coord = webrtc::RtpSctpCoordinator::GetActiveInstance();
    if (coord && coord->IsMafsEnabled()) {
      coord->OnFlowDataSent(flow->stream_id, send_size);
    }

    // Conservative byte-burst: accumulate + trigger idle at threshold
    OnBurstBytesSent(send_size);
    if (burst_idle_active_) break;  // stop pumping immediately once idle starts
  }

  // Post-loop check: the while loop may exit due to buffer full (buffered_amount >= kHighWaterMark)
  // without re-checking bytes_sent. With WFQ, low-priority streams may never drain their buffer,
  // so OnBufferedAmountChange never fires and PumpFlowData is never called again.
  // Catch completion here to avoid flows hanging forever.
  if (flow->total_bytes > 0 && flow->bytes_sent.load() >= flow->total_bytes) {
    FinishFlowSend(flow_id);
  }
}

void Conductor::SchedulePumpTimer() {
  if (!pump_timer_active_.load()) return;
  std::thread([this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!pump_timer_active_.load()) return;
    bool any_active = false;
    for (auto& flow : sctp_flows_) {
      if (flow->send_active.load() && flow->channel) {
        any_active = true;
        // Unconditional refill — PumpFlowData's inner while-loop exits
        // when `buffered >= kHighWaterMark` so calling it every tick is
        // cheap and idempotent for streams at the cap. This is the
        // backstop for WFQ-starved streams where OnBufferedAmountChange
        // never fires: once dcsctp's scheduler starts picking the
        // previously-starved stream (e.g. after a higher-priority one
        // finishes), the timer tops it up without relying on the
        // callback chain.
        PumpFlowData(flow->flow_id);
      }
    }
    if (any_active) SchedulePumpTimer();
    else pump_timer_active_ = false;
  }).detach();
}

void Conductor::FinishFlowSend(int flow_id) {
  SctpFlow* flow = nullptr;
  for (auto& f : sctp_flows_) {
    if (f->flow_id == flow_id) { flow = f.get(); break; }
  }
  if (!flow || !flow->send_active.exchange(false)) return;

  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  flow->actual_end_ms = now_ms;
  int64_t duration_ms = now_ms - flow->send_start_ms;
  uint64_t total = flow->bytes_sent.load();
  double mbps = duration_ms > 0 ? (total * 8.0) / (duration_ms * 1000.0) : 0;

  // Update shared measured BW for future pre-send race simulations (sender side only)
  if (is_caller_ && mbps > 0) {
    g_race_measured_bw_mbps.store(mbps, std::memory_order_relaxed);
  }

  // Race-based overlapped prefill: active for HAFS, not for NC/FSE.
  bool race_mode = IsHafsMode();
  if (race_mode && flow->context_tokens > 0 && duration_ms > 0) {
    // Chunk configuration
    int chunk_size = 128;
    const char* cs_env = getenv("RACE_CHUNK_SIZE");
    if (cs_env) chunk_size = std::atoi(cs_env);

    // Receiver per-chunk prefill time (ms)
    // Auto-compute from COMPUTE_PREFILL_A (ms/tok) * chunk_size
    double chunk_prefill_ms = 0.0;
    const char* cp_env = getenv("RACE_RECEIVER_CHUNK_MS");
    if (cp_env) {
      chunk_prefill_ms = std::atof(cp_env);
    } else {
      const char* pa_env = getenv("COMPUTE_PREFILL_A");
      if (pa_env) {
        chunk_prefill_ms = std::atof(pa_env) * chunk_size;
      } else {
        chunk_prefill_ms = 2.336 * chunk_size;  // Jetson Orin default
      }
    }

    // Sender compute time before streaming (prefill+score+prune+serialize on sender)
    double sender_compute_ms = 0.0;
    const char* sc_env = getenv("RACE_SENDER_COMPUTE_MS");
    if (sc_env) {
      sender_compute_ms = std::atof(sc_env);
    } else {
      // Auto-estimate from context_tokens (GH200 measured regression)
      // T_sender ≈ 0.075 * ctx + 15ms (prefill+score+prune+serialize)
      sender_compute_ms = 0.075 * flow->context_tokens + 15.0;
    }

    int n_chunks = (static_cast<int>(flow->context_tokens) + chunk_size - 1) / chunk_size;
    double per_chunk_bytes = n_chunks > 0 ? static_cast<double>(total) / n_chunks : 0;
    double chunk_xfer_ms = (mbps > 0 && per_chunk_bytes > 0)
        ? (per_chunk_bytes * 8.0 / (mbps * 1e6) * 1000.0) : 99999.0;

    // Race simulation: receiver prefills front-to-back, sender streams back-to-front
    // Receiver: chunk i done at (i+1) * chunk_prefill_ms
    // Sender: chunk (N-1-j) arrives at sender_compute_ms + (j+1) * chunk_xfer_ms
    struct RaceEvent { bool local; int idx; double t; };
    std::vector<RaceEvent> events;
    events.reserve(2 * n_chunks);
    for (int i = 0; i < n_chunks; i++)
      events.push_back({true, i, (i + 1) * chunk_prefill_ms});
    for (int j = 0; j < n_chunks; j++) {
      int ci = n_chunks - 1 - j;
      events.push_back({false, ci, sender_compute_ms + (j + 1) * chunk_xfer_ms});
    }
    std::sort(events.begin(), events.end(),
              [](const RaceEvent& a, const RaceEvent& b) { return a.t < b.t; });

    int local_done = 0, remote_min = n_chunks;
    double T_meeting = sender_compute_ms + duration_ms;  // worst case
    for (const auto& e : events) {
      if (e.local) local_done = e.idx + 1;
      else remote_min = std::min(remote_min, e.idx);
      if (local_done >= remote_min) {
        T_meeting = e.t;
        break;
      }
    }

    double dynamic_f = static_cast<double>(local_done) / n_chunks;
    flow->parallel_compute_ms = T_meeting;
    flow->effective_ms = static_cast<int64_t>(T_meeting);

    fprintf(stderr, "[MAFS] Race overlap: flow %d (%s), ctx=%zu, chunks=%d, "
            "f*=%.2f (%d/%d local), sender_compute=%.0fms, "
            "transfer=%ldms (%.1f Mbps), chunk_xfer=%.1fms, "
            "chunk_prefill=%.1fms, meeting=%.0fms, effective=%ldms\n",
            flow->flow_id, flow->label.c_str(), flow->context_tokens, n_chunks,
            dynamic_f, local_done, n_chunks, sender_compute_ms,
            duration_ms, mbps, chunk_xfer_ms, chunk_prefill_ms,
            T_meeting, flow->effective_ms);
  } else if (flow->parallel_compute_ms > 0) {
    // Legacy static overlap
    flow->effective_ms = std::max(duration_ms, static_cast<int64_t>(flow->parallel_compute_ms));
    fprintf(stderr, "[MAFS] Flow %d (%s) complete: %lu bytes, %.2f Mbps, transfer=%ldms"
            ", parallel_compute=%.0fms, effective=%ldms\n",
            flow->flow_id, flow->label.c_str(), total, mbps, duration_ms,
            flow->parallel_compute_ms, flow->effective_ms);
  } else {
    flow->effective_ms = duration_ms;
    fprintf(stderr, "[MAFS] Flow %d (%s) complete: %lu bytes, %.2f Mbps, transfer=%ldms"
            ", effective=%ldms\n",
            flow->flow_id, flow->label.c_str(), total, mbps, duration_ms,
            flow->effective_ms);
  }

  // Notify MAFS coordinator
  auto* coord = webrtc::RtpSctpCoordinator::GetActiveInstance();
  if (coord && coord->IsMafsEnabled() && flow->mafs_flow_id > 0) {
    coord->MarkFlowComplete(flow->mafs_flow_id);
  }

  // Single-query mode: write after each flow finishes (overwrites previous
  // with progress snapshot). Multi-query mode: defer to the all_done branch
  // below so each query produces exactly one set of rows (query_id keyed).
  const bool multi_query_mode =
      (sequence_entries_.size() > 1 || max_queries_ > 0);
  if (!multi_query_mode) {
    WriteFlowCompletionCsv();
  }

  // Check if all flows are done
  bool all_done = true;
  for (const auto& f : sctp_flows_) {
    if (f->send_active.load()) { all_done = false; break; }
  }
  if (all_done) {
    // Multi-query: flush one row set for the current query_iteration, then
    // bump the sender-side counter so the next query's rows get appended.
    if (multi_query_mode) {
      WriteFlowCompletionCsv();
      sender_query_iteration_++;
    }
    pump_timer_active_ = false;  // Stop timer before repeat/next logic
    // Receiver-driven when either multi-set CSV or MAX_QUERIES > 0.
    if (sequence_entries_.size() > 1 || max_queries_ > 0) {
      // Sync measured BW to receiver so next set's overlapped prefill f*
      // is computed with the same estimate on both sides.
      double cur_bw = g_race_measured_bw_mbps.load(std::memory_order_relaxed);
      if (IsHafsMode() && cur_bw > 0) {
        std::string bw_msg = "BW_UPDATE:" + std::to_string(cur_bw);
        SendControlMessage(bw_msg);
        fprintf(stderr, "[CTRL] Sender: sent BW_UPDATE %.1f Mbps to receiver\n", cur_bw);
      }
      // Receiver-driven mode: sender waits for NEXT signal from receiver
      waiting_for_receiver_ = true;
      fprintf(stderr,
              "[MAFS] Set %d sent (%zu flows), waiting for receiver compute "
              "(max_queries=%d)...\n",
              current_set_index_, sctp_flows_.size(), max_queries_);
    } else {
      // Legacy single-set mode: use repeat_count logic
      current_repeat_++;
      bool should_repeat = (repeat_count_ <= 0) ||
                           (current_repeat_ < repeat_count_);

      int64_t now_check = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      bool time_left = true;
      for (const auto& f : sctp_flows_) {
        if (now_check >= f->send_end_ms) { time_left = false; break; }
      }

      if (should_repeat && time_left) {
        fprintf(stderr, "[MAFS] All flows complete (repeat %d/%d), "
                "restarting in %dms\n",
                current_repeat_, repeat_count_, query_complete_delay_ms_);
        std::thread([this]() {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(query_complete_delay_ms_));
          RestartFlowSend();
        }).detach();
      } else {
        fprintf(stderr, "[MAFS] All flows complete! (repeat %d/%d, time_left=%d)\n",
                current_repeat_, repeat_count_, time_left);
      }
    }
  }
}

void Conductor::ResetFlowsForNewSet() {
  for (auto& flow : sctp_flows_) {
    flow->send_active = false;
    flow->bytes_sent = 0;
    flow->actual_end_ms = 0;
    flow->send_start_ms = 0;
    flow->recv_complete = false;
    flow->first_recv_ms = 0;
    flow->last_recv_ms = 0;
    flow->file_data.clear();
    flow->mafs_flow_id = 0;
    if (flow->observer) flow->observer->ClearRecvBuffer();
    // channel, observer, stream_id, chunk_buf — reused
  }
  fprintf(stderr, "[MAFS] ResetFlowsForNewSet: cleared %zu flows\n",
          sctp_flows_.size());
}

void Conductor::OnReceiverQueryComplete(uint64_t query_id) {
  total_queries_completed_++;
  int next_set = current_set_index_ + 1;

  fprintf(stderr, "[CTRL] OnReceiverQueryComplete: query_id=%lu, completed=%d, "
          "next_set=%d/%zu max_queries=%d\n",
          query_id, total_queries_completed_, next_set,
          sequence_entries_.size(), max_queries_);

  // MAX_QUERIES exit: once we've completed the requested number of queries,
  // tell the sender to stop and tear down cleanly. Without this the
  // multi-set loop just wraps around forever.
  // Note: this runs on the RECEIVER side, which doesn't own
  // flow_completion.csv (that's sender-written), so we just signal DONE
  // and exit. The sender's DONE handler similarly skips the per-query
  // flush since FinishFlowSend already flushed for the completed query.
  if (max_queries_ > 0 && total_queries_completed_ >= max_queries_) {
    fprintf(stderr,
            "[CTRL] MAX_QUERIES=%d reached after query %lu — sending DONE "
            "and scheduling shutdown\n",
            max_queries_, query_id);
    SendControlMessage(std::string("DONE"));
    std::thread([this]() {
      // Give the sender a moment to observe DONE and tear down
      std::this_thread::sleep_for(std::chrono::seconds(2));
      DisconnectFromServer();
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      // Mirror the SIGTERM exit path used by main.cc::SignalHandler:
      // fast process exit so the headless runner's sender/receiver wait
      // unblocks.
      _exit(0);
    }).detach();
    return;  // do not wrap / do not schedule next query
  }

  if (next_set >= static_cast<int>(sequence_entries_.size())) {
    // Wrap around to first set for continuous operation
    next_set = 0;
    fprintf(stderr, "[CTRL] All %zu sets complete (cycle %d), wrapping to set 0\n",
            sequence_entries_.size(), total_queries_completed_);
  }

  // Get configurable delay (env QUERY_INTERVAL_MS overrides per-entry value)
  int delay_ms = sequence_entries_[current_set_index_].query_complete_delay_ms;
  const char* env = std::getenv("QUERY_INTERVAL_MS");
  if (env) delay_ms = std::atoi(env);

  // Delay → prepare next set → signal sender
  std::thread([this, next_set, delay_ms]() {
    if (delay_ms > 0) {
      fprintf(stderr, "[CTRL] Receiver: waiting %dms before requesting set %d\n",
              delay_ms, next_set);
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    current_set_index_ = next_set;
    ResetFlowsForNewSet();
    LoadSetByIndex(next_set);  // re-register compute flows on receiver

    SendControlMessage("NEXT:set_index=" + std::to_string(next_set));
  }).detach();
}

void Conductor::RestartFlowSend() {
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();

  fprintf(stderr, "[MAFS] RestartFlowSend: repeat %d, resetting %zu flows\n",
          current_repeat_, sctp_flows_.size());

  for (auto& flow : sctp_flows_) {
    if (!flow->channel || !flow->channel_open) continue;

    // Check time limit
    if (now_ms >= flow->send_end_ms) {
      fprintf(stderr, "[MAFS] Flow %d: time limit reached, not restarting\n",
              flow->flow_id);
      continue;
    }

    // Reset send state for this repeat
    flow->send_active = true;
    flow->bytes_sent = 0;
    flow->actual_end_ms = 0;
    flow->send_start_ms = now_ms;
    // Keep send_end_ms from original (absolute time limit)

    fprintf(stderr, "[MAFS] Restarting flow %d (%s): %zu bytes\n",
            flow->flow_id, flow->label.c_str(), flow->total_bytes);

    PumpFlowData(flow->flow_id);
  }

  // Re-start pump timer for the new repeat
  pump_timer_active_ = true;
  SchedulePumpTimer();
}

void Conductor::WriteFlowCompletionCsv() {
  // Only the sender writes flow_completion.csv (receiver has no timing data)
  if (!is_caller_) return;

  std::string csv_path;
  if (!log_dir_.empty()) {
    csv_path = log_dir_ + "/flow_completion.csv";
  } else {
    csv_path = "flow_completion.csv";
  }

  // Multi-query mode: trunc+header on the first flush, append on subsequent
  // query boundaries. Skip flushes that would overwrite previously written
  // query rows for a different iteration.
  const bool multi_query =
      (sequence_entries_.size() > 1 || max_queries_ > 0);
  const bool append_mode = multi_query && flow_completion_header_written_;
  std::ofstream csv(
      csv_path,
      append_mode ? (std::ios::out | std::ios::app)
                  : (std::ios::out | std::ios::trunc));
  if (!csv.is_open()) {
    fprintf(stderr, "[MAFS] Cannot open %s for writing\n", csv_path.c_str());
    return;
  }

  // Header: always written in trunc mode (file is fresh), skipped in
  // append mode (already present from the first flush).
  if (!append_mode) {
    csv << "query_id,flow_id,label,stream_id,total_bytes,bytes_sent,P_hat_ms,"
        << "parallel_compute_ms,start_ms,end_ms,duration_ms,effective_ms,"
        << "throughput_mbps,mafs_flow_id\n";
    flow_completion_header_written_ = true;
  }

  int64_t earliest_start = INT64_MAX;
  int64_t latest_end = 0;

  // query_id column: sender-side iteration counter. In single-query mode
  // this is just 0 (never incremented). In multi-query mode it increments
  // in FinishFlowSend's all_done branch once per completed query iteration,
  // keeping CSV rows grouped by query.
  const int query_id_for_row = sender_query_iteration_;
  last_flushed_query_iteration_ = query_id_for_row;

  for (const auto& flow : sctp_flows_) {
    uint64_t sent = flow->bytes_sent.load();
    int64_t end_ms = flow->send_start_ms;
    int64_t dur = 0;
    if (flow->actual_end_ms > 0) {
      // Flow completed normally — use recorded completion time
      end_ms = flow->actual_end_ms;
      dur = end_ms - flow->send_start_ms;
    } else if (sent > 0) {
      // Flow still in progress — use current time
      end_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      dur = end_ms - flow->send_start_ms;
    }
    double mbps = dur > 0 ? (sent * 8.0) / (dur * 1000.0) : 0;

    // effective_ms: use stored value if completed, else compute from current dur
    int64_t eff = flow->effective_ms;
    if (eff == 0 && dur > 0) {
      eff = (flow->parallel_compute_ms > 0)
          ? std::max(dur, static_cast<int64_t>(flow->parallel_compute_ms))
          : dur;
    }

    if (flow->send_start_ms < earliest_start)
      earliest_start = flow->send_start_ms;
    if (end_ms > latest_end) latest_end = end_ms;

    csv << query_id_for_row << ","
        << flow->flow_id << ","
        << flow->label << ","
        << flow->stream_id << ","
        << flow->total_bytes << ","
        << sent << ","
        << flow->P_hat_ms << ","
        << flow->parallel_compute_ms << ","
        << flow->send_start_ms << ","
        << end_ms << ","
        << dur << ","
        << eff << ","
        << mbps << ","
        << flow->mafs_flow_id << "\n";
  }

  // Makespan: use effective end = start + effective_ms for each flow
  int64_t latest_effective_end = latest_end;
  for (const auto& flow : sctp_flows_) {
    if (flow->effective_ms > 0 && flow->send_start_ms > 0) {
      int64_t eff_end = flow->send_start_ms + flow->effective_ms;
      if (eff_end > latest_effective_end)
        latest_effective_end = eff_end;
    }
  }
  int64_t makespan_ms = latest_effective_end - earliest_start;
  csv << "# makespan_ms=" << makespan_ms << "\n";
  csv.close();

  fprintf(stderr, "[MAFS] flow_completion.csv written to %s (makespan=%ldms)\n",
          csv_path.c_str(), makespan_ms);
}

void Conductor::CreateReceiverFlowChannels() {
  if (!peer_connection_ || sctp_flows_.empty()) return;

  // Create control channel first (stream_id=0)
  CreateControlChannel();

  for (auto& flow : sctp_flows_) {
    webrtc::DataChannelInit config;
    config.ordered = true;
    config.reliable = true;
    config.id = flow->stream_id;
    config.negotiated = true;

    auto result = peer_connection_->CreateDataChannelOrError(
        flow->label, &config);
    if (result.ok()) {
      flow->channel = result.MoveValue();
      flow->observer = std::make_unique<FlowObserver>(this, flow->flow_id);
      flow->channel->RegisterObserver(flow->observer.get());
      // Note: channel_open will be set by OnFlowChannelOpen when kOpen fires.
      // Receiver does NOT start sends (is_caller_ check in OnFlowChannelOpen).
      fprintf(stderr, "[MAFS-RECV] Created negotiated DataChannel '%s' stream_id=%d\n",
              flow->label.c_str(), flow->stream_id);
    } else {
      fprintf(stderr, "[MAFS-RECV] Failed to create DataChannel '%s'\n",
              flow->label.c_str());
    }
  }
}

// ===== Conservative Time-Burst Mode =====
// Pace at CONSERVATIVE_BURST_RATE_MBPS for CONSERVATIVE_BURST_SEC seconds,
// then idle for CONSERVATIVE_IDLE_SEC, repeat. Bounded duty cycle avoids
// queue spikes that kill RTP video under GCC.
// Activated when COORDINATOR_MODE=conservative.

void Conductor::InitConservativeBurstMode() {
  const char* mode_env = std::getenv("COORDINATOR_MODE");
  if (!mode_env || std::string(mode_env) != "conservative") return;

  conservative_burst_enabled_ = true;

  const char* burst_env = std::getenv("CONSERVATIVE_BURST_SEC");
  if (burst_env) {
    conservative_burst_sec_ = std::atof(burst_env);
  }

  const char* idle_env = std::getenv("CONSERVATIVE_IDLE_SEC");
  if (idle_env) {
    conservative_idle_sec_ = std::atof(idle_env);
  }

  const char* rate_env = std::getenv("CONSERVATIVE_BURST_RATE_MBPS");
  if (rate_env) {
    conservative_burst_rate_mbps_ = std::atof(rate_env);
  }

  burst_bytes_sent_ = 0;
  burst_idle_active_ = false;
  burst_idle_start_ms_ = 0;
  burst_start_ms_ = 0;

  fprintf(stderr, "[SCTP][Conservative] Time-burst mode: burst=%.1fs, idle=%.1fs, rate=%.1fMbps (duty=%.1f%%)\n",
          conservative_burst_sec_, conservative_idle_sec_,
          conservative_burst_rate_mbps_,
          100.0 * conservative_burst_sec_ /
              (conservative_burst_sec_ + conservative_idle_sec_));
}

bool Conductor::IsConservativeBurstOpen() {
  if (!conservative_burst_enabled_) return true;

  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();

  if (burst_idle_active_) {
    // Idle phase: auto-resume when elapsed >= idle_sec (backup for stale timer).
    double elapsed = (now_ms - burst_idle_start_ms_) / 1000.0;
    if (elapsed >= conservative_idle_sec_) {
      burst_idle_active_ = false;
      burst_bytes_sent_ = 0;
      burst_start_ms_ = 0;
      fprintf(stderr, "[SCTP][Conservative] Idle complete (%.1fs), resuming\n", elapsed);
      return true;
    }
    return false;
  }

  // Burst phase: enter idle when burst duration expires.
  if (burst_start_ms_ != 0) {
    double burst_elapsed = (now_ms - burst_start_ms_) / 1000.0;
    if (burst_elapsed >= conservative_burst_sec_) {
      burst_idle_active_ = true;
      burst_idle_start_ms_ = now_ms;
      burst_bytes_sent_ = 0;
      burst_start_ms_ = 0;
      ++burst_idle_gen_;
      fprintf(stderr, "[SCTP][Conservative] Burst complete (%.1fs), entering idle for %.1fs (gen=%lld)\n",
              burst_elapsed, conservative_idle_sec_,
              (long long)burst_idle_gen_);
      ScheduleBurstResume();
      return false;
    }
  }
  return true;
}

// In-burst pacing gate. Closed while the cumulative in-burst send rate is
// above CONSERVATIVE_BURST_RATE_MBPS; the existing 200ms SchedulePumpTimer
// + OnBufferedAmountChange re-entries drive the retry loop naturally.
// Also stamps burst_start_ms_ on the first send of a new burst, which the
// burst-duration check in IsConservativeBurstOpen() keys off.
bool Conductor::IsConservativePaceOpen() {
  if (!conservative_burst_enabled_) return true;
  if (conservative_burst_rate_mbps_ <= 0.0) return true;
  if (burst_idle_active_) return true;  // idle gate dominates

  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  if (burst_start_ms_ == 0) {
    burst_start_ms_ = now_ms;
    return true;
  }
  double elapsed_sec = (now_ms - burst_start_ms_) / 1000.0;
  double budget_bytes =
      conservative_burst_rate_mbps_ * 1e6 / 8.0 * elapsed_sec;
  return static_cast<double>(burst_bytes_sent_) <= budget_bytes;
}

void Conductor::OnBurstBytesSent(size_t bytes) {
  if (!conservative_burst_enabled_ || burst_idle_active_) return;
  burst_bytes_sent_ += static_cast<int64_t>(bytes);
}

void Conductor::ScheduleBurstResume() {
  int idle_ms = static_cast<int>(conservative_idle_sec_ * 1000);
  int64_t my_gen = burst_idle_gen_;
  std::thread([this, idle_ms, my_gen]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(idle_ms));
    // Guard against stale timers: if idle was already resumed or a newer idle
    // cycle has begun, our generation won't match current — do nothing.
    if (!burst_idle_active_ || my_gen != burst_idle_gen_) return;
    burst_idle_active_ = false;
    burst_bytes_sent_ = 0;
    fprintf(stderr, "[SCTP][Conservative] Idle timer fired (gen=%lld), resuming all flows\n",
            (long long)my_gen);
    for (auto& flow : sctp_flows_) {
      if (flow->send_active.load()) {
        PumpFlowData(flow->flow_id);
      }
    }
  }).detach();
}
