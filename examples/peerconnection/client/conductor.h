/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef EXAMPLES_PEERCONNECTION_CLIENT_CONDUCTOR_H_
#define EXAMPLES_PEERCONNECTION_CLIENT_CONDUCTOR_H_

#include <curl/curl.h>

#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/types/span.h"
#include "api/data_channel_interface.h"
#include "api/jsep.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/rtp_receiver_interface.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/task_queue_factory.h"
#include "examples/peerconnection/client/main_wnd.h"
#include "examples/peerconnection/client/peer_connection_client.h"
#include "examples/peerconnection/client/rtc_stats_collector.h"
#include "examples/peerconnection/client/traffic_profile.h"
#include "examples/peerconnection/client/websocket_client.h"
#include "json/value.h"
#include "rtc_base/thread.h"
#include "sctp_traffic/bulk/bulk_receiver.h"
#include "sctp_traffic/bulk/bulk_sender.h"
#include "sctp_traffic/file/file_receiver.h"
#include "sctp_traffic/file/file_sender.h"

namespace webrtc {
class VideoCaptureModule;
}  // namespace webrtc

namespace cricket {
class VideoRenderer;
}  // namespace cricket

namespace webrtc_example {
class RemoteMediaRecorder;
}  // namespace webrtc_example

class MyDataObserver;

