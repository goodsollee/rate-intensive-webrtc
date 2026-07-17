// frame_timing_logger.h
#ifndef FRAME_TIMING_LOGGER_H_
#define FRAME_TIMING_LOGGER_H_

#include <fstream>
#include <string>
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "rtc_base/logging.h"

class FrameTimingLogger : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  explicit FrameTimingLogger(const std::string& log_dir) 
      : log_file_(log_dir + "/frame_timing.csv") {
    // Write CSV header
    log_file_ << "timestamp,rtp_timestamp,capture_time,encode_start,encode_finish,"
              << "packetization_finish,pacer_exit,network_timestamp,"
              << "network2_timestamp,receive_start,receive_finish,"
              << "decode_start,decode_finish,render_time,is_outlier,is_timer_triggered"
              << std::endl;
  }

  void OnFrame(const webrtc::VideoFrame& frame) override {
    const webrtc::TimingFrameInfo& timing = frame.timing();
    
    if (!timing.IsInvalid()) {
      log_file_ << rtc::TimeMillis() << ","
                << timing.rtp_timestamp << ","
                << timing.capture_time_ms << ","
                << timing.encode_start_ms << ","
                << timing.encode_finish_ms << ","
                << timing.packetization_finish_ms << ","
                << timing.pacer_exit_ms << ","
                << timing.network_timestamp_ms << ","
                << timing.network2_timestamp_ms << ","
                << timing.receive_start_ms << ","
                << timing.receive_finish_ms << ","
                << timing.decode_start_ms << ","
                << timing.decode_finish_ms << ","
                << timing.render_time_ms << ","
                << timing.IsOutlier() << ","
                << timing.IsTimerTriggered()
                << std::endl;
      
      RTC_LOG(LS_INFO) << "Frame timing: "
                       << "Capture-to-render delay: " << timing.EndToEndDelay() << "ms"
                       << " Encoding time: " 
                       << (timing.encode_finish_ms - timing.encode_start_ms) << "ms";
    }
    
    // Forward frame to next sink if exists
    if (next_sink_)
      next_sink_->OnFrame(frame);
  }

  void SetNextSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) {
    next_sink_ = sink;
  }

 private:
  std::ofstream log_file_;
  rtc::VideoSinkInterface<webrtc::VideoFrame>* next_sink_ = nullptr;
};

#endif  // FRAME_TIMING_LOGGER_H_