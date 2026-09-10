/* Copyright 2026 The WebRTC project authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "pc/rtp_sctp_coordinator.h"

#include <memory>
#include <optional>

#include "api/transport/network_types.h"
#include "modules/congestion_controller/goog_cc/delay_based_bwe.h"
#include "modules/pacing/pacing_controller.h"
#include "rtc_base/time_utils.h"
#include "rtc_base/fake_clock.h"
#include "test/explicit_key_value_config.h"
#include "test/gtest.h"

namespace webrtc {

// Exercise the real DelayBasedBwe -> coordinator route; no copy of the BUR
// implementation. Fixture friendship permits observing the accumulators.
class PudicaFeedbackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CoordinatorConfig config;
    config.mode = CoordinatorMode::kPudica;
    coordinator_ = std::make_unique<RtpSctpCoordinator>(nullptr, config);
    bwe_ = std::make_unique<DelayBasedBwe>(&trials_, nullptr, nullptr);
    base_us_ = rtc::TimeMicros() - 1'000'000;
  }

  void Feedback(int64_t send_offset, int64_t receive_offset, bool video,
                bool probe, uint32_t timestamp = 9000,
                int64_t intended_span_us = 6000) {
    PacketResult packet;
    packet.sent_packet.sequence_number = ++sequence_;
    packet.sent_packet.pacing_info.pudica_send_id = sequence_;
    packet.sent_packet.pacing_info.pudica_send_time_us = base_us_ + send_offset;
    packet.sent_packet.send_time = Timestamp::Micros(base_us_ + send_offset);
    packet.sent_packet.size = DataSize::Bytes(probe ? 50 : 1200);
    packet.sent_packet.video_media = video;
    packet.sent_packet.rtp_timestamp = timestamp;
    packet.sent_packet.pacing_info.pudica_intended_span_us = intended_span_us;
    if (probe) {
      packet.sent_packet.pacing_info.probe_cluster_id =
          PacingController::kPudicaProbeClusterId;
      packet.sent_packet.pacing_info.pudica_probe_interval_us = 2000;
      packet.sent_packet.pacing_info.pudica_probe_frame_timestamp = timestamp;
    }
    packet.receive_time = Timestamp::Micros(base_us_ + receive_offset);
    TransportPacketsFeedback feedback;
    feedback.feedback_time = packet.receive_time + TimeDelta::Millis(10);
    feedback.packet_feedbacks.push_back(packet);
    bwe_->IncomingPacketFeedbackVector(feedback, std::nullopt, std::nullopt,
                                        std::nullopt, false);
  }

  double Dmin() const { return coordinator_->pudica_d_min_us_; }
  size_t Probes() const { return coordinator_->pudica_probe_results_.size(); }
  int FramePackets() const { return coordinator_->pudica_frame_.frame_packets; }
  int64_t FrameBytes() const { return coordinator_->pudica_frame_.frame_bytes; }
  int64_t IntendedSpan() const {
    return coordinator_->pudica_frame_.intended_span_us;
  }
  double FrameBur() {
    return coordinator_->PudicaComputeFrameBur(rtc::TimeMicros());
  }
  void SetPublishedRate(int64_t bps) {
    coordinator_->pudica_rtp_target_bps_.store(bps);
  }
  void SetDmin(double us) { coordinator_->pudica_d_min_us_ = us; }
  int64_t CommittedRate() const { return coordinator_->pudica_rtp_ctrl_.committed_bps; }
  int64_t OldestAfterFeedback(uint64_t* oldest_id) const {
    return PacingController::PudicaOldestUnackedSendUs(
        coordinator_->pudica_acked_send_id_.load(), oldest_id);
  }
  void UpdateWithoutMeasuredRecv(double bur, int64_t now_us) {
    coordinator_->PudicaUpdateRtpTarget(bur, now_us);
  }
  void SetCommittedRate(int64_t bps) {
    coordinator_->pudica_rtp_ctrl_.committed_bps = bps;
  }

  void CheckDelayedBatchBarrier(bool aimd) {
    rtc::ScopedBaseFakeClock clock;
    clock.SetTime(Timestamp::Micros(2'000'000));
    base_us_ = 1'000'000;
    // Prime a 20 ms propagation floor before the frames under test. A 10 ms
    // queued tail makes utilization BUR choose AI-MD without triggering the
    // raw BUR > 1 fallback. The MI case uses the propagation floor exactly.
    const int64_t extra_delay = aimd ? 10'000 : 0;
    Feedback(-100'000, -80'000, true, false, 6000);
    Feedback(0, 20'000 + extra_delay, true, false, 9000);
    coordinator_->pudica_rtp_ctrl_ = PudicaRtpRateCtrl{};
    coordinator_->pudica_rtp_ctrl_.cfg.ack_ceil_k = 0;
    coordinator_->pudica_rtp_ctrl_.committed_bps = 10'000'000;
    coordinator_->pudica_rtp_target_bps_.store(10'000'000);
    coordinator_->pudica_bur_history_.clear();
    const char* decision = aimd ? "PUD-AIMD" : "PUD-MI";
    Feedback(33'333, 53'333 + extra_delay, true, false, 12000);
    EXPECT_EQ(coordinator_->unified_metrics_.mode, decision);
    const int64_t first_target = CommittedRate();
    Feedback(66'666, 86'666 + extra_delay, true, false, 15000);
    EXPECT_EQ(coordinator_->unified_metrics_.mode, "PUD-HOLD");
    Feedback(99'999, 119'999 + extra_delay, true, false, 18000);
    EXPECT_EQ(coordinator_->unified_metrics_.mode, "PUD-HOLD");
    EXPECT_EQ(CommittedRate(), first_target);

    // A frame sent after the 2 s decision eventually permits the next step.
    // The callback completing the old frame remains HOLD; the next callback
    // completes the post-decision frame and must release the barrier.
    clock.SetTime(Timestamp::Micros(2'100'000));
    Feedback(1'000'001, 1'020'001 + extra_delay, true, false, 21000);
    EXPECT_EQ(coordinator_->unified_metrics_.mode, "PUD-HOLD");
    Feedback(1'033'334, 1'053'334 + extra_delay, true, false, 24000);
    EXPECT_EQ(coordinator_->unified_metrics_.mode, decision);
  }

  test::ExplicitKeyValueConfig trials_{""};
  std::unique_ptr<RtpSctpCoordinator> coordinator_;
  std::unique_ptr<DelayBasedBwe> bwe_;
  int64_t base_us_ = 0;
  int64_t sequence_ = 0;
};

TEST_F(PudicaFeedbackTest, PaddingProbeReachesCorrectionWithoutLoweringVideoDmin) {
  Feedback(0, 20'000, true, false);
  Feedback(10'000, 30'000, true, false);
  ASSERT_DOUBLE_EQ(Dmin(), 20'000);
  // This probe's OWD is too small to be a valid video propagation minimum.
  Feedback(25'000, 31'000, false, true);
  EXPECT_DOUBLE_EQ(Dmin(), 20'000);
  EXPECT_EQ(Probes(), 1u);
  EXPECT_EQ(FramePackets(), 2);
  EXPECT_EQ(FrameBytes(), 2400);
  Feedback(11'000, 32'000, false, true);
  EXPECT_EQ(Probes(), 2u);
  EXPECT_GT(FrameBur(), 0.0);
}

TEST_F(PudicaFeedbackTest, ProbeAndNonvideoBeforeMediaCannotInitializeDmin) {
  Feedback(0, 20'000, false, true);
  Feedback(1'000, 21'000, false, false);
  EXPECT_LT(Dmin(), 0.0);
  EXPECT_EQ(Probes(), 0u);
  EXPECT_EQ(FramePackets(), 0);
}

TEST_F(PudicaFeedbackTest, OldRtpTimestampCannotMergeOrRewindCurrentFrame) {
  Feedback(0, 20'000, true, false, 9000);
  Feedback(33'000, 53'000, true, false, 12000);
  Feedback(10'000, 54'000, true, false, 9000);
  EXPECT_EQ(FramePackets(), 1);
  EXPECT_EQ(FrameBytes(), 1200);
  Feedback(34'000, 55'000, true, false, 12000);
  EXPECT_EQ(FramePackets(), 2);
  Feedback(15'000, 56'000, false, true, 9000);
  EXPECT_EQ(Probes(), 0u);
}

TEST_F(PudicaFeedbackTest, Eq6KeepsIntendedSpanFromFirstSentPacket) {
  Feedback(0, 20'000, true, false, 9000, 6000);
  PacingController::SetPudicaRho(12.0);
  Feedback(10'000, 30'000, true, false, 9000, 1000);
  EXPECT_EQ(IntendedSpan(), 6000);
  PacingController::SetPudicaRho(0.0);
}

TEST_F(PudicaFeedbackTest, TimerCanReducePublishedTargetWithoutNewFeedback) {
  SetPublishedRate(10'000'000);
  SetDmin(20'000);
  const int64_t committed = CommittedRate();
  // Nothing has completed on the feedback path, but a sent marker is overdue.
  PacingController::PudicaRecordFrameSent(rtc::TimeMicros() - 400'000, 1);
  const int64_t out = RtpSctpCoordinator::GetPudicaTimerFallback();
  EXPECT_GT(out, 0);
  EXPECT_LT(out, 10'000'000);
  EXPECT_EQ(CommittedRate(), committed);
}

TEST_F(PudicaFeedbackTest, AckOfFirstSameTimestampMarkerRetainsSecond) {
  PacingController::PudicaRecordFrameSent(base_us_, 1);
  PacingController::PudicaRecordFrameSent(base_us_, 2);
  Feedback(0, 20'000, true, false);
  uint64_t oldest_id = 0;
  EXPECT_EQ(OldestAfterFeedback(&oldest_id), base_us_);
  EXPECT_EQ(oldest_id, 2u);
  Feedback(0, 21'000, true, false);
  EXPECT_EQ(OldestAfterFeedback(&oldest_id), 0);
}

TEST_F(PudicaFeedbackTest, QueueEstimateDoesNotUseCommittedWhenRecvIsUnknown) {
  SetPublishedRate(10'000'000);
  SetCommittedRate(10'000'000);
  for (int i = 0; i < 3; ++i) {
    UpdateWithoutMeasuredRecv(2.0, base_us_ + i * 33'333);
  }
  // Existing recv fallback uses committed; the queue subtraction must not
  // additionally fabricate queue bytes from that same target.
  EXPECT_EQ(CommittedRate(), 8'500'000);
}

TEST_F(PudicaFeedbackTest, DelayedBatchWaitsForActualMiDecisionTime) {
  CheckDelayedBatchBarrier(false);
}

TEST_F(PudicaFeedbackTest, DelayedBatchWaitsForActualAimdDecisionTime) {
  CheckDelayedBatchBarrier(true);
}

}  // namespace webrtc
