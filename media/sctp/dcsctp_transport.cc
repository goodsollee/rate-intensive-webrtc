/*
 *  Copyright 2021 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "media/sctp/dcsctp_transport.h"
#include "media/sctp/dcsctp_debug.h"
#include <iostream>
#include <fstream>
#include <iomanip>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "api/array_view.h"
#include "api/data_channel_interface.h"
#include "api/dtls_transport_interface.h"
#include "api/environment/environment.h"
#include "api/field_trials_view.h"
#include "api/priority.h"
#include "api/rtc_error.h"
#include "api/sctp_transport_interface.h"
#include "api/sequence_checker.h"
#include "api/task_queue/task_queue_base.h"
#include "api/transport/data_channel_transport_interface.h"
#include "net/dcsctp/public/dcsctp_message.h"
#include "net/dcsctp/public/dcsctp_options.h"
#include "net/dcsctp/public/dcsctp_socket.h"
#include "net/dcsctp/public/dcsctp_socket_factory.h"
#include "net/dcsctp/public/packet_observer.h"
#include "net/dcsctp/public/text_pcap_packet_observer.h"
#include "net/dcsctp/public/timeout.h"
#include "net/dcsctp/public/types.h"
#include "p2p/base/packet_transport_internal.h"
#include "p2p/dtls/dtls_transport_internal.h"
#include "rtc_base/async_packet_socket.h"
#include "rtc_base/checks.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/logging.h"

namespace {
// Helper to read environment variables with default values
double ReadEnvDouble(const char* name, double default_value) {
  const char* env = std::getenv(name);
  if (env) {
    char* end;
    double val = std::strtod(env, &end);
    if (end != env && *end == '\0') {
      return val;
    }
  }
  return default_value;
}

int64_t ReadEnvInt64(const char* name, int64_t default_value) {
  const char* env = std::getenv(name);
  if (env) {
    char* end;
    long long val = std::strtoll(env, &end, 10);
    if (end != env && *end == '\0') {
      return static_cast<int64_t>(val);
    }
  }
  return default_value;
}
}  // namespace
#include "rtc_base/network/received_packet.h"
#include "rtc_base/random.h"
#include "rtc_base/socket.h"
#include "rtc_base/strings/string_builder.h"
#include "rtc_base/thread.h"
#include "rtc_base/trace_event.h"
#include "system_wrappers/include/clock.h"
#include "pc/rtp_sctp_coordinator.h"

namespace webrtc {

namespace {
using ::dcsctp::SendPacketStatus;

// When there is packet loss for a long time, the SCTP retry timers will use
// exponential backoff, which can grow to very long durations and when the
// connection recovers, it may take a long time to reach the new backoff
// duration. By limiting it to a reasonable limit, the time to recover reduces.
constexpr dcsctp::DurationMs kMaxTimerBackoffDuration =
    dcsctp::DurationMs(3000);

enum class WebrtcPPID : dcsctp::PPID::UnderlyingType {
  // https://www.rfc-editor.org/rfc/rfc8832.html#section-8.1
  kDCEP = 50,
  // https://www.rfc-editor.org/rfc/rfc8831.html#section-8
  kString = 51,
  kBinaryPartial = 52,  // Deprecated
  kBinary = 53,
  kStringPartial = 54,  // Deprecated
  kStringEmpty = 56,
  kBinaryEmpty = 57,
};

WebrtcPPID ToPPID(DataMessageType message_type, size_t size) {
  switch (message_type) {
    case DataMessageType::kControl:
      return WebrtcPPID::kDCEP;
    case DataMessageType::kText:
      return size > 0 ? WebrtcPPID::kString : WebrtcPPID::kStringEmpty;
    case DataMessageType::kBinary:
      return size > 0 ? WebrtcPPID::kBinary : WebrtcPPID::kBinaryEmpty;
  }
}

std::optional<DataMessageType> ToDataMessageType(dcsctp::PPID ppid) {
  switch (static_cast<WebrtcPPID>(ppid.value())) {
    case WebrtcPPID::kDCEP:
      return DataMessageType::kControl;
    case WebrtcPPID::kString:
    case WebrtcPPID::kStringPartial:
    case WebrtcPPID::kStringEmpty:
      return DataMessageType::kText;
    case WebrtcPPID::kBinary:
    case WebrtcPPID::kBinaryPartial:
    case WebrtcPPID::kBinaryEmpty:
      return DataMessageType::kBinary;
  }
  return std::nullopt;
}

std::optional<SctpErrorCauseCode> ToErrorCauseCode(dcsctp::ErrorKind error) {
  switch (error) {
    case dcsctp::ErrorKind::kParseFailed:
      return SctpErrorCauseCode::kUnrecognizedParameters;
    case dcsctp::ErrorKind::kPeerReported:
      return SctpErrorCauseCode::kUserInitiatedAbort;
    case dcsctp::ErrorKind::kWrongSequence:
    case dcsctp::ErrorKind::kProtocolViolation:
      return SctpErrorCauseCode::kProtocolViolation;
    case dcsctp::ErrorKind::kResourceExhaustion:
      return SctpErrorCauseCode::kOutOfResource;
    case dcsctp::ErrorKind::kTooManyRetries:
    case dcsctp::ErrorKind::kUnsupportedOperation:
    case dcsctp::ErrorKind::kNoError:
    case dcsctp::ErrorKind::kNotConnected:
      // No SCTP error cause code matches those
      break;
  }
  return std::nullopt;
}

bool IsEmptyPPID(dcsctp::PPID ppid) {
  WebrtcPPID webrtc_ppid = static_cast<WebrtcPPID>(ppid.value());
  return webrtc_ppid == WebrtcPPID::kStringEmpty ||
         webrtc_ppid == WebrtcPPID::kBinaryEmpty;
}

std::string GetDebugName() {
  static std::atomic<int> instance_count = 0;
  StringBuilder sb;
  sb << "DcSctpTransport" << instance_count++;
  return sb.Release();
}

}  // namespace

DcSctpTransport::DcSctpTransport(const Environment& env,
                                 Thread* network_thread,
                                 DtlsTransportInternal* transport)
    : DcSctpTransport(env,
                      network_thread,
                      transport,
                      std::make_unique<dcsctp::DcSctpSocketFactory>()) {}

DcSctpTransport::DcSctpTransport(
    const Environment& env,
    Thread* network_thread,
    DtlsTransportInternal* transport,
    std::unique_ptr<dcsctp::DcSctpSocketFactory> socket_factory)
    : network_thread_(network_thread),
      transport_(transport),
      env_(env),
      random_(env_.clock().TimeInMicroseconds()),
      socket_factory_(std::move(socket_factory)),
      task_queue_timeout_factory_(
          *network_thread,
          [this]() { return TimeMillis(); },
          [this](dcsctp::TimeoutID timeout_id) {
            socket_->HandleTimeout(timeout_id);
          }),
      debug_name_(GetDebugName()) {
  RTC_DCHECK_RUN_ON(network_thread_);
  ConnectTransportSignals();
}

DcSctpTransport::~DcSctpTransport() {
  CleanupBandwidthReader();
  if (socket_) {
    socket_->Close();
  }
}

void DcSctpTransport::SetOnConnectedCallback(std::function<void()> callback) {
  RTC_DCHECK_RUN_ON(network_thread_);
  on_connected_callback_ = std::move(callback);
}

void DcSctpTransport::SetDataChannelSink(DataChannelSink* sink) {
  RTC_DCHECK_RUN_ON(network_thread_);
  data_channel_sink_ = sink;
  if (data_channel_sink_ && ready_to_send_data_) {
    data_channel_sink_->OnReadyToSend();
  }
}

DtlsTransportInternal* DcSctpTransport::dtls_transport() const {
  return transport_;
}

bool DcSctpTransport::Start(const SctpOptions& options) {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_DCHECK(options.max_message_size > 0);
  RTC_DLOG(LS_INFO) << debug_name_ << "->Start(local=" << options.local_port
                    << ", remote=" << options.remote_port
                    << ", max_message_size=" << options.max_message_size
                    << ", local_init="
                    << (options.local_init.has_value() ? "(set)" : "(not set)")
                    << ", remote_init="
                    << (options.remote_init.has_value() ? "(set)" : "(not set)")
                    << ")";

  if (!socket_) {
    dcsctp::DcSctpOptions dcsctp_options =
        CreateDcSctpOptions(options, env_.field_trials());

    // Initialize dynamic cwnd members from environment
    if (const char* dyn_env = std::getenv("DCSCTP_CWND_DYNAMIC")) {
      cwnd_dynamic_ = (std::string(dyn_env) == "1");
    }
    if (cwnd_dynamic_) {
      if (const char* mult = std::getenv("BDP_MULTIPLIER")) {
        bdp_multiplier_ = std::stod(mult);
      }
      RTC_LOG(LS_INFO) << "[SCTP] Dynamic cwnd: multiplier=" << bdp_multiplier_;
    }

    if (options.local_init.has_value()) {
      local_init_ = *options.local_init;
    }
    if (options.remote_init.has_value()) {
      remote_init_ = *options.remote_init;
    }
    std::unique_ptr<dcsctp::PacketObserver> packet_observer;
    if (RTC_LOG_CHECK_LEVEL(LS_VERBOSE)) {
      packet_observer =
          std::make_unique<dcsctp::TextPcapPacketObserver>(debug_name_);
    }

    socket_ = socket_factory_->Create(
        debug_name_, *this, std::move(packet_observer), dcsctp_options);
    
    // Wire SACK observer: pass all info to coordinator for TSN-based BUR
    socket_->SetOnSackReceived(
        [this](const dcsctp::SackInfo& sack_info) {
          if (coordinator_) {
            int64_t now_us = env_.clock().TimeInMicroseconds();
            // Update cwnd/srtt BEFORE OnSackReceived so FSE reads fresh
            // CC-updated values (not stale from previous cycle)
            if (socket_) {
              auto metrics = socket_->GetMetrics();
              if (metrics.has_value()) {
                coordinator_->SetCwnd(static_cast<int64_t>(metrics->cwnd_bytes));
                coordinator_->SetSrtt(static_cast<int64_t>(metrics->srtt_ms));
                coordinator_->SetPeerRwnd(static_cast<int64_t>(metrics->peer_rwnd_bytes));
                coordinator_->SetUnackedBytes(static_cast<int64_t>(metrics->unacked_bytes));
                // Pass emulator bandwidth for link_utilization in unified_metrics.csv
                double bw = GetAvailableBandwidthKbps();
                if (bw > 0) {
                  coordinator_->SetAvailableBandwidth(static_cast<int64_t>(bw));
                }
              }
            }
            coordinator_->OnSackReceived(
                sack_info.cumulative_tsn_ack,
                sack_info.rtt_us,
                static_cast<int64_t>(sack_info.bytes_acked),
                sack_info.has_packet_loss,
                now_us);
            // Update local pacing rate (unless static override is active)
            if (!static_pacing_override_) {
              pacing_rate_bps_ = coordinator_->GetPacingRate();
            }

            // Dynamic cwnd: sized to pacing_rate × rtt_min so cwnd is never
            // the throughput bottleneck — pacing is the sole rate limiter.
            // cwnd = gain × pacing_rate × RTprop / 8.
            if (cwnd_dynamic_ && coordinator_) {
              int64_t rtt_min_us = coordinator_->GetRttMinUs();
              if (rtt_min_us > 0 && pacing_rate_bps_ > 0) {
                size_t bdp_cwnd = static_cast<size_t>(
                    bdp_multiplier_ * pacing_rate_bps_ * (rtt_min_us / 1e6) / 8.0);
                if (bdp_cwnd > 0) {
                  socket_->SetCwnd(bdp_cwnd);
                }
              }
            }
          }
        });
  } else {
    if (options.local_port != socket_->options().local_port ||
        options.remote_port != socket_->options().remote_port) {
      RTC_LOG(LS_ERROR)
          << debug_name_ << "->Start(local=" << options.local_port
          << ", remote=" << options.remote_port
          << "): Can't change ports on already started transport.";
      return false;
    }
    if (options.local_init != local_init_ ||
        options.remote_init != remote_init_) {
      RTC_LOG(LS_ERROR)
          << debug_name_ << "->Start("
          << "local_init="
          << (options.local_init.has_value() ? "(set)" : "(not set)")
          << ", remote_init="
          << (options.remote_init.has_value() ? "(set)" : "(not set)")
          << "): Can't change sctp-init on already started transport.";
      return false;
    }
    socket_->SetMaxMessageSize(options.max_message_size);
  }

  MaybeConnectSocket();

  for (const auto& [sid, stream_state] : stream_states_) {
    socket_->SetStreamPriority(sid, stream_state.priority);
  }

  return true;
}

bool DcSctpTransport::OpenStream(int sid, PriorityValue priority) {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_DLOG(LS_INFO) << debug_name_ << "->OpenStream(" << sid << ", "
                    << priority.value() << ").";

  StreamState stream_state;
  stream_state.priority = dcsctp::StreamPriority(priority.value());
  stream_states_.insert_or_assign(dcsctp::StreamID(static_cast<uint16_t>(sid)),
                                  stream_state);
  if (socket_) {
    socket_->SetStreamPriority(dcsctp::StreamID(sid),
                               dcsctp::StreamPriority(priority.value()));
  }

  return true;
}

void DcSctpTransport::SetStreamPriority(int sid, uint16_t priority) {
  // Thread-safe: can be called from any thread (e.g., MAFS coordinator thread).
  // Queues the priority change; applied on next DrainPacingQueue() call.
  std::lock_guard<std::mutex> lock(pending_priority_mutex_);
  pending_stream_priorities_[sid] = priority;
}

void DcSctpTransport::SetFseCwnd(size_t cwnd_bytes, size_t ssthresh_bytes) {
  if (socket_) {
    socket_->SetCwnd(cwnd_bytes);
    socket_->SetSsthresh(ssthresh_bytes);
  }
}

void DcSctpTransport::ApplyPendingStreamPriorities() {
  RTC_DCHECK_RUN_ON(network_thread_);
  std::map<int, uint16_t> priorities;
  {
    std::lock_guard<std::mutex> lock(pending_priority_mutex_);
    if (pending_stream_priorities_.empty()) return;
    priorities.swap(pending_stream_priorities_);
  }

  for (const auto& [sid, priority] : priorities) {
    auto stream_id = dcsctp::StreamID(static_cast<uint16_t>(sid));
    auto it = stream_states_.find(stream_id);
    if (it != stream_states_.end()) {
      it->second.priority = dcsctp::StreamPriority(priority);
    }
    if (socket_) {
      socket_->SetStreamPriority(stream_id, dcsctp::StreamPriority(priority));
      RTC_LOG(LS_INFO) << "[MAFS] Applied priority: stream=" << sid
                       << " priority=" << priority;
    }
  }
}

bool DcSctpTransport::ResetStream(int sid) {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_DLOG(LS_INFO) << debug_name_ << "->ResetStream(" << sid << ").";
  if (!socket_) {
    RTC_LOG(LS_ERROR) << debug_name_ << "->ResetStream(sid=" << sid
                      << "): Transport is not started.";
    return false;
  }

  dcsctp::StreamID streams[1] = {dcsctp::StreamID(static_cast<uint16_t>(sid))};

  auto it = stream_states_.find(streams[0]);
  if (it == stream_states_.end()) {
    RTC_LOG(LS_ERROR) << debug_name_ << "->ResetStream(sid=" << sid
                      << "): Stream is not open.";
    return false;
  }

  StreamState& stream_state = it->second;
  if (stream_state.closure_initiated || stream_state.incoming_reset_done ||
      stream_state.outgoing_reset_done) {
    // The closing procedure was already initiated by the remote, don't do
    // anything.
    return false;
  }
  stream_state.closure_initiated = true;
  socket_->ResetStreams(streams);
  return true;
}

RTCError DcSctpTransport::SendData(int sid,
                                   const SendDataParams& params,
                                   const CopyOnWriteBuffer& payload) {
  RTC_DCHECK_RUN_ON(network_thread_);

  // Apply any pending MAFS stream priority changes before sending
  ApplyPendingStreamPriorities();

  RTC_DLOG(LS_VERBOSE) << debug_name_ << "->SendData(sid=" << sid
                       << ", type=" << static_cast<int>(params.type)
                       << ", length=" << payload.size() << ").";

  if (!socket_) {
    RTC_LOG(LS_ERROR) << debug_name_
                      << "->SendData(...): Transport is not started.";
    return RTCError(RTCErrorType::INVALID_STATE);
  }

  // It is possible for a message to be sent from the signaling thread at the
  // same time a data-channel is closing, but before the signaling thread is
  // aware of it. So we need to keep track of currently active data channels and
  // skip sending messages for the ones that are not open or closing.
  // The sending errors are not impacting the data channel API contract as
  // it is allowed to discard queued messages when the channel is closing.
  auto stream_state =
      stream_states_.find(dcsctp::StreamID(static_cast<uint16_t>(sid)));
  if (stream_state == stream_states_.end()) {
    RTC_LOG(LS_VERBOSE) << "Skipping message on non-open stream with sid: "
                        << sid;
    return RTCError(RTCErrorType::INVALID_STATE);
  }

  if (stream_state->second.closure_initiated ||
      stream_state->second.incoming_reset_done ||
      stream_state->second.outgoing_reset_done) {
    RTC_LOG(LS_VERBOSE) << "Skipping message on closing stream with sid: "
                        << sid;
    return RTCError(RTCErrorType::INVALID_STATE);
  }

  auto max_message_size = socket_->options().max_message_size;
  if (max_message_size > 0 && payload.size() > max_message_size) {
    RTC_LOG(LS_WARNING) << debug_name_
                        << "->SendData(...): "
                           "Trying to send packet bigger "
                           "than the max message size: "
                        << payload.size() << " vs max of " << max_message_size;
    return RTCError(RTCErrorType::INVALID_RANGE);
  }

  std::vector<uint8_t> message_payload(payload.cdata(),
                                       payload.cdata() + payload.size());
  if (message_payload.empty()) {
    // https://www.rfc-editor.org/rfc/rfc8831.html#section-6.6
    // SCTP does not support the sending of empty user messages. Therefore, if
    // an empty message has to be sent, the appropriate PPID (WebRTC String
    // Empty or WebRTC Binary Empty) is used, and the SCTP user message of one
    // zero byte is sent.
    message_payload.push_back('\0');
  }

  dcsctp::DcSctpMessage message(
      dcsctp::StreamID(static_cast<uint16_t>(sid)),
      dcsctp::PPID(static_cast<uint16_t>(ToPPID(params.type, payload.size()))),
      std::move(message_payload));

  dcsctp::SendOptions send_options;
  send_options.unordered = dcsctp::IsUnordered(!params.ordered);
  if (params.max_rtx_ms.has_value()) {
    RTC_DCHECK(*params.max_rtx_ms >= 0 &&
               *params.max_rtx_ms <= std::numeric_limits<uint16_t>::max());
    send_options.lifetime = dcsctp::DurationMs(*params.max_rtx_ms);
  }
  if (params.max_rtx_count.has_value()) {
    RTC_DCHECK(*params.max_rtx_count >= 0 &&
               *params.max_rtx_count <= std::numeric_limits<uint16_t>::max());
    send_options.max_retransmissions = *params.max_rtx_count;
  }

  dcsctp::SendStatus error = socket_->Send(std::move(message), send_options);
  switch (error) {
    case dcsctp::SendStatus::kSuccess:
      return RTCError::OK();
    case dcsctp::SendStatus::kErrorResourceExhaustion:
      ready_to_send_data_ = false;
      return RTCError(RTCErrorType::RESOURCE_EXHAUSTED);
    default:
      absl::string_view error_message = dcsctp::ToString(error);
      RTC_LOG(LS_ERROR) << debug_name_
                        << "->SendData(...): send() failed with error "
                        << error_message << ".";
      return RTCError(RTCErrorType::NETWORK_ERROR, error_message);
  }
}

bool DcSctpTransport::ReadyToSendData() {
  RTC_DCHECK_RUN_ON(network_thread_);
  return ready_to_send_data_;
}

int DcSctpTransport::max_message_size() const {
  if (!socket_) {
    RTC_LOG(LS_ERROR) << debug_name_
                      << "->max_message_size(...): Transport is not started.";
    return 0;
  }
  return socket_->options().max_message_size;
}

std::optional<int> DcSctpTransport::max_outbound_streams() const {
  if (!socket_ || !socket_->GetMetrics().has_value()) {
    return std::nullopt;
  }
  return socket_->GetMetrics()->negotiated_maximum_outgoing_streams;
}

std::optional<int> DcSctpTransport::max_inbound_streams() const {
  if (!socket_ || !socket_->GetMetrics().has_value()) {
    return std::nullopt;
  }
  return socket_->GetMetrics()->negotiated_maximum_incoming_streams;
}

size_t DcSctpTransport::buffered_amount(int sid) const {
  if (!socket_)
    return 0;
  return socket_->buffered_amount(dcsctp::StreamID(sid));
}

size_t DcSctpTransport::buffered_amount_low_threshold(int sid) const {
  if (!socket_)
    return 0;
  return socket_->buffered_amount_low_threshold(dcsctp::StreamID(sid));
}

void DcSctpTransport::SetBufferedAmountLowThreshold(int sid, size_t bytes) {
  if (!socket_)
    return;
  socket_->SetBufferedAmountLowThreshold(dcsctp::StreamID(sid), bytes);
}

SendPacketStatus DcSctpTransport::SendPacketWithStatus(
    ArrayView<const uint8_t> data) {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_DCHECK(socket_);

  // Per-instance initialization: create coordinator + determine pacing mode
  if (!bur_initialized_) {
    bur_initialized_ = true;
    InitBurParameters();

    // Static rate override (e.g., PACING_RATE_MBPS=300 for fixed-rate tests)
    // When set, coordinator cannot change pacing rate — useful for diagnosing
    // whether the pacer actually controls throughput.
    const char* pacing_env = std::getenv("PACING_RATE_MBPS");
    if (pacing_env) {
      int64_t rate_mbps = std::atoll(pacing_env);
      if (rate_mbps > 0 && rate_mbps < 10000) {
        pacing_rate_bps_ = rate_mbps * 1000000;
        static_pacing_override_ = true;
        pacing_enabled_ = true;
        RTC_LOG(LS_WARNING) << "[PACING] Static rate override: " << rate_mbps
                            << " Mbps (coordinator updates BLOCKED)";
      }
    }

    // Pacing is ONLY for AgentRTC (HAFS) mode — BUR-based dynamic pacing.
    // NC (disabled) and FSE modes use pure dcsctp cwnd CC without pacing.
    // FSE manages SCTP rate via SetFseCwnd() directly, not through pacing.
    if (static_pacing_override_) {
      RTC_LOG(LS_WARNING) << "[PACING] Static override active at "
                          << (pacing_rate_bps_ / 1000000) << " Mbps";
    } else if (coordinator_ &&
        (coordinator_->GetMode() == CoordinatorMode::kAgentRtc ||
         coordinator_->GetMode() == CoordinatorMode::kPudica)) {
      pacing_rate_bps_ = coordinator_->GetPacingRate();
      RTC_LOG(LS_WARNING) << "[PACING] AgentRTC mode: pacing at "
                          << (pacing_rate_bps_ / 1000000) << " Mbps";
    } else {
      pacing_enabled_ = false;
      RTC_LOG(LS_WARNING) << "[PACING] No pacing (mode="
                          << (coordinator_ ? static_cast<int>(coordinator_->GetMode()) : -1)
                          << "): pure dcsctp cwnd CC";
    }
  }

  // If pacing disabled or not connected, send immediately
  if (!pacing_enabled_ || !socket_ ||
      socket_->state() != dcsctp::SocketState::kConnected) {
    return SendPacketImmediate(data);
  }

  // ===== MODIFIED WEBRTC STYLE PACING =====
  // Design: Queue ALL packets in FIFO order. Never drop, never return failure.
  // Let dcsctp's cwnd naturally bound the queue.
  
  int64_t now_us = env_.clock().TimeInMicroseconds();

  // Enqueue packet - pure FIFO, no limits
  PacingQueueEntry entry;
  entry.data = std::vector<uint8_t>(data.begin(), data.end());
  entry.enqueue_time_us = now_us;  // Record enqueue time for pacing delay measurement

  pacing_queue_.push_back(std::move(entry));
  pacing_queue_bytes_ += data.size();

  // Schedule timer if not running
  if (!pacing_drain_task_.Running()) {
    int64_t interval_us = CalculateTransmitTimeUs(data.size(), pacing_rate_bps_);
    // NO min 1ms - use actual calculated interval (even if < 1ms)
    pacing_next_deadline_us_ = now_us + interval_us;
    MaybeSchedulePacingDrain(pacing_next_deadline_us_, now_us);
  }

  // CRITICAL: Always return success - never kTemporaryFailure
  return SendPacketStatus::kSuccess;
}

// Actual packet transmission
dcsctp::SendPacketStatus DcSctpTransport::SendPacketImmediate(
    ArrayView<const uint8_t> data) {
  RTC_DCHECK_RUN_ON(network_thread_);

  // Periodic SCTP metrics dump (every 500ms) for cwnd/retx analysis
  {
    static int64_t last_dump_ms = 0;
    static size_t last_tx = 0, last_rtx = 0;
    static uint64_t last_rtx_bytes = 0;
    int64_t now_ms = env_.clock().TimeInMilliseconds();
    if (now_ms - last_dump_ms >= 500 && socket_) {
      auto m = socket_->GetMetrics();
      if (m.has_value()) {
        size_t dtx = m->tx_packets_count - last_tx;
        size_t drtx = m->rtx_packets_count - last_rtx;
        uint64_t drtx_b = m->rtx_bytes_count - last_rtx_bytes;
        int64_t dt = now_ms - last_dump_ms;
        fprintf(stderr,
          "[SCTP-DIAG] %lldms: cwnd=%zu srtt=%d rwnd=%u "
          "tx=%zu(+%zu) rtx=%zu(+%zu) rtx_bytes=%llu(+%llu) "
          "unacked=%zu rtx_rate=%.1f%%\n",
          (long long)dt, m->cwnd_bytes, m->srtt_ms, m->peer_rwnd_bytes,
          m->tx_packets_count, dtx, m->rtx_packets_count, drtx,
          (unsigned long long)m->rtx_bytes_count,
          (unsigned long long)drtx_b,
          m->unack_data_count,
          dtx > 0 ? 100.0 * drtx / dtx : 0.0);
        last_tx = m->tx_packets_count;
        last_rtx = m->rtx_packets_count;
        last_rtx_bytes = m->rtx_bytes_count;
        last_dump_ms = now_ms;
      }
    }
  }

  if (socket_ && data.size() > socket_->options().mtu) {
    RTC_LOG(LS_ERROR) << debug_name_ << "->SendPacketImmediate: MTU exceeded";
    return SendPacketStatus::kError;
  }
  TRACE_EVENT0("webrtc", "DcSctpTransport::SendPacketImmediate");

  if (!transport_ || !transport_->writable())
    return SendPacketStatus::kError;

  auto result = transport_->SendPacket(
      reinterpret_cast<const char*>(data.data()),
      data.size(), AsyncSocketPacketOptions(), 0);

  if (result < 0) {
    if (IsBlockingError(transport_->GetError())) {
      return SendPacketStatus::kTemporaryFailure;
    }
    return SendPacketStatus::kError;
  }
  return SendPacketStatus::kSuccess;
}

std::unique_ptr<dcsctp::Timeout> DcSctpTransport::CreateTimeout(
    TaskQueueBase::DelayPrecision precision) {
  return task_queue_timeout_factory_.CreateTimeout(precision);
}

dcsctp::TimeMs DcSctpTransport::TimeMillis() {
  return dcsctp::TimeMs(env_.clock().TimeInMilliseconds());
}

uint32_t DcSctpTransport::GetRandomInt(uint32_t low, uint32_t high) {
  return random_.Rand(low, high);
}

void DcSctpTransport::OnTotalBufferedAmountLow() {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (!ready_to_send_data_) {
    ready_to_send_data_ = true;
    if (data_channel_sink_) {
      data_channel_sink_->OnReadyToSend();
    }
  }
}

bool DcSctpTransport::HasPacingQueuedPackets() const {
  return !pacing_queue_.empty();
}

int64_t DcSctpTransport::GetMicrosecondsSinceLastPacingSend() const {
  if (last_pacing_send_time_us_ == 0) {
    return -1;  // No pacing sends yet
  }
  int64_t now_us = env_.clock().TimeInMicroseconds();
  return now_us - last_pacing_send_time_us_;
}

void DcSctpTransport::OnBufferedAmountLow(dcsctp::StreamID stream_id) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (data_channel_sink_) {
    data_channel_sink_->OnBufferedAmountLow(*stream_id);
  }
}

void DcSctpTransport::OnMessageReceived(dcsctp::DcSctpMessage message) {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_DLOG(LS_VERBOSE) << debug_name_ << "->OnMessageReceived(sid="
                       << message.stream_id().value()
                       << ", ppid=" << message.ppid().value()
                       << ", length=" << message.payload().size() << ").";
  auto type = ToDataMessageType(message.ppid());
  if (!type.has_value()) {
    RTC_LOG(LS_VERBOSE) << debug_name_
                        << "->OnMessageReceived(): Received an unknown PPID "
                        << message.ppid().value()
                        << " on an SCTP packet. Dropping.";
    return;
  }
  receive_buffer_.Clear();
  if (!IsEmptyPPID(message.ppid()))
    receive_buffer_.AppendData(message.payload().data(),
                               message.payload().size());

  if (data_channel_sink_) {
    data_channel_sink_->OnDataReceived(message.stream_id().value(), *type,
                                       receive_buffer_);
  }
}

void DcSctpTransport::OnError(dcsctp::ErrorKind error,
                              absl::string_view message) {
  if (error == dcsctp::ErrorKind::kResourceExhaustion) {
    // Indicates that a message failed to be enqueued, because the send buffer
    // is full, which is a very common (and wanted) state for high throughput
    // sending/benchmarks.
    RTC_LOG(LS_VERBOSE) << debug_name_
                        << "->OnError(error=" << dcsctp::ToString(error)
                        << ", message=" << message << ").";
  } else {
    RTC_LOG(LS_ERROR) << debug_name_
                      << "->OnError(error=" << dcsctp::ToString(error)
                      << ", message=" << message << ").";
  }
}

void DcSctpTransport::OnAborted(dcsctp::ErrorKind error,
                                absl::string_view message) {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_LOG(LS_ERROR) << debug_name_
                    << "->OnAborted(error=" << dcsctp::ToString(error)
                    << ", message=" << message << ").";
  ready_to_send_data_ = false;
  RTCError rtc_error(RTCErrorType::OPERATION_ERROR_WITH_DATA,
                     std::string(message));
  rtc_error.set_error_detail(RTCErrorDetailType::SCTP_FAILURE);
  auto code = ToErrorCauseCode(error);
  if (code.has_value()) {
    rtc_error.set_sctp_cause_code(static_cast<uint16_t>(*code));
  }
  if (data_channel_sink_) {
    data_channel_sink_->OnTransportClosed(rtc_error);
  }
}

void DcSctpTransport::OnConnected() {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_DLOG(LS_INFO) << debug_name_ << "->OnConnected().";
  ready_to_send_data_ = true;
  if (on_connected_callback_) {
    on_connected_callback_();
  }
  if (data_channel_sink_) {
    data_channel_sink_->OnTransportConnected();
    data_channel_sink_->OnReadyToSend();
  }
}

void DcSctpTransport::OnClosed() {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_DLOG(LS_INFO) << debug_name_ << "->OnClosed().";
  ready_to_send_data_ = false;
}

void DcSctpTransport::OnConnectionRestarted() {
  RTC_DLOG(LS_INFO) << debug_name_ << "->OnConnectionRestarted().";
}

void DcSctpTransport::OnStreamsResetFailed(
    ArrayView<const dcsctp::StreamID> outgoing_streams,
    absl::string_view reason) {
  // TODO(orphis): Need a test to check for correct behavior
  for (auto& stream_id : outgoing_streams) {
    RTC_LOG(LS_WARNING)
        << debug_name_
        << "->OnStreamsResetFailed(...): Outgoing stream reset failed"
        << ", sid=" << stream_id.value() << ", reason: " << reason << ".";
  }
}

void DcSctpTransport::OnStreamsResetPerformed(
    ArrayView<const dcsctp::StreamID> outgoing_streams) {
  RTC_DCHECK_RUN_ON(network_thread_);
  for (auto& stream_id : outgoing_streams) {
    RTC_LOG(LS_INFO) << debug_name_
                     << "->OnStreamsResetPerformed(...): Outgoing stream reset"
                     << ", sid=" << stream_id.value();

    auto it = stream_states_.find(stream_id);
    if (it == stream_states_.end()) {
      // Ignoring an outgoing stream reset for a closed stream
      return;
    }

    StreamState& stream_state = it->second;
    stream_state.outgoing_reset_done = true;

    if (stream_state.incoming_reset_done) {
      //  When the close was not initiated locally, we can signal the end of the
      //  data channel close procedure when the remote ACKs the reset.
      if (data_channel_sink_) {
        data_channel_sink_->OnChannelClosed(stream_id.value());
      }
      stream_states_.erase(stream_id);
    }
  }
}

void DcSctpTransport::OnIncomingStreamsReset(
    ArrayView<const dcsctp::StreamID> incoming_streams) {
  RTC_DCHECK_RUN_ON(network_thread_);
  for (auto& stream_id : incoming_streams) {
    RTC_LOG(LS_INFO) << debug_name_
                     << "->OnIncomingStreamsReset(...): Incoming stream reset"
                     << ", sid=" << stream_id.value();

    auto it = stream_states_.find(stream_id);
    if (it == stream_states_.end())
      return;

    StreamState& stream_state = it->second;
    stream_state.incoming_reset_done = true;

    if (!stream_state.closure_initiated) {
      // When receiving an incoming stream reset event for a non local close
      // procedure, the transport needs to reset the stream in the other
      // direction too.
      dcsctp::StreamID streams[1] = {stream_id};
      socket_->ResetStreams(streams);
      if (data_channel_sink_) {
        data_channel_sink_->OnChannelClosing(stream_id.value());
      }
    }

    if (stream_state.outgoing_reset_done) {
      // The close procedure that was initiated locally is complete when we
      // receive and incoming reset event.
      if (data_channel_sink_) {
        data_channel_sink_->OnChannelClosed(stream_id.value());
      }
      stream_states_.erase(stream_id);
    }
  }
}

void DcSctpTransport::ConnectTransportSignals() {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (!transport_) {
    return;
  }
  transport_->SubscribeWritableState(
      this, [this](PacketTransportInternal* transport) {
        OnTransportWritableState(transport);
      });

  transport_->RegisterReceivedPacketCallback(
      this,
      [&](PacketTransportInternal* transport, const ReceivedIpPacket& packet) {
        OnTransportReadPacket(transport, packet);
      });
  transport_->SetOnCloseCallback([this]() {
    RTC_DCHECK_RUN_ON(network_thread_);
    RTC_DLOG(LS_VERBOSE) << debug_name_ << "->OnTransportClosed().";
    if (data_channel_sink_) {
      data_channel_sink_->OnTransportClosed({});
    }
  });
  transport_->SubscribeDtlsTransportState(
      this, [this](DtlsTransportInternal* transport, DtlsTransportState state) {
        OnDtlsTransportState(transport, state);
      });
}

void DcSctpTransport::DisconnectTransportSignals() {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (!transport_) {
    return;
  }
  transport_->UnsubscribeWritableState(this);
  transport_->DeregisterReceivedPacketCallback(this);
  transport_->SetOnCloseCallback(nullptr);
  transport_->UnsubscribeDtlsTransportState(this);
}

void DcSctpTransport::OnTransportWritableState(
    PacketTransportInternal* transport) {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_DCHECK_EQ(transport_, transport);
  RTC_DLOG(LS_VERBOSE) << debug_name_
                       << "->OnTransportWritableState(), writable="
                       << transport->writable() << " socket: "
                       << (socket_ ? std::to_string(
                                         static_cast<int>(socket_->state()))
                                   : "UNSET");
  MaybeConnectSocket();
}

void DcSctpTransport::OnDtlsTransportState(DtlsTransportInternal* transport,
                                           DtlsTransportState state) {
  if (state == DtlsTransportState::kNew && socket_) {
    // IF DTLS restart (DtlsTransportState::kNew)
    // THEN
    //   reset the socket so that we send an SCTP init
    //   before any outgoing messages. This is needed
    //   after DTLS fingerprint changed since peer will discard
    //   messages with crypto derived from old fingerprint.
    //   The socket will be restarted (with changed parameters)
    //   later.
    RTC_DLOG(LS_INFO) << debug_name_ << " DTLS restart";
    socket_.reset();
  }
}

void DcSctpTransport::OnTransportReadPacket(
    PacketTransportInternal* /* transport */,
    const ReceivedIpPacket& packet) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (packet.decryption_info() != ReceivedIpPacket::kDtlsDecrypted) {
    // We are only interested in SCTP packets.
    return;
  }

  RTC_DLOG(LS_VERBOSE) << debug_name_ << "->OnTransportReadPacket(), length="
                       << packet.payload().size();
  if (socket_) {
    socket_->ReceivePacket(packet.payload());
  }
}

