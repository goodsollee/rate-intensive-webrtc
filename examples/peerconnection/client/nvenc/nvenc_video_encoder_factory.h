#ifndef EXAMPLES_PEERCONNECTION_CLIENT_NVENC_NVENC_VIDEO_ENCODER_FACTORY_H_
#define EXAMPLES_PEERCONNECTION_CLIENT_NVENC_NVENC_VIDEO_ENCODER_FACTORY_H_

#include <memory>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace webrtc {

std::vector<SdpVideoFormat> NvencH264Formats();

class NvencH264EncoderFactory : public VideoEncoderFactory {
 public:
  std::vector<SdpVideoFormat> GetSupportedFormats() const override;
  std::unique_ptr<VideoEncoder> Create(const Environment& env,
                                       const SdpVideoFormat& format) override;
};

// FFmpeg/OpenH264 decode, but SDP must advertise High 5.2 or the offer has
// no video codec (stock decoder list is 3.1).
class NvencH264DecoderFactory : public VideoDecoderFactory {
 public:
  std::vector<SdpVideoFormat> GetSupportedFormats() const override;
  std::unique_ptr<VideoDecoder> Create(const Environment& env,
                                       const SdpVideoFormat& format) override;
};

}  // namespace webrtc

#endif  // EXAMPLES_PEERCONNECTION_CLIENT_NVENC_NVENC_VIDEO_ENCODER_FACTORY_H_
