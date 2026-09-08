#ifndef ANDROID_RECEIVER_NATIVE_FRAME_DELAY_SINK_H_
#define ANDROID_RECEIVER_NATIVE_FRAME_DELAY_SINK_H_

// Portable copy of GtkMainWnd::FrameDelaySink (linux/main_wnd.{h,cc}).
// GTK-free. Same CSV schema as finesse_score.py. Do not compile this into
// the live Linux peerconnection_client; inject into sdk/android at AAR build.

#include <fstream>
#include <stdint.h>
#include <string>

#include "api/media_stream_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"

namespace finesse {

class FrameDelaySink : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  explicit FrameDelaySink(webrtc::VideoTrackInterface* track);
  ~FrameDelaySink() override;

  void OnFrame(const webrtc::VideoFrame& frame) override;

 private:
  rtc::scoped_refptr<webrtc::VideoTrackInterface> track_;
  std::ofstream csv_;
  int flush_counter_ = 0;
  int64_t last_receive_finish_ms_ = 0;
  bool offset_initialized_ = false;
  int64_t rtp_time_offset_ = 0;
};

}  // namespace finesse

#endif