void DcSctpTransport::MaybeConnectSocket() {
  RTC_DLOG(LS_VERBOSE)
      << debug_name_ << "->MaybeConnectSocket(), writable="
      << (transport_ ? std::to_string(transport_->writable()) : "UNSET")
      << " socket: "
      << (socket_ ? std::to_string(static_cast<int>(socket_->state()))
                  : "UNSET");
  if (transport_ && transport_->writable() && socket_ &&
      socket_->state() == dcsctp::SocketState::kClosed) {
    if (!(local_init_.has_value() && remote_init_.has_value())) {
      return socket_->Connect();
    }
    socket_->ConnectWithConnectionToken(*local_init_, *remote_init_);
  }
}

dcsctp::DcSctpOptions DcSctpTransport::CreateDcSctpOptions(
    const SctpOptions& options,
    const FieldTrialsView& field_trials) {
  dcsctp::DcSctpOptions dcsctp_options;
  dcsctp_options.local_port = options.local_port;
  dcsctp_options.remote_port = options.remote_port;
  dcsctp_options.max_message_size = options.max_message_size;
  dcsctp_options.max_timer_backoff_duration = kMaxTimerBackoffDuration;
  // Don't close the connection automatically on too many retransmissions.
  dcsctp_options.max_retransmissions = std::nullopt;
  dcsctp_options.max_init_retransmits = std::nullopt;
  dcsctp_options.per_stream_send_queue_limit =
      DataChannelInterface::MaxSendQueueSize();
  dcsctp_options.announced_maximum_outgoing_streams = options.max_sctp_streams;
  // This is just set to avoid denial-of-service. Practically unlimited.
  dcsctp_options.max_send_buffer_size = std::numeric_limits<size_t>::max();
  dcsctp_options.enable_message_interleaving =
      field_trials.IsEnabled("WebRTC-DataChannelMessageInterleaving");

  // Allow overriding cwnd to a fixed value for testing BUR-only rate control.
  // DCSCTP_CWND_OVERRIDE_MB=10 → 10 MB fixed cwnd (no congestion control).
  // Supports decimal: DCSCTP_CWND_OVERRIDE_MB=0.1 → 100KB
  if (const char* cwnd_env = std::getenv("DCSCTP_CWND_OVERRIDE_MB")) {
    double cwnd_mb = std::stod(cwnd_env);
    if (cwnd_mb > 0) {
      dcsctp_options.cwnd_override_bytes = static_cast<size_t>(cwnd_mb * 1024 * 1024);
      fprintf(stderr, "[SCTP] cwnd override: %.2f MB (%zu bytes)\n",
              cwnd_mb, dcsctp_options.cwnd_override_bytes);
    }
  }

  // DCSCTP_CWND_DYNAMIC=1 → dynamic BDP-based cwnd (CC disabled, cwnd updated
  // per SACK based on pacing_rate × srtt). Bootstrap with 256KB initial cwnd
  // so first packets can flow and trigger SACK → dynamic update loop.
  if (const char* dyn_env = std::getenv("DCSCTP_CWND_DYNAMIC")) {
    if (std::string(dyn_env) == "1") {
      dcsctp_options.cwnd_override_bytes = 256 * 1024;  // 256KB bootstrap
      RTC_LOG(LS_INFO) << "[SCTP] Dynamic cwnd enabled (BDP-based, bootstrap 256KB)";
    }
  }

  // Copa CC requires dcsctp Copa support (cc_algorithm, copa_delta fields)
  // Disabled when Copa fields not available in dcsctp_options.h
  // To re-enable: add CongestionControlAlgorithm enum and copa_delta to DcSctpOptions

  return dcsctp_options;
}

