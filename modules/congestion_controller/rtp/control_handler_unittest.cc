/*
 * Copyright 2026 The WebRTC project authors. All Rights Reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file in the root of the source tree.
 */
#include "modules/congestion_controller/rtp/control_handler.h"

#include <limits>

#include "modules/pacing/pacing_controller.h"
#include "test/gtest.h"

namespace webrtc {
namespace {
TargetTransferRate Rate(double signal) {
  TargetTransferRate rate;
  rate.at_time = Timestamp::Millis(1000);
  rate.target_rate = DataRate::KilobitsPerSec(300);
  rate.network_estimate.loss_rate_ratio = 0.1;
  rate.network_estimate.round_trip_time = TimeDelta::Millis(20);
  rate.is_overused_for_encoder = signal;
  return rate;
}
}  // namespace

TEST(CongestionControlHandlerTest, MaeOnlyRiseDuplicateAndFall) {
  CongestionControlHandler handler;
  for (double signal : {1.0, 1.5, 1.75, 1.0}) {
    handler.SetTargetRate(Rate(signal));
    auto update = handler.GetUpdate();
    ASSERT_TRUE(update);
    EXPECT_DOUBLE_EQ(update->is_overused_for_encoder, signal);
    EXPECT_EQ(update->target_rate, Rate(signal).target_rate);
    EXPECT_EQ(update->network_estimate.loss_rate_ratio,
              Rate(signal).network_estimate.loss_rate_ratio);
    EXPECT_EQ(update->network_estimate.round_trip_time, TimeDelta::Millis(20));
    handler.SetTargetRate(Rate(signal));
    EXPECT_FALSE(handler.GetUpdate());
  }
}

TEST(CongestionControlHandlerTest, MaeNonfiniteReturnsToNeutralOnce) {
  CongestionControlHandler handler;
  for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity()}) {
    handler.SetTargetRate(Rate(1.5));
    ASSERT_TRUE(handler.GetUpdate());
    handler.SetTargetRate(Rate(invalid));
    auto update = handler.GetUpdate();
    ASSERT_TRUE(update);
    EXPECT_DOUBLE_EQ(update->is_overused_for_encoder, 1.0);
    handler.SetTargetRate(Rate(invalid));
    EXPECT_FALSE(handler.GetUpdate());
  }
}

TEST(CongestionControlHandlerTest, MaePauseAndResumePreserveLatestSignal) {
  for (bool network_pause : {false, true}) {
    CongestionControlHandler handler;
    handler.SetTargetRate(Rate(1.0));
    ASSERT_TRUE(handler.GetUpdate());
    if (network_pause)
      handler.SetNetworkAvailability(false);
    else
      handler.SetPacerQueue(PacingController::kMaxExpectedQueueLength +
                            TimeDelta::Millis(1));
    auto paused = handler.GetUpdate();
    ASSERT_TRUE(paused);
    EXPECT_TRUE(paused->target_rate.IsZero());
    handler.SetTargetRate(Rate(1.5));
    EXPECT_FALSE(handler.GetUpdate());
    handler.SetTargetRate(Rate(1.75));
    EXPECT_FALSE(handler.GetUpdate());
    handler.SetNetworkAvailability(true);
    handler.SetPacerQueue(TimeDelta::Zero());
    auto resumed = handler.GetUpdate();
    ASSERT_TRUE(resumed);
    EXPECT_EQ(resumed->target_rate, Rate(1.75).target_rate);
    EXPECT_DOUBLE_EQ(resumed->is_overused_for_encoder, 1.75);
    EXPECT_FALSE(handler.GetUpdate());
  }
}

TEST(CongestionControlHandlerTest, OrdinaryLossRttAndRateStillPropagate) {
  CongestionControlHandler handler;
  auto rate = Rate(1.0);
  handler.SetTargetRate(rate);
  ASSERT_TRUE(handler.GetUpdate());
  rate.network_estimate.loss_rate_ratio = 0.2;
  handler.SetTargetRate(rate);
  ASSERT_TRUE(handler.GetUpdate());
  rate.network_estimate.round_trip_time = TimeDelta::Millis(30);
  handler.SetTargetRate(rate);
  ASSERT_TRUE(handler.GetUpdate());
  rate.target_rate = DataRate::KilobitsPerSec(400);
  handler.SetTargetRate(rate);
  ASSERT_TRUE(handler.GetUpdate());
  EXPECT_FALSE(handler.GetUpdate());
}
}  // namespace webrtc
