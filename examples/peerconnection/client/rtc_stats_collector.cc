#include "examples/peerconnection/client/rtc_stats_collector.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <ios>
#include <locale>
#include <sstream>

#include "api/stats/rtcstats_objects.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace {
constexpr int kAverageStatsIntervalMs = 200;

std::string SanitizeForFilename(const std::string& input) {
    if (input.empty()) {
        return "sctp_flow";
    }

    std::string sanitized;
    sanitized.reserve(input.size());
    for (char ch : input) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '-' || ch == '_') {
            sanitized.push_back(ch);
        } else {
            sanitized.push_back('_');
        }
    }
    if (sanitized.empty()) {
        return "sctp_flow";
    }
    return sanitized;
}

std::string JoinPath(const std::string& base, const std::string& leaf) {
    if (base.empty()) {
        return leaf;
    }
    std::string path = base;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path.push_back('/');
    }
    path += leaf;
    return path;
}

std::string MakeDataChannelKey(
    const webrtc::RTCDataChannelStats& data_stats) {
    std::string label = data_stats.label.has_value() ? *data_stats.label : "";
    if (!label.empty()) {
        std::replace(label.begin(), label.end(), '|', '_');
    }
    std::string channel_id = data_stats.data_channel_identifier.has_value()
                                  ? std::to_string(
                                        *data_stats.data_channel_identifier)
                                  : "";

    if (!label.empty() || !channel_id.empty()) {
        std::ostringstream oss;
        oss << label << "|" << channel_id;
        return oss.str();
    }

    // Fall back to the raw stats id which is stable for the life of the data
    // channel.
    return std::string(data_stats.id());
}

}  // namespace

RTCStatsCollectorCallback::RTCStatsCollectorCallback(
    std::ofstream& per_frame_stats_file,
    std::ofstream& average_stats_file,
    std::mutex& stats_mutex,
    PersistentStats& persistent_stats)  // Add persistent stats
    : per_frame_stats_file_(per_frame_stats_file),
        average_stats_file_(average_stats_file),
        stats_mutex_(stats_mutex),
        persistent_stats_(persistent_stats) {
    RTC_LOG(LS_INFO) << "RTCStatsCollectorCallback created.";
}

RTCStatsCollectorCallback::~RTCStatsCollectorCallback() {
    RTC_LOG(LS_INFO) << "RTCStatsCollectorCallback destroyed.";
}

void RTCStatsCollectorCallback::OnStatsDelivered(
    const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
    RTC_LOG(LS_INFO) << "OnStatsDelivered called. Posting task to signaling thread.";
    OnStatsDeliveredOnSignalingThread(report);
}


