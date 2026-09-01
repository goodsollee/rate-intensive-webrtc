/*
 *  Copyright (c) 2026 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

// What happens to congestion control when ONE TWCC feedback datagram never
// arrives.
//
// The RAN-side feedback gate drops a bounded number of TWCC datagrams after it
// discards a superseded backlog, so the receiver's report of that discard never
// reaches the sender's estimators. The argument for why that is safe is a
// code-reading argument; these tests turn it into assertions.
//
// The claims, one test each:
//
//   1. A dropped datagram's packets become INVISIBLE, not lost. There is no
//      expected-vs-received inference anywhere in the pipeline, so the loss
//      ratio's denominator shrinks with its numerator.
//   2. The delay-based path never sees reported-lost packets at all
//      (SortedByReceiveTime filters on IsReceived), so removing a report of
//      loss cannot move the delay estimate.
//   3. The acknowledged-rate estimate is never raised by the hole, and falls
//      only in proportion to the packets it hid. (The mechanism is NOT the
//      largest-gap removal, as the first draft claimed -- see the test.)
//   4. Whatever the loss path does, LossBasedBweV2's output is capped by the
//      delay-based estimate, which the gate cannot influence.
//
// Plus one test for a prediction that turned out to be WRONG, kept because the
// reason is load-bearing: where the drops fall does not matter, because the
// binding term in the acked-rate estimate is the send-side one.

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "modules/congestion_controller/goog_cc/acknowledged_bitrate_estimator_interface.h"
#include "modules/congestion_controller/goog_cc/delay_based_bwe.h"
#include "modules/congestion_controller/goog_cc/loss_based_bwe_v2.h"
#include "modules/congestion_controller/goog_cc/robust_throughput_estimator.h"
#include "test/explicit_key_value_config.h"
#include "test/gtest.h"

namespace webrtc {
namespace {

using ::webrtc::test::ExplicitKeyValueConfig;

constexpr DataSize kPacketSize = DataSize::Bytes(1200);
// One TWCC datagram covers one feedback interval. 50 ms and 15 packets is the
// shape the testbed actually produces at a few Mbps.
constexpr size_t kPacketsPerDatagram = 15;
constexpr TimeDelta kFeedbackInterval = TimeDelta::Millis(50);

// One feedback datagram: the packets the receiver reported in it.
using Datagram = std::vector<PacketResult>;

// Builds a stream of datagrams at a steady rate. `lost_datagram` marks every
// packet in that one datagram as not-received, which is what the receiver
// reports after the RAN discards a backlog.
std::vector<Datagram> BuildStream(size_t num_datagrams,
                                  std::optional<size_t> lost_datagram) {
  std::vector<Datagram> stream;
  Timestamp send_clock = Timestamp::Millis(100000);
  Timestamp recv_clock = Timestamp::Millis(100050);
  int64_t sequence_number = 1000;

  for (size_t d = 0; d < num_datagrams; ++d) {
    Datagram datagram;
    const bool all_lost = lost_datagram.has_value() && *lost_datagram == d;
    for (size_t i = 0; i < kPacketsPerDatagram; ++i) {
      PacketResult packet;
      packet.sent_packet.send_time = send_clock;
      packet.sent_packet.sequence_number = sequence_number++;
      packet.sent_packet.size = kPacketSize;
      // A packet the receiver never got has an infinite receive time; that is
      // the ONLY way loss is signalled downstream (PacketResult::IsReceived).
      packet.receive_time =
          all_lost ? Timestamp::PlusInfinity() : recv_clock;
      datagram.push_back(packet);
      send_clock += kFeedbackInterval / kPacketsPerDatagram;
      recv_clock += kFeedbackInterval / kPacketsPerDatagram;
    }
    stream.push_back(std::move(datagram));
  }
  return stream;
}

// The gate: remove whole datagrams from the stream by index.
std::vector<Datagram> WithoutDatagrams(const std::vector<Datagram>& stream,
                                       const std::vector<size_t>& drop) {
  std::vector<Datagram> out;
  for (size_t d = 0; d < stream.size(); ++d) {
    bool dropped = false;
    for (size_t k : drop) {
      if (k == d) {
        dropped = true;
        break;
      }
    }
    if (!dropped)
      out.push_back(stream[d]);
  }
  return out;
}

// What DelayBasedBwe and the acked-rate estimators are actually handed:
// TransportPacketsFeedback::SortedByReceiveTime(), which keeps received only.
std::vector<PacketResult> ReceivedOnly(const Datagram& datagram) {
  TransportPacketsFeedback msg;
  msg.packet_feedbacks = datagram;
  return msg.SortedByReceiveTime();
}

std::optional<DataRate> AckedRate(const std::vector<Datagram>& stream) {
  ExplicitKeyValueConfig trials("");
  RobustThroughputEstimatorSettings settings(&trials);
  RobustThroughputEstimator estimator(settings);
  for (const Datagram& datagram : stream) {
    std::vector<PacketResult> received = ReceivedOnly(datagram);
    if (!received.empty())
      estimator.IncomingPacketFeedbackVector(received);
  }
  return estimator.bitrate();
}

std::string V2Config() {
  return "WebRTC-Bwe-LossBasedBweV2/"
         "Enabled:true,BwRampupUpperBoundFactor:1.2,"
         "CandidateFactors:1.1|1.0|0.95,HigherBwBiasFactor:0.01,"
         "InherentLossLowerBound:0.001,InherentLossUpperBoundBwBalance:14kbps,"
         "InherentLossUpperBoundOffset:0.9,InitialInherentLossEstimate:0.01,"
         "NewtonIterations:2,NewtonStepSize:0.4,ObservationWindowSize:15,"
         "SendingRateSmoothingFactor:0.01,"
         "InstantUpperBoundTemporalWeightFactor:0.97,"
         "InstantUpperBoundBwBalance:90kbps,"
         "InstantUpperBoundLossOffset:0.1,TemporalWeightFactor:0.98,"
         "MinNumObservations:1,ObservationDurationLowerBound:1ms,"
         "MaxIncreaseFactor:1000.0,DelayedIncreaseWindow:100ms/";
}

struct V2Out {
  double reported_loss_ratio = 0.0;
  DataRate estimate = DataRate::Zero();
};

V2Out RunV2(const std::vector<Datagram>& stream, DataRate delay_based) {
  ExplicitKeyValueConfig trials(V2Config());
  LossBasedBweV2 estimator(&trials);
  estimator.SetBandwidthEstimate(DataRate::KilobitsPerSec(600));
  for (const Datagram& datagram : stream) {
    estimator.UpdateBandwidthEstimate(datagram, delay_based, /*in_alr=*/false);
  }
  return {estimator.GetAverageReportedLossRatio(),
          estimator.GetLossBasedResult().bandwidth_estimate};
}

