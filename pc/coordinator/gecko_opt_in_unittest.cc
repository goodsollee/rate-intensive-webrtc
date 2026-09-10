/* Copyright 2026 The WebRTC project authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "api/environment/environment_factory.h"
#include "api/rtp_parameters.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "media/engine/webrtc_video_engine.h"
#include "modules/rtp_rtcp/source/rtp_header_extensions.h"
#include "modules/rtp_rtcp/source/rtp_generic_frame_descriptor_extension.h"
#include "modules/rtp_rtcp/source/rtp_packet_received.h"
#include "modules/rtp_rtcp/source/rtp_rtcp_impl2.h"
#include "modules/rtp_rtcp/source/rtp_sender_video.h"
#include "modules/rtp_rtcp/source/rtp_sender.h"
#include "rtc_base/rate_limiter.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/clock.h"
#include "test/explicit_key_value_config.h"
#include "test/gtest.h"

namespace webrtc {
namespace {

// Run these tests in distinct unset/0/1 processes, matching the process-fixed
// production controller. Expectations do not consult the implementation gate.
bool GeckoRequestedByTestProcess() {
  const char* value = std::getenv("GECKO_ENABLED");
  return value && std::strcmp(value, "1") == 0;
}

TEST(GeckoOptInTest, CapabilityAndSubsequentIdsMatchProcessArm) {
  test::ExplicitKeyValueConfig trials("");
  cricket::WebRtcVideoEngine engine(nullptr, nullptr, trials);
  const auto capabilities = engine.GetRtpHeaderExtensions();
  bool found_gecko = false;
  bool found_timing = false;
  bool found_pdu = false;
  for (const auto& capability : capabilities) {
    if (capability.uri == RtpExtension::kGeckoFlagUri) {
      found_gecko = true;
      EXPECT_EQ(capability.preferred_id, 8);
    }
    if (capability.uri == RtpExtension::kVideoTimingUri) {
      found_timing = true;
      EXPECT_EQ(capability.preferred_id, GeckoRequestedByTestProcess() ? 9 : 8);
    }
    if (capability.uri == RtpExtension::kPduSetInfoUri) {
      found_pdu = true;
      EXPECT_EQ(capability.preferred_id, 7);
    }
  }
  EXPECT_EQ(found_gecko, GeckoRequestedByTestProcess());
  EXPECT_TRUE(found_timing);
  EXPECT_TRUE(found_pdu);
}

class CapturingTransport : public Transport {
 public:
  bool SendRtp(rtc::ArrayView<const uint8_t> data,
               const PacketOptions&) override {
    packets.emplace_back(data.begin(), data.end());
    return true;
  }
  bool SendRtcp(rtc::ArrayView<const uint8_t>) override { return true; }
  std::vector<std::vector<uint8_t>> packets;
};

std::vector<std::vector<uint8_t>> SendFrame(bool register_gecko) {
  SimulatedClock clock(Timestamp::Millis(123456789));
  const auto env = CreateEnvironment(&clock);
  CapturingTransport transport;
  RateLimiter retransmission_rate_limiter(&clock, 1000);
  ModuleRtpRtcpImpl2 module(
      env, {.outgoing_transport = &transport,
            .retransmission_rate_limiter = &retransmission_rate_limiter,
            .local_media_ssrc = 725242,
            .rtx_send_ssrc = 912364});
  module.SetSequenceNumber(33);
  module.SetStartTimestamp(0);
  if (register_gecko) {
    module.RegisterRtpHeaderExtension(GeckoFlagExtension::Uri(), 8);
  }
  RTPSenderVideo::Config config;
  config.clock = &clock;
  config.rtp_sender = module.RtpSender();
  config.field_trials = &env.field_trials();
  RTPSenderVideo sender(config);
  uint8_t frame[3000] = {};
  RTPVideoHeader header;
  header.frame_type = VideoFrameType::kVideoFrameKey;
  EXPECT_TRUE(sender.SendVideo(
      100, VideoCodecType::kVideoCodecGeneric, 10, clock.CurrentTime(), frame,
      sizeof(frame), header, TimeDelta::Millis(125), {}));
  return transport.packets;
}

TEST(GeckoOptInTest, RegisteredHeaderIsAbsentAndBytesUnchangedWhileOff) {
  rtc::AutoThread main_thread;
  const auto without_registration = SendFrame(false);
  const auto with_registration = SendFrame(true);
  ASSERT_FALSE(with_registration.empty());
  RtpHeaderExtensionMap extensions;
  extensions.Register<GeckoFlagExtension>(8);
  for (const auto& bytes : with_registration) {
    RtpPacketReceived packet(&extensions);
    ASSERT_TRUE(packet.Parse(bytes));
    EXPECT_EQ(packet.HasExtension<GeckoFlagExtension>(), GeckoRequestedByTestProcess());
  }
  if (!GeckoRequestedByTestProcess()) {
    // Covers complete RTP bytes and packetization boundaries, even when a
    // remote offer or manual caller registered Gecko while local arm is OFF.
    EXPECT_EQ(with_registration, without_registration);
  } else {
    EXPECT_NE(with_registration, without_registration);
  }
}

TEST(GeckoOptInTest, ExtensionSizeListKeepsGenericAndExcludesOnlyDisabledGecko) {
  bool found_generic = false;
  bool found_gecko = false;
  for (const auto& extension : RTPSender::VideoExtensionSizes()) {
    if (extension.type == RtpGenericFrameDescriptorExtension00::kId) found_generic = true;
    if (extension.type == GeckoFlagExtension::kId) found_gecko = true;
  }
  EXPECT_TRUE(found_generic);
  EXPECT_EQ(found_gecko, GeckoRequestedByTestProcess());
}

TEST(GeckoOptInTest, HeaderOverheadIgnoresDisabledGeckoRegistration) {
  rtc::AutoThread main_thread;
  SimulatedClock clock(Timestamp::Millis(123456789));
  const auto env = CreateEnvironment(&clock);
  CapturingTransport transport;
  RateLimiter limiter(&clock, 1000);
  ModuleRtpRtcpImpl2 module(
      env, {.outgoing_transport = &transport,
            .retransmission_rate_limiter = &limiter,
            .local_media_ssrc = 725242});
  const size_t baseline = module.ExpectedPerPacketOverhead();
  module.RegisterRtpHeaderExtension(GeckoFlagExtension::Uri(), 8);
  const size_t gecko = module.ExpectedPerPacketOverhead();
  if (GeckoRequestedByTestProcess()) {
    EXPECT_GT(gecko, baseline);
  } else {
    EXPECT_EQ(gecko, baseline);
  }
  module.RegisterRtpHeaderExtension(RtpGenericFrameDescriptorExtension00::Uri(), 9);
  EXPECT_GT(module.ExpectedPerPacketOverhead(), gecko);
}

}  // namespace
}  // namespace webrtc
