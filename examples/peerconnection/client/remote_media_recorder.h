/*
 *  Copyright 2024 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef EXAMPLES_PEERCONNECTION_CLIENT_REMOTE_MEDIA_RECORDER_H_
#define EXAMPLES_PEERCONNECTION_CLIENT_REMOTE_MEDIA_RECORDER_H_

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "rtc_base/synchronization/mutex.h"
#include "rtc_base/thread_annotations.h"

namespace webrtc_example {

class Mp4FileWriter;

class RemoteMediaRecorder
    : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  RemoteMediaRecorder(std::unique_ptr<webrtc::VideoEncoder> encoder,
                      std::unique_ptr<Mp4FileWriter> writer,
                      uint32_t timescale = 90000);
  ~RemoteMediaRecorder() override;

  void OnFrame(const webrtc::VideoFrame& frame) override;

  void Stop();

 private:
  class EncoderCallback : public webrtc::EncodedImageCallback {
   public:
    explicit EncoderCallback(RemoteMediaRecorder* owner) : owner_(owner) {}

    Result OnEncodedImage(const webrtc::EncodedImage& encoded_image,
                          const webrtc::CodecSpecificInfo* codec_specific_info) override;

   private:
    RemoteMediaRecorder* owner_;
  };

  bool InitializeEncoder(const webrtc::VideoFrame& frame)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(lock_);
  bool EnsureWriterInitialized(int width, int height)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(lock_);
  void HandleEncodedImage(const webrtc::EncodedImage& image)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(lock_);
  void FinalizeLocked() RTC_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  webrtc::Mutex lock_;
  std::unique_ptr<webrtc::VideoEncoder> encoder_ RTC_GUARDED_BY(lock_);
  std::unique_ptr<Mp4FileWriter> writer_ RTC_GUARDED_BY(lock_);
  EncoderCallback encoder_callback_;
  bool encoder_initialized_ RTC_GUARDED_BY(lock_) = false;
  bool writer_initialized_ RTC_GUARDED_BY(lock_) = false;
  bool closed_ RTC_GUARDED_BY(lock_) = false;

  uint32_t timescale_;
  uint32_t width_ RTC_GUARDED_BY(lock_) = 0;
  uint32_t height_ RTC_GUARDED_BY(lock_) = 0;
  uint32_t target_bitrate_kbps_ RTC_GUARDED_BY(lock_) = 2500;
  uint32_t target_fps_ RTC_GUARDED_BY(lock_) = 30;

  std::vector<uint8_t> sps_ RTC_GUARDED_BY(lock_);
  std::vector<uint8_t> pps_ RTC_GUARDED_BY(lock_);

  std::vector<uint64_t> sample_pts_ RTC_GUARDED_BY(lock_);

  friend class Mp4FileWriter;
};

std::unique_ptr<RemoteMediaRecorder> CreateRemoteRecorder(
    const std::string& output_path,
    uint32_t timescale = 90000);

}  // namespace webrtc_example

#endif  // EXAMPLES_PEERCONNECTION_CLIENT_REMOTE_MEDIA_RECORDER_H_