class Conductor : public webrtc::PeerConnectionObserver,
                  public webrtc::CreateSessionDescriptionObserver,
                  public PeerConnectionClientObserver,
                  public MainWndCallback {
 public:
  enum CallbackID {
    MEDIA_CHANNELS_INITIALIZED = 1,
    PEER_CONNECTION_CLOSED,
    SEND_MESSAGE_TO_PEER,
    NEW_TRACK_ADDED,
    TRACK_REMOVED,
  };

  // Stats callback types
  enum StatsType {
    STATS_CONNECTING,      // ICE connection state changes
    STATS_CONNECTED,       // When peer connection is established
    STATS_DISCONNECTED,    // When peer connection is closed
    STATS_RATE_UPDATED,    // Bitrate/framerate updates
    STATS_ERROR           // Any errors during stats collection
  };


  Conductor(PeerConnectionClient* client, MainWindow* main_wnd, bool headless);

  void Start();

  bool connection_active() const;

  void Close() override;
  
  void ServiceWebSocket();

  void SetRoomId(const std::string& room_id) { room_id_ = room_id; }

  void SetEmulationMode(bool is_emulation, bool is_sender);

  void SetNetInterface(std::string interface_name);

  void SetY4mPath(const std::string& path) { y4m_path_ = path; }

  // juheon added
  void SetHeadless(bool headless) { headless_ = headless; }

  // Signalling transport: scheme is "https" (default, no port in URL) or "http"
  // (local server; port is appended). Used to build the /join and /message URLs.
  void SetSignaling(const std::string& scheme, int port) {
    if (!scheme.empty())
      server_scheme_ = scheme;
    signaling_port_ = port;
  }

  void SetLogDirectory(const std::string& log_dir) { log_dir_ = log_dir; }

  void SetTrafficProfile(const std::string& path) { SetSctpTrafficProfile(path); }
  void SetSctpTrafficProfile(const std::string& path) { sctp_csv_path_ = path; }
  void SetRtpTrafficProfile(const std::string& path) { rtp_csv_path_ = path; }
  void SetRecordingPath(const std::string& path);
  void EnableRecording(bool enable);

  enum class TrafficKind {kKv, kMesh, kBulkTest, kControl};
  using PayloadHandler = std::function<void(absl::Span<const uint8_t>)>;

  bool AddSctpFlow(TrafficKind kind, const std::string& label, const webrtc::DataChannelInit& cfg);
  bool SendPayload(TrafficKind kind, absl::Span<const uint8_t> data);
  void RegisterPayloadHandler(TrafficKind kind, PayloadHandler handler);
  void ConfigureBufferedAmountLowCallback(
      TrafficKind kind,
      uint64_t threshold_bytes,
      std::function<void()> callback);

  bool IsFlowOpen(TrafficKind kind) const;
  uint64_t BufferedAmount(TrafficKind kind) const;
  size_t MaxSctpMessageSize(TrafficKind kind) const;
  rtc::Thread* signaling_thread() const { return signaling_thread_.get(); }

 protected:
  ~Conductor();
  bool InitializePeerConnection();
  bool ReinitializePeerConnectionForLoopback();
  bool CreatePeerConnection();
  void DeletePeerConnection();
  void EnsureStreamingUI();
  void AddTracks();
  void AddSCTPs();

  //
  // PeerConnectionObserver implementation.
  //

  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override {}
  void OnAddTrack(
      rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
      const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>&
          streams) override;
  void OnRemoveTrack(
      rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override;
  void OnDataChannel(
      rtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
  void OnRenegotiationNeeded() override {}
  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState new_state) override {}
  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
  void OnIceConnectionReceivingChange(bool receiving) override {}

  //
  // PeerConnectionClientObserver implementation.
  //

  void OnSignedIn() override;

  void OnDisconnected() override;

  void OnPeerConnected(int id, const std::string& name) override;

  void OnPeerDisconnected(int id) override;

  void OnMessageFromPeer(int peer_id, const std::string& message) override;

  void OnMessageSent(int err) override;

  void OnServerConnectionFailure() override;

  //
  // MainWndCallback implementation.
  //

  void StartLogin(const std::string& server, int port) override;

  void DisconnectFromServer() override;

  void ConnectToPeer(int peer_id) override;

  void DisconnectFromCurrentPeer() override;

  void StartBulkSctp() override;
  void StopBulkSctp() override;

  void UIThreadCallback(int msg_id, void* data) override;

  // CreateSessionDescriptionObserver implementation.
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
  void OnFailure(webrtc::RTCError error) override;

  std::string GetLogFolder() const override {return log_dir_;}
  void DumpActiveRtpParameters();

 protected:
  // Send a message to the remote peer.
  void SendMessage(const std::string& json_object);

  int peer_id_;
  bool loopback_;
  std::unique_ptr<rtc::Thread> signaling_thread_;
  webrtc::TaskQueueFactory* task_queue_factory_ = nullptr;
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
      peer_connection_factory_;

  // One flow = one channel + its observer + its handler.
  struct Flow {
    rtc::scoped_refptr<webrtc::DataChannelInterface> channel;
    std::unique_ptr<MyDataObserver> observer;
    PayloadHandler handler;  // nullable until user registers it
    std::string label;       // for debugging / remote mapping
    uint64_t buffered_low_threshold = 0;
    std::function<void()> buffered_low_callback;
  };

   // Map flows by kind
   std::unordered_map<TrafficKind, Flow> flows_;

   // Optional: map by lowercased label for remote-initiated channels
   std::unordered_map<std::string, TrafficKind> label2kind_ = {
       {"kv", TrafficKind::kKv},
       {"mesh", TrafficKind::kMesh},
       {"bulk", TrafficKind::kBulkTest},
       {"ctrl", TrafficKind::kControl}
   };

   // Bulk SCTP traffic helpers.
   std::unique_ptr<sctp::bulk::Sender> bulk_sender_;
   std::unique_ptr<sctp::bulk::Receiver> bulk_receiver_;
   std::vector<std::unique_ptr<sctp::file::Sender>> file_senders_;
   std::vector<std::unique_ptr<sctp::file::Receiver>> file_receivers_;

   /*
   rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel_;
   std::unique_ptr<MyDataObserver> data_observer_;

   std::optional<TrafficKind> payload_kind_;
   PayloadHandler payload_handler_;
   */

   PeerConnectionClient* client_;
   MainWindow* main_wnd_;
   std::deque<std::string*> pending_messages_;

  private:
   std::unique_ptr<WebSocketClient> ws_client_;

   void OnWebSocketMessage(const std::string& message);
   void OnWebSocketConnection(bool connected);

   Json::Value messages_;

   bool is_initiator_;
   bool peer_connected_ = false;

   std::string client_id_;
   std::string room_id_;
   std::string server_;
   std::string server_scheme_ = "https";
   int signaling_port_ = 0;
   Json::Value initial_messages_;

   std::string post_url_;  // For HTTP POST when initiator

   static bool curl_initialized_;
   CURL* curl_ = nullptr;
   static size_t WriteCallback(void* contents,
                               size_t size,
                               size_t nmemb,
                               void* userp);
   bool InitializeCurl();
   void CleanupCurl();
   std::string response_buffer_;

   std::string net_interface_;
   bool is_emulation_ = false;
   bool is_sender_ = true;
  std::string y4m_path_;
  std::string log_dir_;
  bool record_remote_video_ = false;
  std::string record_file_path_;
  rtc::scoped_refptr<webrtc::VideoTrackInterface> recorded_track_;
  std::unique_ptr<webrtc_example::RemoteMediaRecorder> remote_recorder_;

  std::string sctp_csv_path_;
  std::string rtp_csv_path_;
  std::vector<TrafficProfile> sctp_profiles_;
  std::optional<RtpTrafficConfig> rtp_config_;

  int EncoderMaxBitrateBps() const;
  int PeerConnectionMaxBitrateBps() const;
  int TargetFrameRateFps() const;

   // juheon added
   bool headless_ = false;
   bool logged_codecs_ = false;

   void StopStats();
   void GetReceiverVideoStats();

   std::unique_ptr<RTCStatsCollector> stats_collector_;

   using StatsCallback =
       std::function<void(StatsType type, const std::string& message)>;
   using RateCallback =
       std::function<void(double bitrate_bps, double framerate_fps)>;
   using ResolutionCallback = std::function<void(int width, int height)>;

   StatsCallback stats_callback_;
   RateCallback rate_callback_;
   ResolutionCallback resolution_callback_;

};

#endif  // EXAMPLES_PEERCONNECTION_CLIENT_CONDUCTOR_H_