// ---------------------------------------------------------------------------

// Claim 1. Dropping the datagram that carried a loss report cannot RAISE the
// loss ratio, because num_packets counts delivered PacketResults rather than
// an expected count (loss_based_bwe_v2.cc: partial_observation_.num_packets +=
// packet_results.size()). Numerator and denominator lose the same entries.
TEST(FeedbackGateInvarianceTest, DroppingALossReportNeverRaisesLossRatio) {
  const size_t kNumDatagrams = 20;
  const size_t kLossAt = 10;
  std::vector<Datagram> complete = BuildStream(kNumDatagrams, kLossAt);
  std::vector<Datagram> gated = WithoutDatagrams(complete, {kLossAt});

  const DataRate delay_based = DataRate::KilobitsPerSec(2000);
  V2Out with_report = RunV2(complete, delay_based);
  V2Out without_report = RunV2(gated, delay_based);

  // The loss actually reaches the estimator when the report is delivered...
  EXPECT_GT(with_report.reported_loss_ratio, 0.0);
  // ...and hiding it can only lower what the estimator sees. Never raise it.
  EXPECT_LE(without_report.reported_loss_ratio,
            with_report.reported_loss_ratio);
}

// Claim 2. The delay-based path never sees reported-lost packets, so a report
// of loss -- and therefore its removal -- cannot move the delay estimate.
// This is SortedByReceiveTime()'s IsReceived() filter, asserted directly.
TEST(FeedbackGateInvarianceTest, ReportedLossNeverReachesTheDelayPath) {
  std::vector<Datagram> with_loss = BuildStream(4, /*lost_datagram=*/2);
  std::vector<Datagram> no_loss = BuildStream(4, std::nullopt);

  // Datagram 2 is entirely lost in one stream and entirely received in the
  // other; the delay path is handed nothing at all from it in the first case.
  EXPECT_TRUE(ReceivedOnly(with_loss[2]).empty());
  EXPECT_EQ(ReceivedOnly(no_loss[2]).size(), kPacketsPerDatagram);

  // Every other datagram is byte-for-byte the same input to the delay path.
  for (size_t d : {0u, 1u, 3u}) {
    std::vector<PacketResult> a = ReceivedOnly(with_loss[d]);
    std::vector<PacketResult> b = ReceivedOnly(no_loss[d]);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
      EXPECT_EQ(a[i].receive_time, b[i].receive_time);
      EXPECT_EQ(a[i].sent_packet.send_time, b[i].sent_packet.send_time);
    }
  }
}

