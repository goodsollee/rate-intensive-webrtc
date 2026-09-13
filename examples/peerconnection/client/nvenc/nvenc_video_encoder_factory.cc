#include "examples/peerconnection/client/nvenc/nvenc_video_encoder_factory.h"

#include "examples/peerconnection/client/nvenc/h264_nvenc_encoder.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "rtc_base/logging.h"

namespace webrtc {

std::vector<SdpVideoFormat> NvencH264Formats() {
  return {CreateH264Format(H264Profile::kProfileHigh, H264Level::kLevel5_2, "1"),
          CreateH264Format(H264Profile::kProfileHigh, H264Level::kLevel5_2,
                           "0")};
}

std::vector<SdpVideoFormat> NvencH264EncoderFactory::GetSupportedFormats()
    const {
  return NvencH264Formats();
}

std::unique_ptr<VideoEncoder> NvencH264EncoderFactory::Create(
    const Environment& /*env*/,
    const SdpVideoFormat& /*format*/) {
  return std::make_unique<H264NvencEncoder>();
}

std::vector<SdpVideoFormat> NvencH264DecoderFactory::GetSupportedFormats() const {
  return NvencH264Formats();
}

std::unique_ptr<VideoDecoder> NvencH264DecoderFactory::Create(
    const Environment& /*env*/,
    const SdpVideoFormat& /*format*/) {
  if (!H264Decoder::IsSupported()) {
    RTC_LOG(LS_ERROR) << "KFT NVENC H264Decoder::IsSupported=false";
    return nullptr;
  }
  return H264Decoder::Create();
}

}  // namespace webrtc