template <typename T>
bool safe_parse(const std::string& str, T* out) {
    static_assert(std::is_integral<T>::value, "safe_parse requires integral types");
    char* end = nullptr;
    int64_t value = std::strtoll(str.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    if (value < std::numeric_limits<T>::min() || value > std::numeric_limits<T>::max()) {
        return false;
    }
    *out = static_cast<T>(value);
    return true;
}

bool ParseTimingFrameInfo(const std::string& timing_info_str, webrtc::TimingFrameInfo* timing_info) {
    if (!timing_info || timing_info_str.empty()) {
        return false;
    }

    std::istringstream iss(timing_info_str);
    std::vector<std::string> tokens;
    std::string token;

    // Split the string into tokens by comma
    while (std::getline(iss, token, ',')) {
        tokens.push_back(token);
    }

    if (tokens.size() != 16) {  // Adjust for 16 fields in the log
        RTC_LOG(LS_ERROR) << "Invalid timing info string: " << timing_info_str;
        return false;
    }

    // Use safe_parse to handle the parsing
    if (!safe_parse(tokens[0], &timing_info->rtp_timestamp) ||
        !safe_parse(tokens[1], &timing_info->capture_time_ms) ||
        !safe_parse(tokens[2], &timing_info->encode_start_ms) ||
        !safe_parse(tokens[3], &timing_info->encode_finish_ms) ||
        !safe_parse(tokens[4], &timing_info->packetization_finish_ms) ||
        !safe_parse(tokens[5], &timing_info->pacer_exit_ms) ||
        !safe_parse(tokens[6], &timing_info->network_timestamp_ms) ||
        !safe_parse(tokens[7], &timing_info->network2_timestamp_ms) ||
        !safe_parse(tokens[8], &timing_info->receive_start_ms) ||
        !safe_parse(tokens[9], &timing_info->receive_finish_ms) ||
        !safe_parse(tokens[10], &timing_info->decode_start_ms) ||
        !safe_parse(tokens[11], &timing_info->decode_finish_ms) ||
        !safe_parse(tokens[12], &timing_info->render_time_ms)) {// bool-like integer
        RTC_LOG(LS_ERROR) << "Error parsing timing info string: " << timing_info_str;
        return false;
    }

    return true;
}

void RTCStatsCollectorCallback::ProcessInboundRTPStats(const webrtc::RTCStats& stats) {
    std::vector<webrtc::Attribute> attributes = stats.Attributes();
    auto find_attribute = [&attributes](const std::string& name) -> const webrtc::Attribute* {
        for (const auto& attribute : attributes) {
            if (attribute.name() == name) {
                RTC_LOG(LS_INFO) << "Found attribute " << name << " = " << attribute.ToString();
                return &attribute;
            }
        }
        RTC_LOG(LS_WARNING) << "Attribute not found: " << name;
        return nullptr;
    };

    // Helper function to get numeric values safely without exceptions
    auto get_numeric = [&find_attribute](const std::string& name, double default_value = 0) -> double {
        const auto* attr = find_attribute(name);
        if (!attr || attr->ToString() == "null") {
            return default_value;  // Return default if null
        }
        const std::string& value_str = attr->ToString();
        char* end;
        double value = std::strtod(value_str.c_str(), &end);
        if (end == value_str.c_str()) {
            RTC_LOG(LS_WARNING) << "Failed to convert value for " << name << ": " << value_str;
            return default_value;  // Return default on conversion failure
        }
        RTC_LOG(LS_INFO) << "Got numeric value for " << name << ": " << value;
        return value;
    };

    const auto* timing_info_attr = find_attribute("googTimingFrameInfo");
    
    std::lock_guard<std::mutex> lock(stats_mutex_); 
    if (timing_info_attr) {
         // Extract the timing info as a string
        std::string timing_info_str = timing_info_attr->ToString();

        // Parse the timing info string into TimingFrameInfo
        webrtc::TimingFrameInfo timing_info;
        if (ParseTimingFrameInfo(timing_info_str, &timing_info) && timing_info.encode_start_ms > 10000) {
            // Calculate timing stages
            int64_t encoding_ms = (timing_info.encode_finish_ms >= 0 && timing_info.encode_start_ms >= 0)
                                    ? timing_info.encode_finish_ms - timing_info.encode_start_ms
                                    : -1;

            int64_t network_ms = (timing_info.network2_timestamp_ms >= 0 && timing_info.pacer_exit_ms >= 0)
                                    ? timing_info.network2_timestamp_ms - timing_info.pacer_exit_ms
                                    : -1;

            int64_t decoding_ms = (timing_info.decode_finish_ms >= 0 && timing_info.decode_start_ms >= 0)
                                    ? timing_info.decode_finish_ms - timing_info.decode_start_ms
                                    : -1;

            int64_t rendering_ms = (timing_info.render_time_ms >= 0 && timing_info.decode_finish_ms >= 0)
                                    ? timing_info.render_time_ms - timing_info.decode_finish_ms
                                    : -1;

            int64_t e2e_ms = (timing_info.capture_time_ms >= 0 && timing_info.decode_finish_ms >= 0)
                                ? timing_info.decode_finish_ms - timing_info.capture_time_ms
                                : -1;

            // Calculate inter-frame timing (delta between current and last render_time_ms)
            int64_t inter_frame_ms = -1;
            if (persistent_stats_.last_render_time_ms_ >= 0 && timing_info.render_time_ms >= 0) {
                inter_frame_ms = timing_info.render_time_ms - persistent_stats_.last_render_time_ms_;
            }
            persistent_stats_.last_render_time_ms_ = timing_info.render_time_ms;

            // Calculate intra-frame construction time (delta between receive_finish_ms and receive_start_ms)
            int64_t intra_construction_ms = (timing_info.receive_finish_ms >= 0 && timing_info.receive_start_ms >= 0)
                                                ? timing_info.receive_finish_ms - timing_info.receive_start_ms
                                                : -1;

            // Log the frame timings to the file
            {
                if (per_frame_stats_file_.is_open()) {
                    per_frame_stats_file_ << rtc::TimeMillis() << "," << timing_info.rtp_timestamp << ","
                                        << encoding_ms << ","
                                        << network_ms << ","
                                        << decoding_ms << ","
                                        << rendering_ms << ","
                                        << e2e_ms << ","
                                        << inter_frame_ms << ","
                                        << intra_construction_ms << "\n";
                    per_frame_stats_file_.flush();
                }
            }
            // Update the last processed timestamp
            persistent_stats_.last_timestamp_ = timing_info.rtp_timestamp;
        }
    }


    persistent_stats_.frame_timing_count_ += 1;

    // Extract all stats with null handling
    int64_t frames_decoded = static_cast<int64_t>(get_numeric("framesDecoded"));
    int64_t frames_dropped = static_cast<int64_t>(get_numeric("framesDropped"));
    int64_t frames_received = static_cast<int64_t>(get_numeric("framesReceived"));
    double framerate = get_numeric("framesPerSecond");
    double min_playout_delay_ms = get_numeric("googMinPlayoutDelayMs");
    double jitter_buffer_delay = get_numeric("jitterBufferDelay") * 1000.0;
    int64_t width = static_cast<int64_t>(get_numeric("frameWidth"));
    int64_t height = static_cast<int64_t>(get_numeric("frameHeight"));
    double total_decode_time = get_numeric("totalDecodeTime") * 1000.0;
    int64_t bytes_received = static_cast<int64_t>(get_numeric("bytesReceived"));

    int64_t packets_received        = static_cast<int64_t>(get_numeric("packetsReceived"));
    int64_t packets_lost            = static_cast<int64_t>(get_numeric("packetsLost"));
    int64_t packets_discarded       = static_cast<int64_t>(get_numeric("packetsDiscarded"));
    int64_t fec_packets_received    = static_cast<int64_t>(get_numeric("fecPacketsReceived"));
    int64_t fec_packets_discarded   = static_cast<int64_t>(get_numeric("fecPacketsDiscarded"));
    int64_t packets_repaired        = static_cast<int64_t>(get_numeric("packetsRepaired"));
    int64_t fec_bytes_recv = static_cast<int64_t>(get_numeric("fecBytesReceived"));

    int64_t retx_pkts_recv  = static_cast<int64_t>(get_numeric("retransmittedPacketsReceived"));
    int64_t retx_bytes_recv = static_cast<int64_t>(get_numeric("retransmittedBytesReceived"));
    int64_t freeze_count = static_cast<int64_t>(get_numeric("freezeCount"));
    double total_freezes_duration_ms = get_numeric("totalFreezesDuration") * 1000.0;

    int64_t current_time_ms = rtc::TimeMillis();

    // Initialize first stats time if not set
    if (persistent_stats_.first_stats_time_ms_ == -1) {
        persistent_stats_.first_stats_time_ms_ = current_time_ms;
        persistent_stats_.period_start_time_ms_ = current_time_ms;
        persistent_stats_.period_start_bytes_ = bytes_received;
        persistent_stats_.total_bytes_received_ = bytes_received;
    }

    // Accumulate values
    persistent_stats_.acc_frames_decoded_ += frames_decoded;
    persistent_stats_.acc_frames_dropped_ += frames_dropped;
    persistent_stats_.acc_frames_received_ += frames_received;
    persistent_stats_.acc_framerate_ += framerate;
    persistent_stats_.acc_jitter_buffer_delay_ += jitter_buffer_delay;
    persistent_stats_.acc_min_playout_delay_  += min_playout_delay_ms;
    persistent_stats_.acc_total_decode_time_ += total_decode_time;
    persistent_stats_.acc_count_++;

    bool should_write =
        (current_time_ms - persistent_stats_.last_average_time_ms_) >=
        kAverageStatsIntervalMs;

    if (should_write && persistent_stats_.acc_count_ > 0) {

        double period_time_sec =
            (current_time_ms - persistent_stats_.period_start_time_ms_) / 1000.0;
        // Sender bitrates
        double period_sender_bitrate  = 0.0;
        double overall_sender_bitrate = 0.0;

        if (persistent_stats_.first_remote_stats_time_ms_ != -1) {
            double overall_remote_time_sec =
                (current_time_ms - persistent_stats_.first_remote_stats_time_ms_) / 1000.0;
            if (overall_remote_time_sec > 0) {
                overall_sender_bitrate =
                    (persistent_stats_.last_remote_bytes_sent_ * 8.0) / overall_remote_time_sec; // bit/s
            }

            double period_remote_bytes_delta =
                persistent_stats_.last_remote_bytes_sent_ -
                persistent_stats_.period_remote_start_bytes_;
            if (period_time_sec > 0) {
                period_sender_bitrate =
                    (period_remote_bytes_delta * 8.0) / period_time_sec; // bit/s
            }
        }

        // Calculate overall average bitrate (since start)
        double avg_min_playout_delay = persistent_stats_.acc_min_playout_delay_ / persistent_stats_.acc_count_;
        double overall_time_sec = (current_time_ms - persistent_stats_.first_stats_time_ms_) / 1000.0;
        double overall_bytes_delta = bytes_received;
        double overall_average_bitrate = 0.0;
        if (overall_time_sec > 0) {
            overall_average_bitrate = (overall_bytes_delta * 8.0) / overall_time_sec;  // bits per second
        }
        persistent_stats_.total_bytes_received_ = bytes_received;

        // Calculate current period average bitrate
        double period_bytes_delta = bytes_received - persistent_stats_.period_start_bytes_;
        double period_average_bitrate = 0.0;
        if (period_time_sec > 0) {
            period_average_bitrate = (period_bytes_delta * 8.0) / period_time_sec;  // bits per second
        }

        // Calculate averages
        double avg_frames_decoded = static_cast<double>(persistent_stats_.acc_frames_decoded_) / persistent_stats_.acc_count_;
        double avg_frames_dropped = static_cast<double>(persistent_stats_.acc_frames_dropped_) / persistent_stats_.acc_count_;
        double avg_frames_received = static_cast<double>(persistent_stats_.acc_frames_received_) / persistent_stats_.acc_count_;
        double avg_framerate = persistent_stats_.acc_framerate_ / persistent_stats_.acc_count_;
        double avg_jitter_buffer_delay = persistent_stats_.acc_jitter_buffer_delay_ / persistent_stats_.acc_count_;
        double avg_total_decode_time = persistent_stats_.acc_total_decode_time_ / persistent_stats_.acc_count_;

        /* ─── compute per-interval deltas ─── */
        int64_t period_packets_received      = 0;
        int64_t period_packets_lost          = 0;
        int64_t period_packets_discarded     = 0;
        int64_t period_fec_packets_received  = 0;
        int64_t period_fec_packets_discarded = 0;
        int64_t period_packets_repaired      = 0;

        if (persistent_stats_.last_packets_received_ != -1) {
            period_packets_received      = packets_received      - persistent_stats_.last_packets_received_;
            period_packets_lost          = packets_lost          - persistent_stats_.last_packets_lost_;
            period_packets_discarded     = packets_discarded     - persistent_stats_.last_packets_discarded_;
            period_fec_packets_received  = fec_packets_received  - persistent_stats_.last_fec_packets_received_;
            period_fec_packets_discarded = fec_packets_discarded - persistent_stats_.last_fec_packets_discarded_;
            period_packets_repaired      = packets_repaired      - persistent_stats_.last_packets_repaired_;
        }

        double loss_ratio = 0.0;
        if (period_packets_received + period_packets_lost > 0) {
            loss_ratio = static_cast<double>(period_packets_lost) /
                        static_cast<double>(period_packets_received + period_packets_lost);
        }

        /* update last-cumulative snapshots */
        persistent_stats_.last_packets_received_      = packets_received;
        persistent_stats_.last_packets_lost_          = packets_lost;
        persistent_stats_.last_packets_discarded_     = packets_discarded;
        persistent_stats_.last_fec_packets_received_  = fec_packets_received;
        persistent_stats_.last_fec_packets_discarded_ = fec_packets_discarded;
        persistent_stats_.last_packets_repaired_      = packets_repaired;

        // ── receiver FEC bytes Δ ──
        int64_t period_fec_bytes_recv = 0;
        if (persistent_stats_.last_fec_bytes_recv_ != -1)
            period_fec_bytes_recv = fec_bytes_recv -
                                    persistent_stats_.last_fec_bytes_recv_;
        persistent_stats_.last_fec_bytes_recv_ = fec_bytes_recv;

        // ── sender FEC bytes Δ ──
        int64_t period_fec_bytes_sent = 0;
        if (persistent_stats_.period_remote_start_fec_bytes_sent_ != 0)
            period_fec_bytes_sent = persistent_stats_.last_remote_fec_bytes_sent_ -
                                    persistent_stats_.period_remote_start_fec_bytes_sent_;

        // receiver-side FEC share of total traffic this interval
        double fec_byte_ratio = 0.0;
        if (period_bytes_delta > 0)                       // already computed bytes Δ
            fec_byte_ratio = static_cast<double>(period_fec_bytes_recv) /
                            static_cast<double>(period_bytes_delta);

        // ───── retransmission deltas (receiver) ─────
        int64_t period_retx_pkts_recv  = 0;
        int64_t period_retx_bytes_recv = 0;

        if (persistent_stats_.last_retx_pkts_recv_ != -1) {
            period_retx_pkts_recv  = retx_pkts_recv  - persistent_stats_.last_retx_pkts_recv_;
            period_retx_bytes_recv = retx_bytes_recv - persistent_stats_.last_retx_bytes_recv_;
        }

        // update snapshots for next period
        persistent_stats_.last_retx_pkts_recv_  = retx_pkts_recv;
        persistent_stats_.last_retx_bytes_recv_ = retx_bytes_recv;

        // ───── retransmission deltas (sender) ─────
        int64_t period_retx_pkts_sent  = 0;
        int64_t period_retx_bytes_sent = 0;
        if (persistent_stats_.period_remote_start_retx_pkts_sent_ != 0) {
            period_retx_pkts_sent  = persistent_stats_.last_remote_retx_pkts_sent_
                                - persistent_stats_.period_remote_start_retx_pkts_sent_;
            period_retx_bytes_sent = persistent_stats_.last_remote_retx_bytes_sent_
                                - persistent_stats_.period_remote_start_retx_bytes_sent_;
        }

        // simple receiver-side retransmission ratio
        double retransmission_ratio = 0.0;
        if (period_packets_received + period_packets_lost > 0) {
            retransmission_ratio = static_cast<double>(period_retx_pkts_recv) /
                                static_cast<double>(period_packets_received + period_packets_lost);
        }

        int64_t period_freeze_count = 0;
        if (persistent_stats_.last_freeze_count_ != -1) {
            period_freeze_count = freeze_count - persistent_stats_.last_freeze_count_;
            if (period_freeze_count < 0)
                period_freeze_count = 0;
        }
        persistent_stats_.last_freeze_count_ = freeze_count;

        double period_freeze_duration_ms = 0.0;
        if (persistent_stats_.last_total_freezes_duration_ms_ >= 0.0) {
            period_freeze_duration_ms =
                total_freezes_duration_ms -
                persistent_stats_.last_total_freezes_duration_ms_;
            if (period_freeze_duration_ms < 0.0)
                period_freeze_duration_ms = 0.0;
        }
        persistent_stats_.last_total_freezes_duration_ms_ = total_freezes_duration_ms;

        double freeze_time_ratio = 0.0;
        if (period_time_sec > 0) {
            freeze_time_ratio =
                (period_freeze_duration_ms / 1000.0) / period_time_sec;
        }

        // Handle decoder implementation
        const auto* decoder_impl_attr = find_attribute("decoderImplementation");
        std::string decoder_implementation =
            decoder_impl_attr ? decoder_impl_attr->ToString() : "unknown";
        if (decoder_implementation == "null") {
            decoder_implementation = "unknown";
        }

        // Write stats to file with mutex protection
        if (average_stats_file_.is_open() && avg_frames_decoded > 0) {
            average_stats_file_ << current_time_ms << ","
                << avg_frames_decoded << ","
                << avg_frames_dropped << ","
                << avg_frames_received << ","
                << avg_framerate << ","
                << avg_jitter_buffer_delay << ","
                << avg_min_playout_delay << ","
                << width << ","
                << height << ","
                << avg_total_decode_time << ","
                << bytes_received << ","
                << period_average_bitrate << ","
                << overall_average_bitrate << ","
                << period_sender_bitrate << ","          
                << overall_sender_bitrate << ","         
                << decoder_implementation << ","
                << period_packets_received << ","
                << period_packets_lost << ","
                << loss_ratio << ","
                << period_packets_discarded << ","
                << period_fec_packets_received << ","
                << period_fec_packets_discarded << ","
                << period_packets_repaired << ","
                << period_fec_bytes_recv << ","
                << period_fec_bytes_sent << ","
                << fec_byte_ratio <<","
                << period_retx_pkts_recv << ","
                << period_retx_bytes_recv << ","
                << period_retx_pkts_sent << ","
                << period_retx_bytes_sent << ","
                << retransmission_ratio << ","
                << period_freeze_count << ","
                << period_freeze_duration_ms << ","
                << freeze_time_ratio << "\n";
                average_stats_file_.flush();
        }

        // Reset accumulators
        persistent_stats_.acc_min_playout_delay_ = 0.0; // **Reset**
        persistent_stats_.acc_frames_decoded_ = 0;
        persistent_stats_.acc_frames_dropped_ = 0;
        persistent_stats_.acc_frames_received_ = 0;
        persistent_stats_.acc_framerate_ = 0.0;
        persistent_stats_.acc_jitter_buffer_delay_ = 0.0;
        persistent_stats_.acc_total_decode_time_ = 0.0;
        persistent_stats_.acc_count_ = 0;
        persistent_stats_.last_average_time_ms_ = current_time_ms;
        persistent_stats_.period_remote_start_bytes_ = persistent_stats_.last_remote_bytes_sent_;

        persistent_stats_.period_remote_start_retx_bytes_sent_ =

        persistent_stats_.period_remote_start_fec_bytes_sent_ =
            persistent_stats_.last_remote_fec_bytes_sent_;

        // Reset period accumulators
        persistent_stats_.period_start_time_ms_ = current_time_ms;
        persistent_stats_.period_start_bytes_ = bytes_received;

        persistent_stats_.period_remote_start_retx_pkts_sent_  = persistent_stats_.last_remote_retx_pkts_sent_;
        persistent_stats_.period_remote_start_retx_bytes_sent_ = persistent_stats_.last_remote_retx_bytes_sent_;

    }
}

void RTCStatsCollectorCallback::ProcessRemoteOutboundRTPStats(
    const webrtc::RTCStats& stats) {

  std::vector<webrtc::Attribute> attributes = stats.Attributes();
  auto find_attribute = [&attributes](const std::string& name) -> const webrtc::Attribute* {
      for (const auto& a : attributes)
          if (a.name() == name) return &a;
      return nullptr;
  };
  auto get_numeric = [&find_attribute](const std::string& name,
                                       double def = 0) -> double {
      const auto* attr = find_attribute(name);
      if (!attr || attr->ToString() == "null") return def;
      char* end; const std::string& s = attr->ToString();
      double v = std::strtod(s.c_str(), &end);
      return (end == s.c_str()) ? def : v;
  };

  std::lock_guard<std::mutex> lock(stats_mutex_);

  int64_t bytes_sent = static_cast<int64_t>(get_numeric("bytesSent"));
  int64_t now_ms     = rtc::TimeMillis();

  if (persistent_stats_.first_remote_stats_time_ms_ == -1) {
      persistent_stats_.first_remote_stats_time_ms_ = now_ms;
      persistent_stats_.period_remote_start_bytes_  = bytes_sent;
  }
  persistent_stats_.last_remote_bytes_sent_ = bytes_sent;

  int64_t fec_bytes_sent = static_cast<int64_t>(
                           get_numeric("fecBytesSent"));

  if (persistent_stats_.first_remote_stats_time_ms_ == -1) {
      /* …existing init… */
      persistent_stats_.period_remote_start_fec_bytes_sent_ = fec_bytes_sent;
  }
  persistent_stats_.last_remote_fec_bytes_sent_ = fec_bytes_sent;

  int64_t retx_bytes_sent = static_cast<int64_t>(
                            get_numeric("retransmittedBytesSent"));

    if (persistent_stats_.first_remote_stats_time_ms_ == -1) {
        persistent_stats_.period_remote_start_retx_bytes_sent_ = retx_bytes_sent;
    }
    persistent_stats_.last_remote_retx_bytes_sent_ = retx_bytes_sent;
}

void RTCStatsCollectorCallback::ProcessDataChannelStats(
    const webrtc::RTCStats& stats) {
    const auto& data_stats = stats.cast_to<webrtc::RTCDataChannelStats>();

    std::string label = data_stats.label.has_value()
                            ? *data_stats.label
                            : std::string(stats.id());
    std::string key = MakeDataChannelKey(data_stats);

    uint64_t total_bytes_received =
        data_stats.bytes_received.value_or(0);
    uint64_t total_bytes_sent = data_stats.bytes_sent.value_or(0);
    int64_t timestamp_ms = data_stats.timestamp().ms();

    std::lock_guard<std::mutex> lock(stats_mutex_);

    if (persistent_stats_.log_directory_.empty()) {
        RTC_LOG(LS_WARNING)
            << "SCTP throughput logging skipped: log directory not set.";
        return;
    }

    DataChannelLogState& channel_state =
        persistent_stats_.data_channel_logs_[key];

    if (channel_state.label.empty()) {
        channel_state.label = label;
    }
    if (channel_state.unique_key.empty()) {
        channel_state.unique_key = key;
    }

    if (channel_state.sanitized_label.empty()) {
        channel_state.sanitized_label = SanitizeForFilename(label);
        if (channel_state.sanitized_label.empty()) {
            channel_state.sanitized_label = SanitizeForFilename(key);
        }
    }
    if (channel_state.data_channel_id == -1 &&
        data_stats.data_channel_identifier.has_value()) {
        channel_state.data_channel_id =
            data_stats.data_channel_identifier.value();
    }

    if (!channel_state.csv_file.is_open()) {
        // Use only the sanitized label for the filename to avoid duplication
        std::string filename = channel_state.sanitized_label;
        if (channel_state.data_channel_id >= 0) {
            filename += "_dc" + std::to_string(channel_state.data_channel_id);
        }
        filename += "_throughput.csv";
        std::string path = JoinPath(persistent_stats_.log_directory_, filename);
        channel_state.csv_file.open(path, std::ios::out | std::ios::trunc);
        if (!channel_state.csv_file.is_open()) {
            RTC_LOG(LS_WARNING)
                << "Failed to open SCTP throughput log file: " << path;
            return;
        }
        RTC_LOG(LS_INFO) << "Logging SCTP throughput for '" << label
                          << "' (key: '" << channel_state.unique_key
                          << "') to " << path;
        channel_state.csv_file
            << "timestamp_ms,interval_ms,bytes_received,total_bytes_received,"
               "receive_throughput_bps,receive_throughput_mbps,"
               "bytes_sent,total_bytes_sent,send_throughput_bps,"
               "send_throughput_mbps\n";
        channel_state.csv_file.flush();
    }

    if (channel_state.first_timestamp_ms == -1) {
        channel_state.first_timestamp_ms = timestamp_ms;
    }

    bool has_previous = channel_state.last_timestamp_ms >= 0;
    int64_t interval_ms = has_previous
                              ? (timestamp_ms - channel_state.last_timestamp_ms)
                              : 0;

    if (has_previous && interval_ms <= 0) {
        channel_state.last_timestamp_ms = timestamp_ms;
        channel_state.last_bytes_received = total_bytes_received;
        channel_state.last_bytes_sent = total_bytes_sent;
        return;
    }

    uint64_t delta_bytes_received = 0;
    uint64_t delta_bytes_sent = 0;
    if (has_previous) {
        if (total_bytes_received >= channel_state.last_bytes_received) {
            delta_bytes_received =
                total_bytes_received - channel_state.last_bytes_received;
        }
        if (total_bytes_sent >= channel_state.last_bytes_sent) {
            delta_bytes_sent = total_bytes_sent - channel_state.last_bytes_sent;
        }
    }

    double recv_throughput_bps = 0.0;
    double send_throughput_bps = 0.0;
    if (interval_ms > 0) {
        const double interval_sec =
            static_cast<double>(interval_ms) / 1000.0;
        recv_throughput_bps =
            interval_sec > 0 ? (delta_bytes_received * 8.0) / interval_sec : 0.0;
        send_throughput_bps =
            interval_sec > 0 ? (delta_bytes_sent * 8.0) / interval_sec : 0.0;
    }

    double recv_throughput_mbps = recv_throughput_bps / 1'000'000.0;
    double send_throughput_mbps = send_throughput_bps / 1'000'000.0;

    std::ostringstream line;
    line.imbue(std::locale::classic());
    line << timestamp_ms << "," << interval_ms << "," << delta_bytes_received
         << "," << total_bytes_received << "," << recv_throughput_bps << ","
         << std::fixed << std::setprecision(6) << recv_throughput_mbps << ","
         << std::defaultfloat << delta_bytes_sent << "," << total_bytes_sent
         << "," << send_throughput_bps << "," << std::fixed
         << std::setprecision(6) << send_throughput_mbps;

    channel_state.csv_file << line.str() << "\n";
    channel_state.csv_file.flush();

    channel_state.last_timestamp_ms = timestamp_ms;
    channel_state.last_bytes_received = total_bytes_received;
    channel_state.last_bytes_sent = total_bytes_sent;
}


void RTCStatsCollectorCallback::ProcessTransportStats(
    const webrtc::RTCStats& stats) {
    const auto& transport_stats = stats.cast_to<webrtc::RTCTransportStats>();

    uint64_t total_bytes_received = transport_stats.bytes_received.value_or(0);
    uint64_t total_bytes_sent = transport_stats.bytes_sent.value_or(0);

    std::lock_guard<std::mutex> lock(stats_mutex_);

    if (persistent_stats_.log_directory_.empty()) {
        RTC_LOG(LS_WARNING)
            << "SCTP transport throughput logging skipped: log directory not set.";
        return;
    }

    const std::string& transport_id = transport_stats.id();

    // Log all transport stats to capture SCTP throughput at transport level
    TransportLogState& transport_state =
        persistent_stats_.transport_logs_[transport_id];

    if (transport_state.transport_id.empty()) {
        transport_state.transport_id = transport_id;
    }

    if (transport_state.sanitized_id.empty()) {
        transport_state.sanitized_id =
            SanitizeForFilename(transport_state.transport_id);
        if (transport_state.sanitized_id.empty()) {
            transport_state.sanitized_id = "sctp_transport";
        }
    }

    if (!transport_state.csv_file.is_open()) {
        std::string filename;
        if (!persistent_stats_.sctp_flow_name_.empty()) {
            filename = persistent_stats_.sctp_flow_name_ + "_throughput.csv";
        } else {
            filename = transport_state.sanitized_id + "_throughput.csv";
        }
        std::string path = JoinPath(persistent_stats_.log_directory_, filename);
        transport_state.csv_file.open(path, std::ios::out | std::ios::trunc);
        if (!transport_state.csv_file.is_open()) {
            RTC_LOG(LS_WARNING)
                << "Failed to open SCTP transport throughput log file: " << path;
            persistent_stats_.transport_logs_.erase(transport_id);
            return;
        }
        RTC_LOG(LS_INFO) << "Logging SCTP transport throughput for '"
                          << transport_state.transport_id << "' to " << path;
        transport_state.csv_file
            << "timestamp_ms,interval_ms,bytes_received,total_bytes_received,"
               "receive_throughput_bps,receive_throughput_mbps,bytes_sent,"
               "total_bytes_sent,send_throughput_bps,send_throughput_mbps\n";
        transport_state.csv_file.flush();
    }

    // Initialize period start if needed
    int64_t current_time_ms = rtc::TimeMillis();
    if (transport_state.period_start_time_ms == -1) {
        transport_state.period_start_time_ms = current_time_ms;
        transport_state.period_start_bytes_received = total_bytes_received;
        transport_state.period_start_bytes_sent = total_bytes_sent;
        transport_state.last_log_time_ms = current_time_ms;
        return;
    }

    // Accumulate bytes received/sent in this collection cycle
    if (total_bytes_received >= transport_state.period_start_bytes_received) {
        uint64_t delta = total_bytes_received - transport_state.period_start_bytes_received;
        transport_state.accumulated_bytes_received += delta;
    }
    if (total_bytes_sent >= transport_state.period_start_bytes_sent) {
        uint64_t delta = total_bytes_sent - transport_state.period_start_bytes_sent;
        transport_state.accumulated_bytes_sent += delta;
    }
    transport_state.period_start_bytes_received = total_bytes_received;
    transport_state.period_start_bytes_sent = total_bytes_sent;

    // Check if we should log (every 200ms)
    int64_t time_since_last_log = current_time_ms - transport_state.last_log_time_ms;
    constexpr int kLogIntervalMs = 200;

    if (time_since_last_log >= kLogIntervalMs) {
        // Calculate throughput over the 200ms interval
        double interval_sec = static_cast<double>(time_since_last_log) / 1000.0;
        double recv_throughput_bps = 0.0;
        double send_throughput_bps = 0.0;

        if (interval_sec > 0) {
            recv_throughput_bps = (transport_state.accumulated_bytes_received * 8.0) / interval_sec;
            send_throughput_bps = (transport_state.accumulated_bytes_sent * 8.0) / interval_sec;
        }

        double recv_throughput_mbps = recv_throughput_bps / 1'000'000.0;
        double send_throughput_mbps = send_throughput_bps / 1'000'000.0;

        std::ostringstream line;
        line.imbue(std::locale::classic());
        line << current_time_ms << ","
             << time_since_last_log << ","
             << transport_state.accumulated_bytes_received << ","
             << total_bytes_received << ","
             << recv_throughput_bps << ","
             << std::fixed << std::setprecision(6) << recv_throughput_mbps << ","
             << std::defaultfloat
             << transport_state.accumulated_bytes_sent << ","
             << total_bytes_sent << ","
             << send_throughput_bps << ","
             << std::fixed << std::setprecision(6) << send_throughput_mbps;

        transport_state.csv_file << line.str() << "\n";
        transport_state.csv_file.flush();

        // Reset accumulators for next period
        transport_state.accumulated_bytes_received = 0;
        transport_state.accumulated_bytes_sent = 0;
        transport_state.last_log_time_ms = current_time_ms;
    }
}


void RTCStatsCollectorCallback::OnStatsDeliveredOnSignalingThread(
    rtc::scoped_refptr<const webrtc::RTCStatsReport> report) {
    RTC_LOG(LS_INFO) << "OnStatsDeliveredOnSignalingThread called.";

    if (!report) {
        RTC_LOG(LS_ERROR) << "Null stats report received";
        return;
    }

    for (const auto& stats : *report) {
        std::string stats_type(stats.type());
        if (stats_type == webrtc::RTCDataChannelStats::kType) {
            ProcessDataChannelStats(stats);
            continue;
        }
        if (stats_type == webrtc::RTCTransportStats::kType) {
            ProcessTransportStats(stats);
            continue;
        }

        std::vector<webrtc::Attribute> attributes = stats.Attributes();
        auto find_attribute = [&attributes](const std::string& name) -> const webrtc::Attribute* {
            for (const auto& attribute : attributes) {
                if (attribute.name() == name) {
                    return &attribute;
                }
            }
            return nullptr;
        };

        const auto* kind = find_attribute("kind");
        if (!kind) {
            RTC_LOG(LS_INFO) << "Skipping stats " << stats.id() << " because 'kind' attribute not found.";
            continue;
        }

        if (kind->ToString() != "video") {
            RTC_LOG(LS_INFO) << "Skipping stats " << stats.id() << " because kind is not video.";
            continue;
        }

        if (std::string(stats.type()) == "inbound-rtp") {
            RTC_LOG(LS_INFO) << "Processing inbound-rtp stats: " << stats.id();
            ProcessInboundRTPStats(stats);
        }
        if (std::string(stats.type()) == "remote-outbound-rtp") {
            RTC_LOG(LS_INFO) << "Processing remote-outbound-rtp stats: " << stats.id();
            ProcessRemoteOutboundRTPStats(stats);
        }
    }
}

RTCStatsCollector::RTCStatsCollector() {
    RTC_LOG(LS_INFO) << "Creating RTCStatsCollector...";
}

RTCStatsCollector::~RTCStatsCollector() {
    RTC_LOG(LS_INFO) << "Destroying RTCStatsCollector...";
    Stop();  // Ensure the thread is stopped and resources are cleaned up

    if (stats_thread_.joinable()) {
        stats_thread_.join();  // Wait for the thread to finish
    }

    CloseStatsFile();  // Flush and close all CSV files before destruction

    RTC_LOG(LS_INFO) << "RTCStatsCollector destroyed.";
}

bool RTCStatsCollector::Start(
    const std::string& foldername,
    rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection) {


    RTC_LOG(LS_INFO) << "RTCStatsCollector starts.";

    if (is_running_) {
        RTC_LOG(LS_INFO) << "RTCStatsCollector is already running.";
        return true;
    }

    if (!peer_connection) {
        RTC_LOG(LS_ERROR) << "No peer connection provided. Cannot start stats collection.";
        return false;
    }

    if (!OpenStatsFile(foldername)) {
        RTC_LOG(LS_ERROR) << "Failed to open stats file. Cannot start stats collection.";
        return false;
    }

    peer_connection_ = peer_connection;
    should_collect_ = true;
    is_running_ = true;

    stats_thread_ = std::thread(&RTCStatsCollector::ThreadLoop, this);

    return true;
}

void RTCStatsCollector::Stop() {
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        should_collect_ = false;  // Signal the thread to stop
    }
    stop_cv_.notify_all(); // Wake up the thread if it's waiting
    RTC_LOG(LS_INFO) << "RTCStatsCollector stopped.";
}

