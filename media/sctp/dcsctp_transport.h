/*
 *  Copyright (c) 2021 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MEDIA_SCTP_DCSCTP_TRANSPORT_H_
#define MEDIA_SCTP_DCSCTP_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <atomic>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "absl/strings/string_view.h"
#include "api/array_view.h"
#include "api/environment/environment.h"
#include "pc/coordinator/bur_estimator.h"
#include "api/field_trials_view.h"
#include "api/priority.h"
#include "api/rtc_error.h"
#include "api/task_queue/task_queue_base.h"
#include "api/transport/data_channel_transport_interface.h"
#include "media/sctp/sctp_transport_internal.h"
#include "net/dcsctp/public/dcsctp_message.h"
#include "net/dcsctp/public/dcsctp_options.h"
#include "net/dcsctp/public/dcsctp_socket.h"
#include "net/dcsctp/public/dcsctp_socket_factory.h"
#include "net/dcsctp/public/timeout.h"
#include "net/dcsctp/public/types.h"
#include "net/dcsctp/timer/task_queue_timeout.h"
#include "p2p/base/packet_transport_internal.h"
#include "rtc_base/containers/flat_map.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/task_utils/repeating_task.h"
#include "rtc_base/network/received_packet.h"
#include "rtc_base/random.h"
#include "pc/rtp_sctp_coordinator.h"
#include "rtc_base/third_party/sigslot/sigslot.h"
#include "rtc_base/thread.h"
#include "rtc_base/thread_annotations.h"
#include "system_wrappers/include/clock.h"

namespace webrtc {

// Forward declaration
class RtpSctpCoordinator;

class DcSctpTransport : public cricket::SctpTransportInternal,
                        public dcsctp::DcSctpSocketCallbacks,
                        public sigslot::has_slots<> {
 public:
  DcSctpTransport(const Environment& env,
                  rtc::Thread* network_thread,
                  rtc::PacketTransportInternal* transport);
  DcSctpTransport(const Environment& env,
                  rtc::Thread* network_thread,
                  rtc::PacketTransportInternal* transport,
                  std::unique_ptr<dcsctp::DcSctpSocketFactory> socket_factory);
  ~DcSctpTransport() override;

  // cricket::SctpTransportInternal
  void SetOnConnectedCallback(std::function<void()> callback) override;
  void SetDataChannelSink(DataChannelSink* sink) override;
  void SetDtlsTransport(rtc::PacketTransportInternal* transport) override;
  bool Start(int local_sctp_port,
             int remote_sctp_port,
             int max_message_size) override;
  bool OpenStream(int sid, PriorityValue priority) override;
  bool ResetStream(int sid) override;
  RTCError SendData(int sid,
                    const SendDataParams& params,
                    const rtc::CopyOnWriteBuffer& payload) override;
  bool ReadyToSendData() override;
  int max_message_size() const override;
  std::optional<int> max_outbound_streams() const override;
  std::optional<int> max_inbound_streams() const override;
  size_t buffered_amount(int sid) const override;
  size_t buffered_amount_low_threshold(int sid) const override;
  void SetBufferedAmountLowThreshold(int sid, size_t bytes) override;
  void set_debug_name_for_testing(const char* debug_name) override;

  // Set BUR Coordinator for rate control
  void SetCoordinator(RtpSctpCoordinator* coordinator);

  // Dynamic stream priority (MAFS: called from any thread)
  void SetStreamPriority(int sid, uint16_t priority);

  // FSEv2: Override SCTP congestion window directly
  void SetFseCwnd(size_t cwnd_bytes, size_t ssthresh_bytes);

 private:
  // Apply pending stream priorities on network thread
  void ApplyPendingStreamPriorities();
  // dcsctp::DcSctpSocketCallbacks
  dcsctp::SendPacketStatus SendPacketWithStatus(
      rtc::ArrayView<const uint8_t> data) override;
  std::unique_ptr<dcsctp::Timeout> CreateTimeout(
      TaskQueueBase::DelayPrecision precision) override;
  dcsctp::TimeMs TimeMillis() override;
  uint32_t GetRandomInt(uint32_t low, uint32_t high) override;
  void OnTotalBufferedAmountLow() override;
  // Research hooks queried by the (research-modified) dcsctp socket for
  // T3-RTX suppression. Not virtual in this tree's DcSctpSocketCallbacks,
  // so they are plain methods here.
  bool HasPacingQueuedPackets() const;
  int64_t GetMicrosecondsSinceLastPacingSend() const;
  void OnBufferedAmountLow(dcsctp::StreamID stream_id) override;
  void OnMessageReceived(dcsctp::DcSctpMessage message) override;
  void OnError(dcsctp::ErrorKind error, absl::string_view message) override;
  void OnAborted(dcsctp::ErrorKind error, absl::string_view message) override;
  void OnConnected() override;
  void OnClosed() override;
  void OnConnectionRestarted() override;
  void OnStreamsResetFailed(
      rtc::ArrayView<const dcsctp::StreamID> outgoing_streams,
      absl::string_view reason) override;
  void OnStreamsResetPerformed(
      rtc::ArrayView<const dcsctp::StreamID> outgoing_streams) override;
  void OnIncomingStreamsReset(
      rtc::ArrayView<const dcsctp::StreamID> incoming_streams) override;

  // Transport callbacks
  void ConnectTransportSignals();
  void DisconnectTransportSignals();
  void OnTransportWritableState(rtc::PacketTransportInternal* transport);
  void OnTransportReadPacket(rtc::PacketTransportInternal* transport,
                             const rtc::ReceivedPacket& packet);
  void MaybeConnectSocket();

  rtc::Thread* const network_thread_;
  rtc::PacketTransportInternal* transport_;
  const Environment env_;
  Random random_;

  const std::unique_ptr<dcsctp::DcSctpSocketFactory> socket_factory_;
  dcsctp::TaskQueueTimeoutFactory task_queue_timeout_factory_;
  std::unique_ptr<dcsctp::DcSctpSocketInterface> socket_;
  std::string debug_name_;
  rtc::CopyOnWriteBuffer receive_buffer_;

  // Used to keep track of the state of data channels.
  // Reset needs to happen both ways before signaling the transport
  // is closed.
  struct StreamState {
    // True when the local connection has initiated the reset.
    // If a connection receives a reset for a stream that isn't
    // already being reset locally, it needs to fire the signal
    // SignalClosingProcedureStartedRemotely.
    bool closure_initiated = false;
    // True when the local connection received OnIncomingStreamsReset
    bool incoming_reset_done = false;
    // True when the local connection received OnStreamsResetPerformed
    bool outgoing_reset_done = false;
    // Priority of the stream according to RFC 8831, section 6.4
    dcsctp::StreamPriority priority =
        dcsctp::StreamPriority(PriorityValue(Priority::kLow).value());
  };

  // Map of all currently open or closing data channels
  flat_map<dcsctp::StreamID, StreamState> stream_states_
      RTC_GUARDED_BY(network_thread_);
  bool ready_to_send_data_ RTC_GUARDED_BY(network_thread_) = false;
  std::function<void()> on_connected_callback_ RTC_GUARDED_BY(network_thread_);
  DataChannelSink* data_channel_sink_ RTC_GUARDED_BY(network_thread_) = nullptr;

  dcsctp::DcSctpOptions CreateDcSctpOptions(int local_sctp_port,
                                            int remote_sctp_port,
                                            int max_message_size,
                                            const FieldTrialsView& field_trials);

  // ===== Pacing Members (Modified WebRTC Style) =====
  struct PacingQueueEntry {
    std::vector<uint8_t> data;
    int64_t enqueue_time_us = 0;  // For pacing delay measurement
  };
  std::deque<PacingQueueEntry> pacing_queue_;
  size_t pacing_queue_bytes_ = 0;
  int64_t pacing_rate_bps_ = 0;
  bool pacing_enabled_ = true;
  bool static_pacing_override_ = false;
  bool bur_initialized_ = false;  // Per-instance init flag (replaces static pacing_checked)

  // Coordinator for BUR-based pacing
  RtpSctpCoordinator* coordinator_ = nullptr;
  int64_t pacing_next_deadline_us_ = 0;
  int64_t pacing_debt_bytes_ = 0;
  int64_t pacing_last_process_us_ = 0;
  int64_t last_pacing_send_time_us_ = 0;  // For T3-RTX suppression grace period
  RepeatingTaskHandle pacing_drain_task_;

  dcsctp::SendPacketStatus SendPacketImmediate(
      rtc::ArrayView<const uint8_t> data);
  void MaybeSchedulePacingDrain(int64_t target_us, int64_t now_us);
  TimeDelta DrainPacingQueue();
  int64_t CalculateTransmitTimeUs(size_t bytes, int64_t rate_bps) const;

  // BUR-based rate control
  BurEstimator bur_estimator_;
  // bool bur_enabled_ = false;  // DISABLED for RTT testing

  // ===== BUR-based Pacing Control =====
  struct BurState {
    int64_t interval_start_ms = 0;     // Start of current measurement interval
    int64_t rtt_ref_us = -1;           // Reference RTT (previous interval's last RTT) - kept for logging
    int64_t rtt_min_us = -1;           // Minimum RTT observed (baseline for BUR)
    int64_t rtt_max_us = 0;            // Max RTT in current interval
    int64_t last_rtt_us = 0;           // Last RTT sample
    double current_bur = 0.0;          // Current BUR value
    int64_t last_rate_update_ms = 0;   // Last rate adjustment time
    bool first_interval = true;        // First interval flag
    int64_t last_throughput_bps = 0;   // Last measured throughput (for SPIKE mode)
    double smoothed_bur = 0.0;         // Exponentially smoothed BUR
  };
  BurState bur_state_;

  // BUR calculation and rate control
  double CalculateBur(int64_t L_us);
  void AdjustPacingRate(double bur);
  void OnBurIntervalComplete(int64_t now_ms, int64_t L_ms);

  // BUR pacing parameters (configurable via environment variables)
  // See config/bur_profiles.csv for profile definitions
  int64_t bur_interval_ms_ = 50;            // BUR_INTERVAL_MS (default 50ms)
  int64_t initial_pacing_rate_bps_ = 10'000'000;   // BUR_R_INIT_KBPS (default 10 Mbps)
  int64_t min_pacing_rate_bps_ = 1'000'000;        // BUR_MIN_RATE_KBPS (default 1 Mbps)
  int64_t max_pacing_rate_bps_ = 1'000'000'000;    // BUR_MAX_RATE_MBPS (default 1 Gbps)
  double bur_alpha_ = 0.85;                 // BUR_ALPHA (default 0.85)
  double bur_threshold_ = 0.85;             // BUR_THRESHOLD (default 0.85)
  double bur_spike_threshold_ = 1.0;        // BUR_SPIKE_THRESHOLD (default 1.0)
  double bur_max_mi_multiplier_ = 1.2;      // BUR_MAX_MI_MULTIPLIER (default 1.2)
  double bur_spike_reduction_ = 0.85;       // BUR_SPIKE_REDUCTION (default 0.85)

  void InitBurParameters();  // Initialize from environment variables

  // ===== Shared Memory Bandwidth Reader =====
  // Structure must match network_emulator.h SharedBandwidthData
  struct SharedBandwidthData {
    std::atomic<int64_t> timestamp_ms;
    std::atomic<double> bandwidth_kbps;
    std::atomic<double> latency_ms;
    std::atomic<bool> valid;
    std::atomic<uint64_t> sequence;
  };
  static constexpr const char* kBandwidthShmNameBase = "/webrtc_bandwidth_shm_";
  int shm_fd_ = -1;
  const SharedBandwidthData* shm_data_ = nullptr;
  std::string shm_name_;
  void InitBandwidthReader();
  void CleanupBandwidthReader();
  double GetAvailableBandwidthKbps();  // non-const: lazy init of shm

  // ===== Unified Metrics Logging =====
  // Owned Coordinator for BUR-based pacing (created if COORDINATOR_MODE=agentrtc)
  std::unique_ptr<RtpSctpCoordinator> owned_coordinator_;

  // ===== Dynamic BDP-based cwnd =====
  bool cwnd_dynamic_ = false;       // DCSCTP_CWND_DYNAMIC=1 enables BDP-based cwnd
  double bdp_multiplier_ = 3.0;     // BDP_MULTIPLIER (default 3.0, allows rtt_actual up to 3×rtt_min)

  // ===== MAFS Dynamic Stream Priority =====
  std::mutex pending_priority_mutex_;
  std::map<int, uint16_t> pending_stream_priorities_;
};

}  // namespace webrtc

#endif  // MEDIA_SCTP_DCSCTP_TRANSPORT_H_
