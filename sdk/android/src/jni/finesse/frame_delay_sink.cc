#include "frame_delay_sink.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

namespace finesse {

FrameDelaySink::FrameDelaySink(webrtc::VideoTrackInterface* track)
    : track_(track) {
  track_->AddOrUpdateSink(this, rtc::VideoSinkWants());
}

FrameDelaySink::~FrameDelaySink() {
  track_->RemoveSink(this);
  if (csv_.is_open())
    csv_.close();
}

void FrameDelaySink::OnFrame(const webrtc::VideoFrame& frame) {
  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();

  const auto& ft = frame.frame_timing();

  const int64_t receive_start_ms = ft.first_packet_arrival_timestamp;
  const int64_t receive_finish_ms = ft.last_packet_arrival_timestamp;
  const int64_t decode_start_ms = receive_finish_ms;
  const int64_t decode_finish_ms =
      receive_finish_ms > 0 ? receive_finish_ms + ft.decode_ms : 0;
  const int64_t jitter_buffer_ms = -1;
  const bool timing_valid = receive_start_ms > 0 && receive_finish_ms > 0;

  int64_t rtp_ms = static_cast<int64_t>(frame.rtp_timestamp()) / 90;

  int64_t inter_frame_delay_ms = 0;
  if (last_receive_finish_ms_ > 0 && receive_finish_ms > 0) {
    inter_frame_delay_ms = receive_finish_ms - last_receive_finish_ms_;
  }
  if (receive_finish_ms > 0) {
    last_receive_finish_ms_ = receive_finish_ms;
  }

  if (!offset_initialized_ && receive_start_ms > 0) {
    int64_t rtt_ms = ft.network_delay_ms;
    if (rtt_ms <= 0) {
      const char* lat_env = std::getenv("NETWORK_LATENCY_MS");
      if (lat_env && std::strlen(lat_env) > 0) {
        rtt_ms = std::atol(lat_env) * 2;
      }
    }
    if (rtt_ms > 0) {
      rtp_time_offset_ =
          receive_start_ms - (rtt_ms / 2 - 5) - (rtp_ms + ft.encode_ms);
      offset_initialized_ = true;
    }
  }

  int64_t estimated_network_ms = -1;
  int64_t e2e_delay_ms = -1;
  if (offset_initialized_) {
    int64_t estimated_departure = rtp_ms + rtp_time_offset_ + ft.encode_ms;
    estimated_network_ms = receive_finish_ms - estimated_departure;
    e2e_delay_ms = decode_finish_ms - (rtp_ms + rtp_time_offset_);
  }

  if (!csv_.is_open()) {
    const char* dir = std::getenv("FRAME_DELAY_CSV_DIR");
    if (!dir)
      dir = std::getenv("UNIFIED_CSV_DIR");
    if (dir && std::strlen(dir) > 0) {
      std::string path = std::string(dir) + "/frame_delay.csv";
      csv_.open(path, std::ios::out | std::ios::trunc);
      if (csv_.is_open()) {
        csv_ << "wall_ms,rtp_timestamp,width,height,"
                "receive_start_ms,receive_finish_ms,"
                "decode_start_ms,decode_finish_ms,"
                "frame_construction_ms,jitter_buffer_ms,decode_ms,"
                "inter_frame_delay_ms,"
                "encode_ms,pacing_ms,"
                "network_delay_ms,estimated_network_ms,e2e_delay_ms,"
                "is_keyframe,timing_valid\n";
      }
    }
  }

  if (csv_.is_open()) {
    csv_ << now_ms << ","
         << frame.rtp_timestamp() << ","
         << frame.width() << "," << frame.height() << ","
         << receive_start_ms << "," << receive_finish_ms << ","
         << decode_start_ms << "," << decode_finish_ms << ","
         << ft.frame_construction_delay_ms << ","
         << jitter_buffer_ms << "," << ft.decode_ms << ","
         << inter_frame_delay_ms << ","
         << ft.encode_ms << "," << ft.pacing_ms << ","
         << ft.network_delay_ms << "," << estimated_network_ms << ","
         << e2e_delay_ms << ","
         << (ft.is_keyframe ? 1 : 0) << ","
         << (timing_valid ? 1 : 0) << "\n";
    if (++flush_counter_ >= 30) {
      csv_.flush();
      flush_counter_ = 0;
    }
  }
}

}  // namespace finesse
