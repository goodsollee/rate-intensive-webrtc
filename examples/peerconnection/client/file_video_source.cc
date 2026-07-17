// file_video_source.h
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

// file_video_source.cc
#include "file_video_source.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

FileVideoSource::FrameGenerator::FrameGenerator(
    rtc::scoped_refptr<webrtc::test::Video> video_file,
    int target_fps)
    : video_file_(video_file),
      frame_iterator_(video_file->begin()),
      target_fps_(target_fps),
      running_(false) {
  thread_ = rtc::Thread::CreateWithSocketServer();  // Changed to CreateWithSocketServer
  thread_->Start();
}

FileVideoSource::FrameGenerator::~FrameGenerator() {
  running_ = false;
  thread_->Stop();
}

void FileVideoSource::FrameGenerator::AddOrUpdateSink(
    rtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
    const rtc::VideoSinkWants& wants) {
  broadcaster_.AddOrUpdateSink(sink, wants);
  if (!running_)
    StartTimer();
}

void FileVideoSource::FrameGenerator::RemoveSink(
    rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) {
  broadcaster_.RemoveSink(sink);
  if (!broadcaster_.has_sinks())  // Changed from GetSinks().empty()
    running_ = false;
}

void FileVideoSource::FrameGenerator::StartTimer() {
  running_ = true;
  thread_->PostTask(rtc::ToQueuedTask([this]() {  // Changed to use ToQueuedTask
    while (running_) {
      OnFrame();
      rtc::Thread::Current()->SleepMs(1000 / target_fps_);
    }
  }));
}

void FileVideoSource::FrameGenerator::OnFrame() {
  if (frame_iterator_ == video_file_->end()) {
    frame_iterator_ = video_file_->begin(); // Loop back to start
  }

  webrtc::VideoFrame video_frame =
      webrtc::VideoFrame::Builder()
          .set_video_frame_buffer(*frame_iterator_)
          .set_timestamp_us(rtc::TimeMicros())
          .set_rotation(webrtc::kVideoRotation_0)
          .build();

  broadcaster_.OnFrame(video_frame);
  ++frame_iterator_;
}

FileVideoSource::FileVideoSource(
    rtc::scoped_refptr<webrtc::test::Video> video_file,
    int target_fps)
    : VideoTrackSource(/*remote=*/false) {
  frame_generator_ = std::make_unique<FrameGenerator>(video_file, target_fps);
}

rtc::scoped_refptr<FileVideoSource> FileVideoSource::Create(
    const std::string& file_path,
    int target_fps,
    int width,
    int height) {
  rtc::scoped_refptr<webrtc::test::Video> video_file;
  
  if (width != 0 && height != 0) {
    // If dimensions are provided, try to open as YUV file
    video_file = webrtc::test::OpenYuvFile(file_path, width, height);
  } else {
    // Try to open as either YUV or Y4M file
    video_file = webrtc::test::OpenYuvOrY4mFile(file_path, width, height);
  }

  if (!video_file) {
    RTC_LOG(LS_ERROR) << "Failed to open video file: " << file_path;
    return nullptr;
  }

  return rtc::make_ref_counted<FileVideoSource>(video_file, target_fps);
}