std::vector<uint8_t> DcSctpTransport::GenerateConnectionToken(
    const Environment& env) {
  RTC_DCHECK(env.field_trials().IsEnabled("WebRTC-Sctp-Snap"))
      << "Only implemented under field trial.";
  Random random(env.clock().TimeInMicroseconds());
  auto temp_factory = std::make_unique<dcsctp::DcSctpSocketFactory>();
  return temp_factory->GenerateConnectionToken(
      CreateDcSctpOptions({}, env.field_trials()),
      [&random](uint32_t low, uint32_t high) {
        return random.Rand(low, high);
      });
}

// ===== Pacing Helper Functions (Modified WebRTC Style) =====

void DcSctpTransport::MaybeSchedulePacingDrain(int64_t target_us, int64_t now_us) {
  RTC_DCHECK_RUN_ON(network_thread_);
  pacing_next_deadline_us_ = target_us;
  
  int64_t delay_us = std::max(int64_t{0}, target_us - now_us);
  // NO min 1ms limit - use actual delay (even if 0)
  TimeDelta delay = TimeDelta::Micros(std::max(int64_t{1}, delay_us));
  
  pacing_drain_task_ = RepeatingTaskHandle::DelayedStart(
      network_thread_, delay,
      [this]() {
        RTC_DCHECK_RUN_ON(network_thread_);
        return DrainPacingQueue();
      });
}

