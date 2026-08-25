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

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "absl/base/nullability.h"
#include "api/data_channel_interface.h"
#include "api/environment/environment.h"
#include "api/jsep.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/rtp_receiver_interface.h"
#include "api/scoped_refptr.h"
#include "examples/peerconnection/client/main_wnd.h"
#include "json/value.h"
#include "examples/peerconnection/client/peer_connection_client.h"
#include "examples/peerconnection/client/websocket_client.h"
#include "examples/peerconnection/client/rtc_stats_collector.h"
#include "pc/coordinator/compute_emulation_integration.h"
#include "pc/rtp_sctp_coordinator.h"
#include "rtc_base/thread.h"

#if defined(ENABLE_LLAMA_INFERENCE)
#include "examples/peerconnection/client/llm_receiver_adapter.h"
#endif

namespace webrtc {
class VideoCaptureModule;
}  // namespace webrtc

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
    DEMO_CHAT_MESSAGE,
    SWITCH_TO_STREAMING_UI,
  };

  // Ported to older baseline: `absl_nonnull` qualifier not available in this
  // tree's abseil revision, dropped.
  Conductor(const webrtc::Environment& env,
            PeerConnectionClient* client,
            MainWindow* main_wnd);

  bool connection_active() const;

  void Close() override;

  // Write partial/final flow_completion.csv (public for signal handler)
  void WriteFlowCompletionCsv();

  // WebSocket signaling methods
  void ServiceWebSocket();
  void SetRoomId(const std::string& room_id) { room_id_ = room_id; }
  void SetServer(const std::string& server) { signaling_server_ = server; }
  void SetPort(int port) { signaling_port_ = port; }
  void SetIsSender(bool is_sender) { is_sender_ = is_sender; }
  void SetY4mPath(const std::string& path) { y4m_path_ = path; }
  void SetMaxBitrateKbps(int kbps) { max_bitrate_kbps_ = kbps; }
  void SetVideoFps(int fps) { video_fps_ = fps; }
  void SetLogDirectory(const std::string& log_dir) { log_dir_ = log_dir; }
  std::string GetLogDirectory() const { return log_dir_; }
  void SetTrafficConfig(const std::string& queries_csv) {
    queries_csv_path_ = queries_csv;
  }
  void SetDemoMode(bool demo) { demo_mode_ = demo; }
  void SetModelPath(const std::string& path) { model_path_ = path; }
  void SetContextPath(const std::string& path) { context_path_ = path; }
  void SetKVCachePath(const std::string& path) { kvcache_path_ = path; }
  void SetContextMethod(const std::string& method) { context_method_ = method; }
  void SetContextDir(const std::string& dir) { context_dir_ = dir; }
  void StartWebSocketSignaling();

 protected:
  ~Conductor() override;
  bool InitializePeerConnection();
  bool ReinitializePeerConnectionForLoopback();
  bool CreatePeerConnection();
  void DeletePeerConnection();
  void EnsureStreamingUI();
  void AddTracks();

  //
  // PeerConnectionObserver implementation.
  //

  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override {}
  void OnAddTrack(
      webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
      const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&
          streams) override;
  void OnRemoveTrack(
      webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override;
  void OnDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
  void OnRenegotiationNeeded() override {}
  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState new_state) override {}
  // Ported to older baseline: IceCandidate -> IceCandidateInterface, and
  // OnIceCandidateRemoved(single candidate) does not exist on this
  // PeerConnectionObserver (only OnIceCandidatesRemoved), so it was dropped.
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

  void UIThreadCallback(int msg_id, void* data) override;

  void OnQuerySubmitted(const std::string& query) override;

  // CreateSessionDescriptionObserver implementation.
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
  void OnFailure(webrtc::RTCError error) override;

 protected:
  // WebSocket callbacks
  void OnWebSocketMessage(const std::string& message);
  void OnWebSocketConnection(bool connected);
  void ConnectToRoom();

  // Send a message to the remote peer.
  void SendMessage(const std::string& json_object);

  int peer_id_;
  bool loopback_;
  const webrtc::Environment env_;
  std::unique_ptr<rtc::Thread> signaling_thread_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
      peer_connection_factory_;
  PeerConnectionClient* client_;
  MainWindow* main_wnd_;
  std::deque<std::string*> pending_messages_;
  std::string server_;
  // Holds a reference to the local video source so that its underlying
  // capturer is released only after the PeerConnection (and any tracks/senders)
  // have been torn down. This helps ensure the capturer is destroyed on the
  // same thread it was created on when DeletePeerConnection() runs.
  webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> local_video_source_;

  //
  // WebSocket signaling members
  //
  std::unique_ptr<WebSocketClient> ws_client_;
  std::string room_id_;
  std::string y4m_path_;
  int max_bitrate_kbps_ = 20000;  // 20 Mbps default
  int video_fps_ = 30;
  std::string log_dir_;
  std::unique_ptr<RTCStatsCollector> stats_collector_;
  std::string signaling_server_ = "goodsol.overlinkapp.org";
  int signaling_port_ = 443;
  std::string client_id_;
  bool is_sender_ = false;
  bool is_initiator_ = false;
  Json::Value initial_messages_;

  //
  // DataChannel throughput test members
  //
  webrtc::scoped_refptr<webrtc::DataChannelInterface> data_channel_;
  bool is_caller_ = false;
  bool datachannel_test_mode_ = false;
  bool rtp_sctp_mode_ = false;
  bool rtp_only_mode_ = false;
  // --with_audio: add an audio track on a synthetic device, own stream id.
  bool with_audio_ = false;
  // Apollo: owns the coordinator in RTP-only mode (no SCTP transport to own
  // it). Drives the Pudica/AgentRtc/FSE RTP-rate controller.
  std::unique_ptr<webrtc::RtpSctpCoordinator> rtp_only_coordinator_;
  int test_duration_sec_ = 10;

  // Receiver side throughput measurement
  std::atomic<uint64_t> bytes_received_{0};
  int64_t receive_start_time_ms_ = 0;
  int64_t last_report_time_ms_ = 0;
  uint64_t last_report_bytes_ = 0;
  bool transfer_started_ = false;
  bool transfer_finished_ = false;
  // Event-driven bulk send members
  std::atomic<bool> bulk_send_active_{false};
  std::vector<uint8_t> bulk_send_chunk_;
  int64_t bulk_send_start_time_ms_ = 0;
  int64_t bulk_send_end_time_ms_ = 0;
  std::atomic<uint64_t> bulk_send_total_sent_{0};
  int64_t bulk_send_last_stats_time_ms_ = 0;
  uint64_t bulk_send_last_bytes_sent_ = 0;
  std::unique_ptr<std::ofstream> bulk_send_csv_file_;
  
  // Internal DataChannel observer
  class DCObserver : public webrtc::DataChannelObserver {
   public:
    explicit DCObserver(Conductor* conductor) : conductor_(conductor) {}
    void OnStateChange() override;
    void OnMessage(const webrtc::DataBuffer& buffer) override;
    void OnBufferedAmountChange(uint64_t sent_data_size) override;
   private:
    Conductor* conductor_;
  };
  std::unique_ptr<DCObserver> dc_observer_;

  void CreateDataChannel();
  void StartBulkSend();
  void PumpData();  // Event-driven data pump
  void FinishBulkSend();  // Cleanup and report
  void PrintThroughputStats();

  //
  // MAFS Multi-flow members
  //

  // Per-flow DataChannel observer
  class FlowObserver : public webrtc::DataChannelObserver {
   public:
    FlowObserver(Conductor* conductor, int flow_id)
        : conductor_(conductor), flow_id_(flow_id) {}
    void OnStateChange() override;
    void OnMessage(const webrtc::DataBuffer& buffer) override;
    void OnBufferedAmountChange(uint64_t sent_data_size) override;
    void ClearRecvBuffer() { recv_buffer_.clear(); recv_buffer_.shrink_to_fit(); }
   private:
    Conductor* conductor_;
    int flow_id_;
    std::vector<uint8_t> recv_buffer_;  // Accumulates received data for real inference
  };

  // Sequential dataset entry (from hafs_sequence.csv)
  struct SequenceEntry {
    int sequence_id = 0;
    std::string query_path;          // "set_000", "set_001", ...
    int query_complete_delay_ms = 0; // receiver delay after compute
  };

  // Control channel observer (stream_id=0, bidirectional)
  class ControlObserver : public webrtc::DataChannelObserver {
   public:
    explicit ControlObserver(Conductor* c) : conductor_(c) {}
    void OnStateChange() override;
    void OnMessage(const webrtc::DataBuffer& buffer) override;
    void OnBufferedAmountChange(uint64_t) override {}
   private:
    Conductor* conductor_;
  };

  // Per-flow state
  struct SctpFlow {
    int flow_id = -1;
    int stream_id = -1;     // dcSCTP stream ID
    std::string label;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
    std::unique_ptr<FlowObserver> observer;
    size_t total_bytes = 0;
    double P_hat_ms = 0.0;
    double sigma_P_ms = 0.0;
    double parallel_compute_ms = 0.0;  // receiver parallel compute time (e.g. prefill)
    std::string rate_schedule_path;

    // Send state
    std::atomic<bool> send_active{false};
    std::atomic<uint64_t> bytes_sent{0};
    std::vector<uint8_t> chunk_buf;
    int64_t send_start_ms = 0;
    int64_t send_end_ms = 0;       // Time limit
    int64_t actual_end_ms = 0;     // Actual completion timestamp (0 = not done)
    int64_t effective_ms = 0;      // max(transfer_ms, parallel_compute_ms)
    uint32_t mafs_flow_id = 0;     // Registered MAFS flow ID
    bool channel_open = false;

    // Receiver-side tracking for compute emulation
    int64_t first_recv_ms = 0;
    int64_t last_recv_ms = 0;
    bool recv_complete = false;
    size_t context_tokens = 0;
    size_t decode_tokens = 0;          // predicted (for scheduling)
    size_t actual_decode_tokens = 0;   // actual (for compute)

    // V22: Real file support
    std::string data_format;           // "kv_cache", "raw_text", "" (emulation/dummy)
    std::string file_path;             // Path to actual file to send (optional)
    std::vector<uint8_t> file_data;    // Loaded file content for sending

    // Overlapped prefill: front token info for real GPU backends
    size_t front_context_tokens = 0;   // Number of front tokens prefilled locally
    std::string front_text_content;    // Raw text for front tokens (empty = emulation only)
  };
  std::vector<std::unique_ptr<SctpFlow>> sctp_flows_;
  std::string queries_csv_path_;

  // Multi-flow methods
  bool ParseTrafficConfig();
  void CreateMultiFlowDataChannels();
  void CreateReceiverFlowChannels();
  void StartMultiFlowSend();
  void PumpFlowData(int flow_id);
  void FinishFlowSend(int flow_id);
  void OnFlowChannelOpen(int flow_id);
  int CountOpenFlowChannels() const;
  bool multi_flow_send_started_ = false;

  // Control channel (stream_id=0, bidirectional)
  webrtc::scoped_refptr<webrtc::DataChannelInterface> control_channel_;
  std::unique_ptr<ControlObserver> control_observer_;

  // Sequential dataset
  std::vector<SequenceEntry> sequence_entries_;
  int current_set_index_ = 0;
  bool waiting_for_receiver_ = false;
  int total_queries_completed_ = 0;

  // READY gate: sender stalls the first flow send until the receiver reports
  // compute-path warmup done. Prevents the first real SPRV decode from paying
  // the Metal/CUDA JIT cost (tens of seconds on cold-cache MPSGraph builds).
  // Receiver-driven multi-query mode only — single-query runs bypass the gate.
  // Uses ready_received_ (latch) so either arrival order is safe:
  //   - channels open first → OnFlowChannelOpen sees !ready_received_, stalls.
  //   - READY arrives first  → OnControlMessage latches, OnFlowChannelOpen
  //     starts the send immediately when the last channel opens.
  bool ready_received_ = false;        // Sender: latched true when READY arrives
  bool compute_ready_ = false;         // Receiver: true once compute_emulation_ init returned
  bool receiver_ready_sent_ = false;   // Receiver: one-shot guard for READY emission

  // Backward-compatible sequence control (from hafs_sequence.csv)
  int start_delay_ms_ = 0;          // Delay before first send
  int repeat_count_ = 1;            // How many times to send (0 = infinite until duration)
  int query_complete_delay_ms_ = 0; // Delay between repeats after completion
  int current_repeat_ = 0;          // Current repeat iteration

  // MAX_QUERIES env var: when >0, the process exits gracefully after this
  // many receiver-driven query completions (counts total_queries_completed_).
  // Activates receiver-driven mode even with a single sequence entry so that
  // a CSV with `repeat_count=1` can run N queries end-to-end under the
  // compute-aware path (waits for real receiver-side compute between runs).
  int max_queries_ = 0;

  // Sender-side monotonic query iteration counter. Incremented each time
  // a new query starts (initial or via NEXT / RestartFlowSend). Used as the
  // `query_id` column in flow_completion.csv since total_queries_completed_
  // is only bumped on the receiver.
  int sender_query_iteration_ = 0;

  // flow_completion.csv append-mode tracking: trunc+header on the first
  // flush, append on subsequent query boundaries. -1 means "nothing
  // written yet".
  int last_flushed_query_iteration_ = -1;
  bool flow_completion_header_written_ = false;

  void RestartFlowSend();  // Reset flows and resend for repeat (legacy)

  // Receiver-driven control channel methods
  void CreateControlChannel();
  void SendControlMessage(const std::string& msg);
  void OnControlMessage(const std::string& msg);
  bool LoadSetByIndex(int set_index);
  void ResetFlowsForNewSet();
  void OnReceiverQueryComplete(uint64_t query_id);

  // PumpFlowData timer fallback: 200ms periodic check to recover stalled callbacks
  std::atomic<bool> pump_timer_active_{false};
  void SchedulePumpTimer();

  // Conservative time-burst mode: pace at CONSERVATIVE_BURST_RATE_MBPS for
  // CONSERVATIVE_BURST_SEC seconds, then idle for CONSERVATIVE_IDLE_SEC, repeat.
  // Bounded duty cycle = burst_sec / (burst_sec + idle_sec).
  bool conservative_burst_enabled_ = false;
  double conservative_burst_sec_ = 2.0;         // burst duration (seconds)
  double conservative_idle_sec_ = 5.0;          // idle gap (seconds)
  double conservative_burst_rate_mbps_ = 100.0; // pacing rate within burst
  int64_t burst_bytes_sent_ = 0;                // bytes in current burst (pacing budget)
  bool burst_idle_active_ = false;              // true = currently in idle phase
  int64_t burst_idle_start_ms_ = 0;             // epoch when idle began
  int64_t burst_start_ms_ = 0;                  // epoch when current burst began (0 = not yet)
  int64_t burst_idle_gen_ = 0;                  // increments each idle entry; stale threads abort
  void InitConservativeBurstMode();
  bool IsConservativeBurstOpen();               // false = in idle, skip pumping
  bool IsConservativePaceOpen();                // false = over in-burst rate budget, skip pumping
  void OnBurstBytesSent(size_t bytes);          // accumulate pacing budget
  void ScheduleBurstResume();                   // timer to resume after idle

  // Compute emulation (receiver-side only)
  std::unique_ptr<webrtc::coordinator::ComputeEmulationIntegration> compute_emulation_;
  size_t final_decode_tokens_ = 0;

  // Demo mode members
  //
  bool demo_mode_ = false;
  std::string model_path_;

  // Control DataChannel (negotiated, SCTP id=99) — demo mode
  class ControlDCObserver : public webrtc::DataChannelObserver {
   public:
    explicit ControlDCObserver(Conductor* conductor) : conductor_(conductor) {}
    void OnStateChange() override;
    void OnMessage(const webrtc::DataBuffer& buffer) override;
   private:
    Conductor* conductor_;
  };
  std::unique_ptr<ControlDCObserver> control_dc_observer_;
  void SendControlMessage(const Json::Value& msg);

  // Context data channel (negotiated, SCTP id=100) for bulk binary transfer
  webrtc::scoped_refptr<webrtc::DataChannelInterface> context_data_channel_;
  class ContextDataObserver : public webrtc::DataChannelObserver {
   public:
    explicit ContextDataObserver(Conductor* conductor)
        : conductor_(conductor) {}
    void OnStateChange() override;
    void OnMessage(const webrtc::DataBuffer& buffer) override;
   private:
    Conductor* conductor_;
  };
  std::unique_ptr<ContextDataObserver> context_data_observer_;

  // Sender-side context storage
  std::string context_path_;
  std::string kvcache_path_;
  std::string context_dir_;               // Directory with 00.txt, 01.txt, 02.txt
  std::vector<std::string> context_texts_;  // Loaded from context_dir_
  std::string context_text_;              // Loaded from --context_path
  std::vector<uint8_t> kvcache_data_;     // Loaded from --kvcache_path
  std::vector<uint8_t> kvcache_compressed_;  // zstd-compressed KV cache (kvzip)
  std::string context_method_;            // "raw_text", "kv_cache", or "kvzip"
  void LoadDemoContext();

  // Receiver-side accumulation
  std::vector<uint8_t> recv_context_buffer_;
  size_t recv_expected_size_ = 0;     // Expected bytes from context_meta
  std::string pending_query_;
  bool context_done_received_ = false; // True when context_done arrived
  int64_t transfer_start_ms_ = 0;
  int64_t query_start_ms_ = 0;       // When query was submitted (for total E2E)
  int64_t inference_start_ms_ = 0;   // When inference began (for TTFT)
  void ProcessReceivedContext(const std::string& query);
  void TryProcessPendingContext();    // Check if all data arrived

#if defined(ENABLE_LLAMA_INFERENCE)
  std::unique_ptr<LlmReceiverAdapter> llm_adapter_;
  void InitializeLlmAdapter();
#endif
};

#endif  // EXAMPLES_PEERCONNECTION_CLIENT_CONDUCTOR_H_
