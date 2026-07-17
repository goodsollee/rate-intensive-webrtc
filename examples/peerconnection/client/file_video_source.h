#ifndef FILE_VIDEO_SOURCE_H_
#define FILE_VIDEO_SOURCE_H_

#include "api/video/video_frame.h"
#include "api/video/video_source_interface.h"
#include "pc/video_track_source.h"
#include "rtc_tools/video_file_reader.h"
#include "rtc_base/thread.h"
#include "media/base/video_broadcaster.h"
#include "rtc_base/system/rtc_export.h"

class FileVideoSource : public webrtc::VideoTrackSource {
 public:
  static rtc::scoped_refptr<FileVideoSource> Create(
      const std::string& file_path, 
      int target_fps,
      int width = 0,    // Only needed for .yuv files
      int height = 0);  // Only needed for .yuv files

 protected:
  explicit FileVideoSource(rtc::scoped_refptr<webrtc::test::Video> video_file,
                          int target_fps);

 private:
  class FrameGenerator : public rtc::VideoSourceInterface<webrtc::VideoFrame> {
   public:
    FrameGenerator(rtc::scoped_refptr<webrtc::test::Video> video_file,
                  int target_fps);
    ~FrameGenerator() override;

    void AddOrUpdateSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                        const rtc::VideoSinkWants& wants) override;
    void RemoveSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override;

   private:
    void StartTimer();
    void OnFrame();

    rtc::scoped_refptr<webrtc::test::Video> video_file_;
    cricket::VideoBroadcaster broadcaster_;  // Changed from rtc to cricket
    std::unique_ptr<rtc::Thread> thread_;
    webrtc::test::Video::Iterator frame_iterator_;
    int target_fps_;
    bool running_;
  };

  std::unique_ptr<FrameGenerator> frame_generator_;
  rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return frame_generator_.get();
  }
};

#endif  // FILE_VIDEO_SOURCE_H_