TimeDelta DcSctpTransport::DrainPacingQueue() {
  RTC_DCHECK_RUN_ON(network_thread_);

  // Apply any pending MAFS stream priority changes
  ApplyPendingStreamPriorities();

  int64_t now_us = env_.clock().TimeInMicroseconds();
  
  // ===== RTT SAMPLING & PACING DEBUG =====
  static int64_t drain_call_counter = 0;
  static int64_t rtt_sample_count = 0;
  static int64_t last_rtt_ms = 0;
  int64_t now_ms = now_us / 1000;
  
  // Safety: ensure rate is set (InitBurParameters already called from SendPacketWithStatus)
  if (pacing_rate_bps_ == 0 && coordinator_ && !static_pacing_override_) {
    pacing_rate_bps_ = coordinator_->GetPacingRate();
    bur_state_.interval_start_ms = now_ms;
    RTC_LOG(LS_INFO) << "[BUR-PACING] Drain init: rate="
                     << (pacing_rate_bps_ / 1000000) << " Mbps";
  }

  // Check timeouts on pending measurement intervals
  if (coordinator_) {
    coordinator_->CheckTimeouts(now_us);
    if (!static_pacing_override_) {
      pacing_rate_bps_ = coordinator_->GetPacingRate();
    }
  }
  
  // Local BUR interval check (backward compat, only when no coordinator)
  if (!coordinator_ && bur_state_.interval_start_ms > 0 && 
      (now_ms - bur_state_.interval_start_ms) >= bur_interval_ms_) {
    int64_t L_ms = now_ms - bur_state_.interval_start_ms;
    OnBurIntervalComplete(now_ms, L_ms);
  }
  
  // Log pacing status every 1000 calls (~1 sec)
  ++drain_call_counter;
  PACER_DEEP_LOG_IF(drain_call_counter % 1000 == 0,
      "[PACING-STATUS] calls=" << drain_call_counter
      << " queue=" << pacing_queue_.size() << " pkts"
      << " queue_bytes=" << pacing_queue_bytes_
      << " budget=" << pacing_debt_bytes_
      << " rate=" << (pacing_rate_bps_ / 1000000) << "Mbps"
      << " rtt_samples=" << rtt_sample_count);
  
  // ===== BUDGET-BASED PACING (like RTP PacingController) =====
  // Add budget based on elapsed time since last process
  if (pacing_last_process_us_ > 0 && pacing_rate_bps_ > 0) {
    int64_t elapsed_us = now_us - pacing_last_process_us_;
    // budget_addition = elapsed_time * rate (in bytes)
    int64_t budget_bytes = (elapsed_us * pacing_rate_bps_) / 8000000;
    pacing_debt_bytes_ += budget_bytes;
    // Cap budget to avoid huge bursts (max 5ms worth of data).
    // Enforce a minimum of 2 × kMaxSafeMTUSize so that very low pacing
    // rates (e.g. 1 Mbps) can still release a single SCTP packet each
    // drain call — without this floor, the 5 ms budget clamp falls below
    // the MTU (at 1 Mbps: 625 bytes < 1191 bytes = kMaxSafeMTUSize) and
    // the pacer deadlocks because no `pkt_size`-fitting budget ever
    // accumulates. Symptom: BUR rate controller cuts pacing to the
    // configured floor after bufferbloat, then no further packets leave
    // the transport until the floor is raised.
    constexpr int64_t kMinPacerBudgetBytes =
        2 * static_cast<int64_t>(::dcsctp::DcSctpOptions::kMaxSafeMTUSize);
    int64_t max_budget =
        std::max<int64_t>((pacing_rate_bps_ * 5) / 8000, kMinPacerBudgetBytes);
    pacing_debt_bytes_ = std::min(pacing_debt_bytes_, max_budget);
  }
  pacing_last_process_us_ = now_us;
  
  // Send packets while we have budget
  int packets_sent = 0;
  constexpr int kMaxPacketsPerProcess = 100;
  
  // Pacing delay statistics
  static int64_t total_pacing_delay_us = 0;
  static int64_t max_pacing_delay_us = 0;
  static int64_t pacing_delay_samples = 0;
  
  while (!pacing_queue_.empty() && packets_sent < kMaxPacketsPerProcess) {
    const auto& entry = pacing_queue_.front();
    int64_t pkt_size = static_cast<int64_t>(entry.data.size());
    
    // Check if we have enough budget
    if (pacing_debt_bytes_ < pkt_size) {
      break;  // Not enough budget, wait for next timer
    }
    
    // Measure pacing delay
    int64_t pacing_delay_us = now_us - entry.enqueue_time_us;
    total_pacing_delay_us += pacing_delay_us;
    max_pacing_delay_us = std::max(max_pacing_delay_us, pacing_delay_us);
    pacing_delay_samples++;
    
    SendPacketImmediate(ArrayView<const uint8_t>(
        entry.data.data(), entry.data.size()));

    // Parse ALL DATA/I-DATA chunk TSNs from the SCTP packet at drain time.
    // Zero storage overhead — we iterate the packet bytes once here instead
    // of storing TSNs in the queue entry.
    // SCTP packet layout: 12-byte common header, then chunks.
    // Each chunk: type(1) + flags(1) + length(2) + [TSN at offset 4-7 for DATA].
    // DATA chunk type = 0, I-DATA chunk type = 0x40 (64).
    {
      auto actual_sent = webrtc::Timestamp::Micros(now_us);
      const uint8_t* ptr = entry.data.data();
      size_t remaining = entry.data.size();
      uint32_t first_tsn = 0;

      if (remaining >= 12) {
        ptr += 12;  // Skip SCTP common header
        remaining -= 12;
        while (remaining >= 4) {
          uint8_t chunk_type = ptr[0];
          uint16_t chunk_len =
              static_cast<uint16_t>(ptr[2]) << 8 | ptr[3];
          if (chunk_len < 4 || chunk_len > remaining) break;

          // DATA (0) or I-DATA (0x40) chunk with TSN at offset 4
          if ((chunk_type == 0 || chunk_type == 0x40) && chunk_len >= 8) {
            uint32_t tsn = static_cast<uint32_t>(ptr[4]) << 24 |
                           static_cast<uint32_t>(ptr[5]) << 16 |
                           static_cast<uint32_t>(ptr[6]) << 8 |
                           ptr[7];
            if (first_tsn == 0) first_tsn = tsn;
            // Correct time_sent for EVERY TSN in this packet
            if (socket_) {
              socket_->NotifyPacketSent(tsn, actual_sent);
            }
          }
          // Advance to next chunk (padded to 4-byte boundary)
          size_t padded = (chunk_len + 3) & ~size_t{3};
          if (padded > remaining) break;
          ptr += padded;
          remaining -= padded;
        }
      }

      // Report to Coordinator (first TSN as interval marker)
      if (coordinator_ && first_tsn != 0) {
        coordinator_->OnChunkSent(first_tsn, entry.data.size(), now_us);
      }
    }

    // Track last pacing send time for T3-RTX suppression grace period
    last_pacing_send_time_us_ = now_us;

    // Consume budget
    pacing_debt_bytes_ -= pkt_size;
    pacing_queue_bytes_ -= entry.data.size();
    pacing_queue_.pop_front();
    packets_sent++;
  }
  
  // RTT tracking moved to Coordinator
  
  // Log pacing delay stats every 1000 calls
  PACER_DEEP_LOG_IF(drain_call_counter % 1000 == 0 && pacing_delay_samples > 0,
      "[PACING-DELAY] avg=" << (total_pacing_delay_us / pacing_delay_samples / 1000.0) << "ms"
      << " max=" << (max_pacing_delay_us / 1000.0) << "ms"
      << " samples=" << pacing_delay_samples
      << " RTT=" << last_rtt_ms << "ms");
  
  // Calculate next process time
  if (!pacing_queue_.empty() && pacing_rate_bps_ > 0) {
    const auto& next_entry = pacing_queue_.front();
    int64_t needed_bytes = static_cast<int64_t>(next_entry.data.size()) - pacing_debt_bytes_;
    if (needed_bytes > 0) {
      int64_t wait_us = (needed_bytes * 8000000) / pacing_rate_bps_;
      wait_us = std::max(int64_t{100}, wait_us);  // Min 100us
      return TimeDelta::Micros(std::min(wait_us, int64_t{5000}));  // Max 5ms
    }
  }
  
  return TimeDelta::Millis(1);
}

