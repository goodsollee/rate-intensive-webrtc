#include "examples/peerconnection/client/nvenc/h264_nvenc_encoder.h"

#include <cuda.h>
#include <algorithm>
#include <cstring>
#include <string>

#include "api/array_view.h"

#include "examples/peerconnection/client/nvenc/nvEncodeAPI.h"
#include "examples/peerconnection/client/nvenc/nvenc_env.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_codec_type.h"
#include "api/video/video_frame_type.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rtc_base/ref_counted_object.h"
#include "third_party/libyuv/include/libyuv/convert.h"

namespace webrtc {
namespace {

#define NVENC_OK(s) ((s) == NV_ENC_SUCCESS)

NV_ENCODE_API_FUNCTION_LIST* Api(void* p) {
  return static_cast<NV_ENCODE_API_FUNCTION_LIST*>(p);
}

}  // namespace

H264NvencEncoder::H264NvencEncoder() = default;

H264NvencEncoder::~H264NvencEncoder() {
  Release();
}

int H264NvencEncoder::InitEncode(const VideoCodec* codec_settings,
                                 const VideoEncoder::Settings& /*settings*/) {
  if (!codec_settings || codec_settings->codecType != kVideoCodecH264) {
    RTC_LOG(LS_ERROR) << "KFT NVENC InitEncode: expected H264, got "
                       << (codec_settings
                               ? static_cast<int>(codec_settings->codecType)
                               : -1);
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  std::lock_guard<std::mutex> lock(mu_);
  DestroySession();
  width_ = codec_settings->width;
  height_ = codec_settings->height;
  fps_ = std::max(1, static_cast<int>(codec_settings->maxFramerate));
  uint32_t br = codec_settings->startBitrate > 0
                    ? codec_settings->startBitrate * 1000u
                    : codec_settings->maxBitrate * 1000u;
  if (br == 0)
    br = 40u * 1000u * 1000u;
  bitrate_bps_ = br;
  if (!OpenSession())
    return WEBRTC_VIDEO_CODEC_ERROR;
  if (!ConfigureEncoder(width_, height_, fps_, bitrate_bps_)) {
    DestroySession();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  inited_ = true;
  encoded_frames_ = 0;
  RTC_LOG(LS_ERROR) << "KFT NVENC InitEncode " << width_ << "x" << height_
                    << "@" << fps_ << " bitrate_bps=" << bitrate_bps_;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t H264NvencEncoder::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  std::lock_guard<std::mutex> lock(mu_);
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t H264NvencEncoder::Release() {
  std::lock_guard<std::mutex> lock(mu_);
  DestroySession();
  inited_ = false;
  return WEBRTC_VIDEO_CODEC_OK;
}

void H264NvencEncoder::SetRates(const RateControlParameters& parameters) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!inited_)
    return;
  if (parameters.framerate_fps >= 1.0)
    fps_ = std::max(1, static_cast<int>(parameters.framerate_fps + 0.5));
  uint32_t br = parameters.bitrate.get_sum_bps();
  if (br == 0)
    br = parameters.target_bitrate.get_sum_bps();
  if (br > 0)
    bitrate_bps_ = br;
  const bool mae_on =
      nvenc::MaeEnabled() && parameters.is_overused_for_encoder > 1.0;
  ApplyRcLocked(bitrate_bps_, fps_, mae_on);
  if (mae_on && !mae_active_) {
    mae_active_ = true;
    RTC_LOG(LS_ERROR) << "KFTF MAE on ratio="
                      << parameters.is_overused_for_encoder << " fps=" << fps_
                      << " vbv_ms=" << (1000 / fps_);
  } else if (!mae_on && mae_active_) {
    mae_active_ = false;
    RTC_LOG(LS_ERROR) << "KFTF MAE off ratio="
                      << parameters.is_overused_for_encoder;
  }
  // Rate/fps only: no IDR here, so the MAE path costs the same as before.
  ReconfigureLocked(/*force_idr=*/false);
}

// QP scaling thresholds. Same numbers as the stock H.264 encoder
// (modules/video_coding/codecs/h264/h264_encoder_impl.cc:51-52, used at :717)
// and they are on the same scale here for a concrete reason: image.qp_ below is
// H264BitstreamParser::GetLastSliceQp(), the identical parser that path uses at
// :548, so this is a parsed H.264 slice QP and not an NVENC-private number.
static const int kLowNvencH264QpThreshold = 24;
static const int kHighNvencH264QpThreshold = 37;

VideoEncoder::EncoderInfo H264NvencEncoder::GetEncoderInfo() const {
  EncoderInfo info;
  // Was kOff, which stops VideoStreamEncoderResourceManager::
  // ConfigureQualityScaler at its `scaling_settings.thresholds.has_value()`
  // test (video/adaptation/video_stream_encoder_resource_manager.cc:565), so no
  // QualityScaler was ever created. is_qp_trusted is left unset (defaults true)
  // and is_hardware_accelerated does not gate that decision.
  info.scaling_settings = VideoEncoder::ScalingSettings(
      kLowNvencH264QpThreshold, kHighNvencH264QpThreshold);
  info.requested_resolution_alignment = 2;
  info.apply_alignment_to_all_simulcast_layers = false;
  info.supports_native_handle = false;
  info.implementation_name = "NVIDIA_NVENC_H264";
  info.has_trusted_rate_controller = true;
  info.is_hardware_accelerated = true;
  info.supports_simulcast = false;
  info.preferred_pixel_formats.push_back(VideoFrameBuffer::Type::kI420);
  info.fps_allocation[0].push_back(EncoderInfo::kMaxFramerateFraction);
  return info;
}

bool H264NvencEncoder::OpenSession() {
  if (cuInit(0) != CUDA_SUCCESS) {
    RTC_LOG(LS_ERROR) << "KFT NVENC cuInit failed";
    return false;
  }
  CUdevice dev = 0;
  if (cuDeviceGet(&dev, 0) != CUDA_SUCCESS) {
    RTC_LOG(LS_ERROR) << "KFT NVENC cuDeviceGet failed";
    return false;
  }
  CUcontext ctx = nullptr;
  if (cuCtxCreate(&ctx, 0, dev) != CUDA_SUCCESS) {
    RTC_LOG(LS_ERROR) << "KFT NVENC cuCtxCreate failed";
    return false;
  }
  cuda_ctx_ = ctx;

  auto* api = new NV_ENCODE_API_FUNCTION_LIST();
  std::memset(api, 0, sizeof(*api));
  api->version = NV_ENCODE_API_FUNCTION_LIST_VER;
  NVENCSTATUS st = NvEncodeAPICreateInstance(api);
  if (!NVENC_OK(st)) {
    RTC_LOG(LS_ERROR) << "KFT NVENC NvEncodeAPICreateInstance " << st;
    delete api;
    return false;
  }
  nvenc_api_ = api;

  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open = {};
  open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
  open.device = ctx;
  open.apiVersion = NVENCAPI_VERSION;
  void* session = nullptr;
  st = api->nvEncOpenEncodeSessionEx(&open, &session);
  if (!NVENC_OK(st)) {
    RTC_LOG(LS_ERROR) << "KFT NVENC OpenEncodeSessionEx " << st;
    return false;
  }
  nvenc_session_ = session;
  return true;
}

void H264NvencEncoder::ApplyRcLocked(uint32_t bitrate_bps,
                                     int fps,
                                     bool mae_on) {
  auto* cfg = static_cast<NV_ENC_CONFIG*>(encode_config_);
  if (!cfg)
    return;
  fps = std::max(1, fps);
  cfg->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  cfg->rcParams.averageBitRate = bitrate_bps;
  cfg->rcParams.maxBitRate = bitrate_bps;
  cfg->rcParams.zeroReorderDelay = 1;
  cfg->rcParams.enableLookahead = 0;
  // MAE: one frame of VBV (bits). Off: one second, same as stock x264-ish.
  const uint32_t vbv_bits =
      mae_on ? std::max(1u, bitrate_bps / static_cast<uint32_t>(fps))
             : bitrate_bps;
  cfg->rcParams.vbvBufferSize = vbv_bits;
  cfg->rcParams.vbvInitialDelay = vbv_bits;
}

bool H264NvencEncoder::ConfigureEncoder(int width,
                                         int height,
                                         int fps,
                                         uint32_t bitrate_bps) {
  auto* api = Api(nvenc_api_);
  void* session = nvenc_session_;
  fps = std::max(1, fps);

  NV_ENC_PRESET_CONFIG preset = {};
  preset.version = NV_ENC_PRESET_CONFIG_VER;
  preset.presetCfg.version = NV_ENC_CONFIG_VER;
  NVENCSTATUS st = api->nvEncGetEncodePresetConfigEx(
      session, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID,
      NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset);
  if (!NVENC_OK(st)) {
    RTC_LOG(LS_WARNING) << "KFT NVENC GetEncodePresetConfigEx " << st
                        << ", falling back to GetEncodePresetConfig";
    st = api->nvEncGetEncodePresetConfig(session, NV_ENC_CODEC_H264_GUID,
                                        NV_ENC_PRESET_P1_GUID, &preset);
    if (!NVENC_OK(st)) {
      RTC_LOG(LS_ERROR) << "KFT NVENC GetEncodePresetConfig " << st;
      return false;
    }
  }

  auto* cfg = new NV_ENC_CONFIG(preset.presetCfg);
  cfg->version = NV_ENC_CONFIG_VER;
  cfg->profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
  cfg->gopLength = static_cast<uint32_t>(fps * 2);
  cfg->frameIntervalP = 1;  // IPPPP, no B frames
  cfg->encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_H264_52;
  cfg->encodeCodecConfig.h264Config.repeatSPSPPS = 1;
  cfg->encodeCodecConfig.h264Config.disableSPSPPS = 0;
  cfg->encodeCodecConfig.h264Config.idrPeriod = cfg->gopLength;
  cfg->encodeCodecConfig.h264Config.chromaFormatIDC = 1;
  // 8 slices so the receiver's FFmpeg slice threads can split one 4K
  // picture. sliceMode=0 + data=0 is a single slice and thread_count>1
  // does nothing.
  cfg->encodeCodecConfig.h264Config.sliceMode = 3;
  cfg->encodeCodecConfig.h264Config.sliceModeData = 8;
  encode_config_ = cfg;
  ApplyRcLocked(bitrate_bps, fps, /*mae_on=*/false);

  auto* init = new NV_ENC_INITIALIZE_PARAMS();
  std::memset(init, 0, sizeof(*init));
  init->version = NV_ENC_INITIALIZE_PARAMS_VER;
  init->encodeGUID = NV_ENC_CODEC_H264_GUID;
  init->presetGUID = NV_ENC_PRESET_P1_GUID;
  init->encodeWidth = width;
  init->encodeHeight = height;
  init->darWidth = width;
  init->darHeight = height;
  // nvEncodeAPI.h:2156-2159: "If set to 0, Encoder will not allow dynamic
  // resolution change", and :4223 repeats that a resolution change is possible
  // only if these were set at session creation. Leaving them 0 is what pinned
  // this encoder to one resolution. QualityScaler only scales down, so the
  // session opens at its ceiling.
  max_width_ = width;
  max_height_ = height;
  init->maxEncodeWidth = static_cast<uint32_t>(width);
  init->maxEncodeHeight = static_cast<uint32_t>(height);
  init->frameRateNum = fps;
  init->frameRateDen = 1;
  init->enableEncodeAsync = 0;
  init->enablePTD = 1;
  init->tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
  init->encodeConfig = cfg;
  initialize_params_ = init;

  st = api->nvEncInitializeEncoder(session, init);
  if (!NVENC_OK(st)) {
    RTC_LOG(LS_WARNING) << "KFT NVENC InitializeEncoder slices=8 failed " << st
                        << ", retrying single slice";
    cfg->encodeCodecConfig.h264Config.sliceMode = 0;
    cfg->encodeCodecConfig.h264Config.sliceModeData = 0;
    st = api->nvEncInitializeEncoder(session, init);
  }
  if (!NVENC_OK(st)) {
    RTC_LOG(LS_ERROR) << "KFT NVENC InitializeEncoder " << st << " "
                      << (api->nvEncGetLastErrorString
                              ? api->nvEncGetLastErrorString(session)
                              : "");
    return false;
  }
  RTC_LOG(LS_ERROR) << "KFT NVENC slices="
                   << cfg->encodeCodecConfig.h264Config.sliceModeData
                   << " sliceMode="
                   << cfg->encodeCodecConfig.h264Config.sliceMode;

  if (!CreateInputBufferLocked(width, height))
    return false;

  // size stays 0: the driver sizes the bitstream buffer, and now that
  // maxEncodeWidth/Height are set it sizes it for the session ceiling, which is
  // what nvEncodeAPI.h:2156-2159 asks of output buffers.
  NV_ENC_CREATE_BITSTREAM_BUFFER outbuf = {};
  outbuf.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
  st = api->nvEncCreateBitstreamBuffer(session, &outbuf);
  if (!NVENC_OK(st)) {
    RTC_LOG(LS_ERROR) << "KFT NVENC CreateBitstreamBuffer " << st;
    return false;
  }
  bitstream_buffer_ = outbuf.bitstreamBuffer;
  return true;
}

bool H264NvencEncoder::CreateInputBufferLocked(int width, int height) {
  auto* api = Api(nvenc_api_);
  if (!api || !nvenc_session_)
    return false;
  if (input_buffer_) {
    api->nvEncDestroyInputBuffer(nvenc_session_, input_buffer_);
    input_buffer_ = nullptr;
  }
  NV_ENC_CREATE_INPUT_BUFFER inbuf = {};
  inbuf.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
  inbuf.width = width;
  inbuf.height = height;
  inbuf.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
  NVENCSTATUS st = api->nvEncCreateInputBuffer(nvenc_session_, &inbuf);
  if (!NVENC_OK(st)) {
    RTC_LOG(LS_ERROR) << "KFT NVENC CreateInputBuffer " << width << "x"
                      << height << " " << st;
    return false;
  }
  input_buffer_ = inbuf.inputBuffer;
  return true;
}

bool H264NvencEncoder::ReconfigureLocked(bool force_idr) {
  auto* api = Api(nvenc_api_);
  auto* init = static_cast<NV_ENC_INITIALIZE_PARAMS*>(initialize_params_);
  if (!api || !nvenc_session_ || !init)
    return false;
  init->frameRateNum = fps_;
  init->frameRateDen = 1;
  // Carry the live resolution: without these three the driver keeps encoding at
  // the dimensions from session creation no matter what Encode() feeds it.
  init->encodeWidth = static_cast<uint32_t>(width_);
  init->encodeHeight = static_cast<uint32_t>(height_);
  init->darWidth = static_cast<uint32_t>(width_);
  init->darHeight = static_cast<uint32_t>(height_);
  NV_ENC_RECONFIGURE_PARAMS rec = {};
  rec.version = NV_ENC_RECONFIGURE_PARAMS_VER;
  rec.reInitEncodeParams = *init;
  rec.resetEncoder = 0;
  rec.forceIDR = force_idr ? 1 : 0;
  NVENCSTATUS st = api->nvEncReconfigureEncoder(nvenc_session_, &rec);
  if (!NVENC_OK(st)) {
    RTC_LOG(LS_WARNING) << "KFT NVENC ReconfigureEncoder " << st;
    return false;
  }
  return true;
}

bool H264NvencEncoder::ApplyResolutionLocked(int width, int height) {
  if (width <= 0 || height <= 0)
    return false;
  if (width > max_width_ || height > max_height_) {
    // Above the ceiling the session cannot be reconfigured at all; the caller
    // keeps the old refuse-the-frame behaviour.
    return false;
  }
  const int from_w = width_;
  const int from_h = height_;
  width_ = width;
  height_ = height;
  if (!CreateInputBufferLocked(width_, height_) ||
      !ReconfigureLocked(/*force_idr=*/true)) {
    width_ = from_w;
    height_ = from_h;
    // Put the buffer back so the stream keeps running at the old size instead
    // of dying on a half-applied change.
    CreateInputBufferLocked(width_, height_);
    RTC_LOG(LS_ERROR) << "KFT NVENC resolution change FAILED " << from_w << "x"
                      << from_h << " -> " << width << "x" << height
                      << ", staying at " << width_ << "x" << height_;
    return false;
  }
  ++resolution_changes_;
  RTC_LOG(LS_ERROR) << "KFT NVENC resolution " << from_w << "x" << from_h
                    << " -> " << width_ << "x" << height_ << " max="
                    << max_width_ << "x" << max_height_ << " n_changes="
                    << resolution_changes_ << " forceIDR=1";
  return true;
}

void H264NvencEncoder::DestroySession() {
  auto* api = Api(nvenc_api_);
  if (api && nvenc_session_) {
    if (input_buffer_)
      api->nvEncDestroyInputBuffer(nvenc_session_, input_buffer_);
    if (bitstream_buffer_)
      api->nvEncDestroyBitstreamBuffer(nvenc_session_, bitstream_buffer_);
    api->nvEncDestroyEncoder(nvenc_session_);
  }
  input_buffer_ = nullptr;
  bitstream_buffer_ = nullptr;
  nvenc_session_ = nullptr;
  delete static_cast<NV_ENC_CONFIG*>(encode_config_);
  encode_config_ = nullptr;
  delete static_cast<NV_ENC_INITIALIZE_PARAMS*>(initialize_params_);
  initialize_params_ = nullptr;
  delete api;
  nvenc_api_ = nullptr;
  if (cuda_ctx_) {
    cuCtxDestroy(static_cast<CUcontext>(cuda_ctx_));
    cuda_ctx_ = nullptr;
  }
}

int32_t H264NvencEncoder::Encode(
    const VideoFrame& frame,
    const std::vector<VideoFrameType>* frame_types) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!inited_ || !callback_)
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