// Claim 3. The acknowledged-rate estimate is never RAISED by a dropped
// datagram, and the amount it falls is bounded by the fraction of packets the
// drop hid.
//
// NOTE, against the first reading of the code: it is NOT the largest-gap
// removal that carries this. That correction applies to the receive term, but
// bitrate() returns min(send_size/send_duration, recv_size/recv_duration) and
// the SEND term is the one that binds -- it has no gap correction, and
// send_duration = last_send - first_send is unchanged by the hole. So the
// estimate falls with the missing bytes, in proportion, and the receive-side
// gap machinery never gets to matter. Measured below rather than assumed.
TEST(FeedbackGateInvarianceTest, AckedRateFallsInProportionAndNeverRises) {
  const size_t kNumDatagrams = 20;
  std::vector<Datagram> complete = BuildStream(kNumDatagrams, std::nullopt);

  std::optional<DataRate> full = AckedRate(complete);
  std::optional<DataRate> one = AckedRate(WithoutDatagrams(complete, {10}));
  std::optional<DataRate> two = AckedRate(WithoutDatagrams(complete, {10, 11}));
  ASSERT_TRUE(full.has_value() && one.has_value() && two.has_value());

  // Never an over-estimate. This is the direction that would matter, because
  // an inflated acked rate is what would let the sender ramp past the link.
  EXPECT_LE(one->bps<double>(), full->bps<double>());
  EXPECT_LE(two->bps<double>(), full->bps<double>());

  // And it degrades gracefully: hiding 1 of 20 datagrams costs well under a
  // fifth of the estimate, hiding 2 costs more than hiding 1.
  EXPECT_GT(one->bps<double>(), full->bps<double>() * 0.80);
  EXPECT_LE(two->bps<double>(), one->bps<double>());
}

// Claim 4. Hiding loss can keep LossBasedBweV2 from backing off -- that is the
// point -- but it cannot lift the estimate above the delay-based estimate,
// which the gate does not influence (loss_based_bwe_v2.cc:299-304).
TEST(FeedbackGateInvarianceTest, V2EstimateIsCappedByDelayBasedEstimate) {
  const DataRate delay_based = DataRate::KilobitsPerSec(800);
  std::vector<Datagram> complete = BuildStream(20, /*lost_datagram=*/10);
  std::vector<Datagram> gated = WithoutDatagrams(complete, {10});

  EXPECT_LE(RunV2(complete, delay_based).estimate, delay_based);
  EXPECT_LE(RunV2(gated, delay_based).estimate, delay_based);
}

// The boundary, measured rather than assumed. The first draft of this file
// predicted that two NON-adjacent drops would be worse than two adjacent ones,
// on the theory that RobustThroughputEstimator's largest-gap removal handles
// exactly one hole. That prediction is WRONG, and the test is kept in its
// corrected form because the reason matters: bitrate() returns the MINIMUM of
// the send-side and receive-side rates, and the send-side term -- which has no
// gap correction and whose duration is unchanged by a hole -- is the binding
// one. It depends on HOW MANY packets were hidden, not on where they fell.
//
// Consequence for the gate: it does not have to drop adjacent datagrams.
TEST(FeedbackGateInvarianceTest, AckedRateDoesNotDependOnWhereTheDropsFall) {
  const size_t kNumDatagrams = 20;
  std::vector<Datagram> complete = BuildStream(kNumDatagrams, std::nullopt);
  std::optional<DataRate> adjacent =
      AckedRate(WithoutDatagrams(complete, {10, 11}));
  std::optional<DataRate> split =
      AckedRate(WithoutDatagrams(complete, {6, 14}));
  ASSERT_TRUE(adjacent.has_value() && split.has_value());

  EXPECT_EQ(adjacent->bps(), split->bps());
}

}  // namespace
}  // namespace webrtc