int64_t DcSctpTransport::CalculateTransmitTimeUs(size_t bytes,
                                                  int64_t rate_bps) const {
  if (rate_bps <= 0) return 0;
  return static_cast<int64_t>(bytes * 8.0 * 1e6 / rate_bps);
}

// ===== BUR-based Pacing Control =====
double DcSctpTransport::CalculateBur(int64_t L_us) {
  // BUR = max(0, RTT_max - RTT_ref) / L
  // Using RTT_ref (interval start RTT) as baseline
  // This provides more responsive congestion detection than RTT_min
  // RTT_max - RTT_ref = queuing delay growth during interval
  if (L_us <= 0 || bur_state_.rtt_ref_us < 0) {
    return 0.0;
  }
  
  int64_t delta_us = std::max<int64_t>(0, bur_state_.rtt_max_us - bur_state_.rtt_ref_us);
  double bur = static_cast<double>(delta_us) / L_us;
  
  // Don't clamp BUR - let it reflect actual congestion level
  // BUR > 1.0 means severe congestion (queuing delay > interval)
  return std::max(0.0, bur);
}


void DcSctpTransport::SetCoordinator(RtpSctpCoordinator* coordinator) {
  coordinator_ = coordinator;
  if (coordinator_) {
    // Sync initial pacing rate from coordinator (unless static override)
    if (!static_pacing_override_) {
      pacing_rate_bps_ = coordinator_->GetPacingRate();
    }
    RTC_LOG(LS_INFO) << "[BUR-PACING] Coordinator connected, rate="
                     << (pacing_rate_bps_ / 1000000) << " Mbps";
    // Initialize bandwidth reader for link_utilization tracking
    InitBandwidthReader();
  }
}