bool RTCStatsCollector::OpenStatsFile(const std::string& foldername) {
    RTC_LOG(LS_INFO) << "Opening stats files in folder: " << foldername;

    // Construct file paths for per-frame and average stats files
    std::string per_frame_filename = foldername + "/per_frame_stats.csv";
    std::string average_filename = foldername + "/average_stats.csv";

    // Open average stats file
    average_stats_file_.open(average_filename);
    if (!average_stats_file_.is_open()) {
        RTC_LOG(LS_ERROR) << "Failed to open average stats file: " << average_filename;
        per_frame_stats_file_.close();  // Cleanup already opened file
        return false;
    }
    RTC_LOG(LS_INFO) << "Average stats file opened successfully: " << average_filename;
    average_stats_file_ << "timestamp_ms,frames_decoded,frames_dropped,frames_received,"
                       "framerate,jitter_buffer_delay_ms,min_playout_delay_ms,video_width,video_height,"
                       "total_decode_time_ms,total_bytes_received,bitrates,overall_avg_bitrates,"
                       "sender_period_bitrate,sender_overall_bitrate,decoder_implementation,"
                       "period_packets_received,period_packets_lost,loss_ratio,"
                       "period_packets_discarded,"
                       "period_fec_packets_received,period_fec_packets_discarded,"
                       "period_packets_repaired,"
                       "period_fec_bytes_recv,period_fec_bytes_sent,fec_byte_ratio,"
                       "period_retx_pkts_recv,period_retx_bytes_recv,"
                       "period_retx_pkts_sent,period_retx_bytes_sent,"
                       "retransmission_ratio,"
                       "period_freeze_count,period_freeze_duration_ms,"
                       "freeze_time_ratio\n";
    average_stats_file_.flush();

    // Prepare SCTP throughput logging
    for (auto& entry : persistent_stats_.data_channel_logs_) {
        if (entry.second.csv_file.is_open()) {
            entry.second.csv_file.close();
        }
    }
    persistent_stats_.data_channel_logs_.clear();
    for (auto& entry : persistent_stats_.transport_logs_) {
        if (entry.second.csv_file.is_open()) {
            entry.second.csv_file.close();
        }
    }
    persistent_stats_.transport_logs_.clear();
    persistent_stats_.log_directory_ = foldername;

    return true;
}

