#ifndef EXAMPLES_PEERCONNECTION_CLIENT_NVENC_H264_NVENC_ENCODER_H_
#define EXAMPLES_PEERCONNECTION_CLIENT_NVENC_H264_NVENC_ENCODER_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "api/video/encoded_image.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "modules/video_coding/include/video_codec_interface.h"

namespace webrtc {

class H264NvencEncoder : public VideoEncoder {
 public:
  H264NvencEncoder();
  ~H264NvencEncoder() override;

  int InitEncode(const VideoCodec* codec_settings,
                  const VideoEncoder::Settings& settings) override;
  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override;
  int32_t Release() override;
  int32_t Encode(const VideoFrame& frame,
                  const std::vector<VideoFrameType>* frame_types) override;
  void SetRates(const RateControlParameters& parameters) override;
  EncoderInfo GetEncoderInfo() const override;

 private:
  bool OpenSession();
  bool ConfigureEncoder(int width, int height, int fps, uint32_t bitrate_bps);
  void ApplyRcLocked(uint32_t bitrate_bps, int fps, bool mae_on);
  // force_idr is what makes a resolution change decodable: the receiver cannot
  // continue a stream whose SPS dimensions just changed from an inter frame.
  bool ReconfigureLocked(bool force_idr);
  // Exactly-sized NV12 input buffer. Encode() writes the UV plane at
  // dst + pitch * height_, which is only correct while the buffer matches the
  // current picture, so a resolution change reallocates rather than reusing a
  // max-sized buffer. nvEncodeAPI.h:2156-2159 requires the max dimension for
  // OUTPUT buffers only.
  bool CreateInputBufferLocked(int width, int height);
  // Move the live session to width x height (must be within the session max).
  bool ApplyResolutionLocked(int width, int height);
  void DestroySession();

  EncodedImageCallback* callback_ = nullptr;
  H264BitstreamParser bitstream_parser_;

  std::mutex mu_;
  bool inited_ = false;
  bool mae_active_ = false;
  int width_ = 0;
  int height_ = 0;
  // Session ceiling, fixed at InitEncode. NV_ENC_INITIALIZE_PARAMS::
  // maxEncodeWidth/Height must be non-zero or the driver refuses every
  // resolution change; QualityScaler only ever scales down, so the initial
  // resolution is the ceiling.
  int max_width_ = 0;
  int max_height_ = 0;
  int fps_ = 60;
  uint32_t bitrate_bps_ = 0;
  uint32_t encoded_frames_ = 0;
  uint32_t resolution_changes_ = 0;

  // Opaque NVENC / CUDA handles (void* to keep cuda/nvenc out of the header).
  void* cuda_ctx_ = nullptr;
  void* nvenc_session_ = nullptr;
  void* nvenc_api_ = nullptr;  // NV_ENCODE_API_FUNCTION_LIST*
  void* input_buffer_ = nullptr;
  void* bitstream_buffer_ = nullptr;
  void* encode_config_ = nullptr;       // NV_ENC_CONFIG*
  void* initialize_params_ = nullptr;    // NV_ENC_INITIALIZE_PARAMS*
};

}  // namespace webrtc

#endif  // EXAMPLES_PEERCONNECTION_CLIENT_NVENC_H264_NVENC_ENCODER_H_