void DcSctpTransport::AdjustPacingRate(double bur) {
  // BUR-based rate control (matching webrtc/src BurRateController formulas)
  // 
  // webrtc/src rate control phases:
  //   1. EFFICIENCY/MI (BUR <= 0.85): multiplier = min(1.2, 1 + alpha/R)
  //   2. FAIRNESS/AI-MD (0.85 < BUR <= 1.0): rate *= alpha/R (moderate decrease)
  //   3. SPIKE (BUR > 1.0, 1-2 frames): rate *= 0.85 (15% reduction)
  //   4. DRAINING (BUR > 1.0, 3+ frames): rate = 0.85 * recv_rate - drain_rate
  
  // Use raw BUR for rate decisions (smoothing caused instability)
  
  // Use member variables configured from environment
  const double kAlpha = bur_alpha_;
  const double kBurThreshold = bur_threshold_;
  const double kSpikeThreshold = bur_spike_threshold_;
  const double kMaxMiMultiplier = bur_max_mi_multiplier_;
  const double kSpikeReduction = bur_spike_reduction_;
  constexpr double kEpsilon = 0.01;            // Prevent division by zero
  
  int64_t old_rate = pacing_rate_bps_;
  double rate_factor = 1.0;
  const char* mode = "STABLE";
  
  // Prevent division by zero
  double bur_safe = std::max(bur, kEpsilon);
  
  if (bur > kSpikeThreshold) {
    // SPIKE/DRAINING: BUR > 1.0 means queue is building
    // Use last measured throughput as reference (like webrtc/src uses recv_rate)
    if (bur_state_.last_throughput_bps > min_pacing_rate_bps_) {
      // Set rate based on actual throughput: new_rate = throughput * 0.85
      int64_t target_rate = static_cast<int64_t>(
          bur_state_.last_throughput_bps * kSpikeReduction);
      // Don't reduce below throughput * 0.7
      target_rate = std::max(target_rate, 
          static_cast<int64_t>(bur_state_.last_throughput_bps * 0.7));
      rate_factor = static_cast<double>(target_rate) / pacing_rate_bps_;
      // Still cap the reduction per interval
      rate_factor = std::max(0.7, rate_factor);
    } else {
      // Fallback: cap-based reduction
      double bur_inverse = 1.0 / bur_safe;
      rate_factor = std::max(0.7, std::min(kSpikeReduction, bur_inverse));
    }
    mode = "SPIKE";
  } else if (bur > kBurThreshold) {
    // FAIRNESS/AI-MD: 0.85 < BUR <= 1.0
    // webrtc/src: AI-MD with small oscillations
    // Simplified: rate *= alpha/BUR (gentle decrease)
    rate_factor = kAlpha / bur_safe;
    mode = "AI-MD";
  } else if (bur > kEpsilon) {
    // EFFICIENCY/MI: BUR <= 0.85
    // Formula: rate *= min(1.2, 1 + alpha/R)
    // Let rate grow naturally to max_pacing_rate (network will limit via BUR)
    double multiplier = 1.0 + kAlpha / bur_safe;
    rate_factor = std::min(kMaxMiMultiplier, multiplier);
    mode = "MI";
  } else {
    // PROBE: BUR approx 0 (no congestion signal)
    // Increase based on current rate, capped at 1.2x
    rate_factor = kMaxMiMultiplier;
    mode = "PROBE";
  }
  
  pacing_rate_bps_ = static_cast<int64_t>(pacing_rate_bps_ * rate_factor);
  
  // Clamp to valid range
  pacing_rate_bps_ = std::max(min_pacing_rate_bps_, 
                               std::min(max_pacing_rate_bps_, pacing_rate_bps_));
  
  // Log rate changes
  if (old_rate != pacing_rate_bps_) {
    RTC_LOG(LS_INFO) << "[BUR-RATE] mode=" << mode 
                     << " BUR=" << bur
                     << " factor=" << rate_factor
                     << " rate: " << (old_rate / 1000000) << " -> " 
                     << (pacing_rate_bps_ / 1000000) << " Mbps";
  }
}