void RTCStatsCollector::CloseStatsFile() {
    if (per_frame_stats_file_.is_open()) {
        RTC_LOG(LS_INFO) << "Closing per-frame stats file.";
        per_frame_stats_file_.flush();
        per_frame_stats_file_.close();
    }

    if (average_stats_file_.is_open()) {
        RTC_LOG(LS_INFO) << "Closing average stats file.";
        average_stats_file_.flush();
        average_stats_file_.close();
    }

    for (auto& entry : persistent_stats_.data_channel_logs_) {
        if (entry.second.csv_file.is_open()) {
            entry.second.csv_file.flush();
            entry.second.csv_file.close();
        }
    }
    for (auto& entry : persistent_stats_.transport_logs_) {
        if (entry.second.csv_file.is_open()) {
            entry.second.csv_file.flush();
            entry.second.csv_file.close();
        }
    }
    persistent_stats_.data_channel_logs_.clear();
    persistent_stats_.transport_logs_.clear();
    persistent_stats_.log_directory_.clear();
}

void RTCStatsCollector::ThreadLoop() {
    std::unique_lock<std::mutex> lock(stats_mutex_);
    while (should_collect_) {
        lock.unlock();  // Explicit unlock
        CollectStats();
        lock.lock();  // Explicit lock before waiting

        stop_cv_.wait_for(lock, std::chrono::milliseconds(kStatsIntervalMs),
                          [this]() { return !should_collect_; });
    }
}

void RTCStatsCollector::CollectStats() {
    if (!peer_connection_) {
        RTC_LOG(LS_WARNING) << "No peer connection. Skipping stats collection.";
        return;
    }

    auto stats_callback = rtc::make_ref_counted<RTCStatsCollectorCallback>(
        per_frame_stats_file_,
        average_stats_file_,
        stats_mutex_,
        persistent_stats_); 

    peer_connection_->GetStats(stats_callback.get());
}