  rtc::scoped_refptr<I420BufferInterface> i420 =
      frame.video_frame_buffer()->ToI420();
  if (!i420)
    return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
  if (i420->width() != width_ || i420->height() != height_) {
    // A different size is the framework asking for a resolution change (the
    // QualityScaler path), not a bad frame. Follow it while it fits the session
    // ceiling; only refuse what the driver genuinely cannot encode.
    if (!ApplyResolutionLocked(i420->width(), i420->height())) {
      RTC_LOG(LS_ERROR) << "KFT NVENC Encode size mismatch got "
                        << i420->width() << "x" << i420->height() << " want "
                        << width_ << "x" << height_ << " max=" << max_width_
                        << "x" << max_height_;
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
  }

  auto* api = Api(nvenc_api_);
  NV_ENC_LOCK_INPUT_BUFFER lock_in = {};
  lock_in.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
  lock_in.inputBuffer = input_buffer_;
  NVENCSTATUS st = api->nvEncLockInputBuffer(nvenc_session_, &lock_in);
  if (!NVENC_OK(st)) {
    RTC_LOG(LS_ERROR) << "KFT NVENC LockInputBuffer " << st;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  auto* dst = static_cast<uint8_t*>(lock_in.bufferDataPtr);
  const int pitch = static_cast<int>(lock_in.pitch);
  libyuv::I420ToNV12(i420->DataY(), i420->StrideY(), i420->DataU(),
                     i420->StrideU(), i420->DataV(), i420->StrideV(), dst,
                     pitch, dst + pitch * height_, pitch, width_, height_);
  api->nvEncUnlockInputBuffer(nvenc_session_, input_buffer_);

  bool force_idr = false;
  if (frame_types) {
    for (auto t : *frame_types) {
      if (t == VideoFrameType::kVideoFrameKey)
        force_idr = true;
    }
  }

  NV_ENC_PIC_PARAMS pic = {};
  pic.version = NV_ENC_PIC_PARAMS_VER;
  pic.inputWidth = width_;
  pic.inputHeight = height_;
  pic.inputPitch = pitch;
  pic.inputBuffer = input_buffer_;
  pic.outputBitstream = bitstream_buffer_;
  pic.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
  pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
  pic.pictureType = NV_ENC_PIC_TYPE_P;
  if (force_idr)
    pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
  pic.inputTimeStamp = frame.rtp_timestamp();

  st = api->nvEncEncodePicture(nvenc_session_, &pic);
  if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
    return WEBRTC_VIDEO_CODEC_OK;
  }
  if (!NVENC_OK(st)) {
    RTC_LOG(LS_ERROR) << "KFT NVENC EncodePicture " << st;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  NV_ENC_LOCK_BITSTREAM lock_out = {};
  lock_out.version = NV_ENC_LOCK_BITSTREAM_VER;
  lock_out.outputBitstream = bitstream_buffer_;
  st = api->nvEncLockBitstream(nvenc_session_, &lock_out);
  if (!NVENC_OK(st)) {
    RTC_LOG(LS_ERROR) << "KFT NVENC LockBitstream " << st;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  const size_t nbytes = lock_out.bitstreamSizeInBytes;
  const bool idr = lock_out.pictureType == NV_ENC_PIC_TYPE_IDR;
  EncodedImage image;
  image.SetEncodedData(EncodedImageBuffer::Create(
      static_cast<const uint8_t*>(lock_out.bitstreamBufferPtr), nbytes));
  image._encodedWidth = width_;
  image._encodedHeight = height_;
  image.SetRtpTimestamp(frame.rtp_timestamp());
  image.ntp_time_ms_ = frame.ntp_time_ms();
  image.capture_time_ms_ = frame.render_time_ms();
  image.rotation_ = frame.rotation();
  image.SetColorSpace(frame.color_space());
  image._frameType =
      idr ? VideoFrameType::kVideoFrameKey : VideoFrameType::kVideoFrameDelta;
  bitstream_parser_.ParseBitstream(rtc::ArrayView<const uint8_t>(
      image.data(), image.size()));
  image.qp_ = bitstream_parser_.GetLastSliceQp().value_or(-1);

  CodecSpecificInfo spec;
  spec.codecType = kVideoCodecH264;
  spec.codecSpecific.H264.packetization_mode =
      H264PacketizationMode::NonInterleaved;
  spec.codecSpecific.H264.temporal_idx = kNoTemporalIdx;
  spec.codecSpecific.H264.idr_frame = idr;
  spec.codecSpecific.H264.base_layer_sync = false;

  api->nvEncUnlockBitstream(nvenc_session_, bitstream_buffer_);

  ++encoded_frames_;
  if (encoded_frames_ == 1 || encoded_frames_ % 60 == 0) {
    RTC_LOG(LS_ERROR) << "KFT NVENC frame n=" << encoded_frames_ << " "
                      << width_ << "x" << height_ << " bytes=" << nbytes
                      << (idr ? " IDR" : " P");
  }

  callback_->OnEncodedImage(image, &spec);
  return WEBRTC_VIDEO_CODEC_OK;
}

}  // namespace webrtc