void DcSctpTransport::InitBurParameters() {
  // Create Coordinator — always, for passive cwnd/rtt logging even in disabled mode
  const char* mode = std::getenv("COORDINATOR_MODE");
  if (!coordinator_) {
    owned_coordinator_ = std::make_unique<RtpSctpCoordinator>(nullptr);
    coordinator_ = owned_coordinator_.get();
    // Connect transport so MAFS can call SetStreamPriority
    coordinator_->SetDcSctpTransport(this);
    // Initialize bandwidth reader for link_utilization tracking
    InitBandwidthReader();
    if (mode && std::string(mode) == "agentrtc") {
      RTC_LOG(LS_INFO) << "[BUR-PACING] Created owned RtpSctpCoordinator (agentrtc)";
    } else {
      RTC_LOG(LS_INFO) << "[BUR-PACING] Created owned RtpSctpCoordinator (disabled, passive logging)";
    }
  }

  // Read BUR parameters from environment variables
  // These can be set via bur_profiles.csv in automated experiments
  bur_alpha_ = ReadEnvDouble("BUR_ALPHA", 0.85);
  bur_threshold_ = ReadEnvDouble("BUR_THRESHOLD", 0.85);
  bur_spike_threshold_ = ReadEnvDouble("BUR_SPIKE_THRESHOLD", 1.0);
  bur_max_mi_multiplier_ = ReadEnvDouble("BUR_MAX_MI_MULTIPLIER", 1.2);
  bur_spike_reduction_ = ReadEnvDouble("BUR_SPIKE_REDUCTION", 0.85);
  bur_interval_ms_ = ReadEnvInt64("BUR_INTERVAL_MS", 50);
  initial_pacing_rate_bps_ = ReadEnvInt64("BUR_R_INIT_KBPS", 10000) * 1000;
  max_pacing_rate_bps_ = ReadEnvInt64("BUR_MAX_RATE_MBPS", 1000) * 1000000;
  min_pacing_rate_bps_ = ReadEnvInt64("BUR_MIN_RATE_KBPS", 1000) * 1000;
  
  RTC_LOG(LS_INFO) << "[BUR-INIT] Parameters loaded from environment:"
                   << " alpha=" << bur_alpha_
                   << " threshold=" << bur_threshold_
                   << " spike_threshold=" << bur_spike_threshold_
                   << " max_mi=" << bur_max_mi_multiplier_
                   << " spike_reduction=" << bur_spike_reduction_
                   << " interval=" << bur_interval_ms_ << "ms"
                   << " init_rate=" << (initial_pacing_rate_bps_/1000000) << "Mbps"
                   << " max_rate=" << (max_pacing_rate_bps_/1000000) << "Mbps";
}

void DcSctpTransport::OnBurIntervalComplete(int64_t now_ms, int64_t L_ms) {
  // Calculate BUR for completed interval (local fallback when no Coordinator)
  int64_t L_us = L_ms * 1000;
  double bur = CalculateBur(L_us);
  
  // Update local BUR state
  bur_state_.current_bur = bur;
  
  // Adjust pacing rate based on BUR (local fallback)
  AdjustPacingRate(bur);
  
  // Prepare for next interval:
  bur_state_.rtt_ref_us = bur_state_.last_rtt_us;
  bur_state_.rtt_max_us = 0;
  bur_state_.interval_start_ms = now_ms;
  bur_state_.first_interval = false;
}

// ===== Unified Metrics CSV Logging =====
// ===== Bandwidth Reader Implementation (Shared Memory) =====
void DcSctpTransport::InitBandwidthReader() {
  // Get namespace ID from environment (set by automated_experiment)
  const char* ns_id_env = std::getenv("NAMESPACE_ID");
  if (ns_id_env && std::strlen(ns_id_env) > 0) {
    shm_name_ = std::string(kBandwidthShmNameBase) + ns_id_env;
  } else {
    // Fallback to default namespace ID 1
    shm_name_ = std::string(kBandwidthShmNameBase) + "1";
  }
  
  // Open shared memory (read-only)
  shm_fd_ = shm_open(shm_name_.c_str(), O_RDONLY, 0666);
  if (shm_fd_ < 0) {
    RTC_LOG(LS_INFO) << "[BW-READER] Shared memory " << shm_name_
                     << " not available (emulator not running?)";
    return;
  }
  
  // Map to process memory (read-only)
  void* ptr = mmap(nullptr, sizeof(SharedBandwidthData),
                   PROT_READ, MAP_SHARED, shm_fd_, 0);
  if (ptr == MAP_FAILED) {
    RTC_LOG(LS_WARNING) << "[BW-READER] Failed to mmap shared memory";
    close(shm_fd_);
    shm_fd_ = -1;
    return;
  }
  
  shm_data_ = static_cast<const SharedBandwidthData*>(ptr);
  RTC_LOG(LS_INFO) << "[BW-READER] Shared memory initialized: " << shm_name_;
}

void DcSctpTransport::CleanupBandwidthReader() {
  if (shm_data_) {
    munmap(const_cast<SharedBandwidthData*>(shm_data_), sizeof(SharedBandwidthData));
    shm_data_ = nullptr;
  }
  if (shm_fd_ >= 0) {
    close(shm_fd_);
    shm_fd_ = -1;
  }
}

double DcSctpTransport::GetAvailableBandwidthKbps() {
  // Lazy init: retry shm_open if not yet connected (emulator may start later)
  if (!shm_data_ && shm_fd_ < 0 && !shm_name_.empty()) {
    shm_fd_ = shm_open(shm_name_.c_str(), O_RDONLY, 0666);
    if (shm_fd_ >= 0) {
      void* ptr = mmap(nullptr, sizeof(SharedBandwidthData),
                       PROT_READ, MAP_SHARED, shm_fd_, 0);
      if (ptr != MAP_FAILED) {
        shm_data_ = static_cast<const SharedBandwidthData*>(ptr);
        RTC_LOG(LS_INFO) << "[BW-READER] Shared memory connected (lazy): "
                         << shm_name_;
      } else {
        close(shm_fd_);
        shm_fd_ = -1;
      }
    }
  }
  if (!shm_data_ || !shm_data_->valid.load(std::memory_order_acquire)) {
    return -1.0;
  }
  return shm_data_->bandwidth_kbps.load(std::memory_order_relaxed);
}

// ===== Unified Metrics CSV =====
// InitUnifiedMetricsCsv moved to Coordinator

// LogUnifiedMetrics moved to Coordinator

}  // namespace webrtc
