/*
 *  Copyright 2025 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "pc/coordinator/flow_registry.h"
#include "pc/coordinator/mafs_priority_calculator.h"
#include "pc/coordinator/multi_agent_flow_coordinator.h"
#include "pc/coordinator/strict_priority_mapper.h"
#include "pc/coordinator/uplink_telemetry.h"

#include <cmath>

#include "test/gtest.h"

namespace webrtc {
namespace coordinator {
namespace {

// ============================================================================
// UplinkTelemetry Tests
// ============================================================================

class UplinkTelemetryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_.sample_interval_ms = 100;
    config_.ewma_window_ms = 500;
    config_.ewma_alpha = 0.5;
    telemetry_ = std::make_unique<UplinkTelemetry>(config_);
  }

  UplinkTelemetry::Config config_;
  std::unique_ptr<UplinkTelemetry> telemetry_;
};

TEST_F(UplinkTelemetryTest, InitialState) {
  EXPECT_EQ(telemetry_->GetUplinkRateBps(), 0.0);
  EXPECT_EQ(telemetry_->GetSigmaR(), 0.0);
  EXPECT_EQ(telemetry_->GetTotalBytesReceived(), size_t{0});
  EXPECT_EQ(telemetry_->GetSampleCount(), size_t{0});
  EXPECT_FALSE(telemetry_->IsReady());
}

TEST_F(UplinkTelemetryTest, SingleStreamBytesTracking) {
  telemetry_->OnBytesReceived(1, 1000, 0);
  EXPECT_EQ(telemetry_->GetStreamBytesReceived(1), size_t{1000});
  EXPECT_EQ(telemetry_->GetStreamBytesReceived(2), size_t{0});
  EXPECT_EQ(telemetry_->GetTotalBytesReceived(), size_t{1000});
}

TEST_F(UplinkTelemetryTest, MultipleStreamBytesTracking) {
  telemetry_->OnBytesReceived(1, 1000, 0);
  telemetry_->OnBytesReceived(2, 2000, 0);
  telemetry_->OnBytesReceived(1, 500, 0);
  EXPECT_EQ(telemetry_->GetStreamBytesReceived(1), size_t{1500});
  EXPECT_EQ(telemetry_->GetStreamBytesReceived(2), size_t{2000});
  EXPECT_EQ(telemetry_->GetTotalBytesReceived(), size_t{3500});
}

TEST_F(UplinkTelemetryTest, RateSamplingAfterInterval) {
  int64_t time_ms = 0;
  telemetry_->OnBytesReceived(1, 10000, time_ms);
  EXPECT_EQ(telemetry_->GetUplinkRateBps(), 0.0);

  time_ms += config_.sample_interval_ms;
  telemetry_->OnBytesReceived(1, 5000, time_ms);

  double expected_rate = 15000.0 * 8.0 / 0.1;
  EXPECT_NEAR(telemetry_->GetUplinkRateBps(), expected_rate, 1000.0);
}

TEST_F(UplinkTelemetryTest, EwmaSmoothing) {
  int64_t time_ms = 0;
  telemetry_->OnBytesReceived(1, 10000, time_ms);
  time_ms += config_.sample_interval_ms;
  telemetry_->OnBytesReceived(1, 0, time_ms);
  double rate1 = telemetry_->GetUplinkRateBps();
  EXPECT_NEAR(rate1, 800000.0, 1000.0);

  telemetry_->OnBytesReceived(1, 20000, time_ms);
  time_ms += config_.sample_interval_ms;
  telemetry_->OnBytesReceived(1, 0, time_ms);
  double rate2 = telemetry_->GetUplinkRateBps();
  EXPECT_NEAR(rate2, 1200000.0, 10000.0);
}

TEST_F(UplinkTelemetryTest, SigmaRCalculation) {
  int64_t time_ms = 0;
  telemetry_->OnBytesReceived(1, 10000, time_ms);
  time_ms += config_.sample_interval_ms;
  telemetry_->OnBytesReceived(1, 0, time_ms);
  EXPECT_EQ(telemetry_->GetSampleCount(), size_t{1});
  EXPECT_EQ(telemetry_->GetSigmaR(), 0.0);

  telemetry_->OnBytesReceived(1, 20000, time_ms);
  time_ms += config_.sample_interval_ms;
  telemetry_->OnBytesReceived(1, 0, time_ms);
  EXPECT_EQ(telemetry_->GetSampleCount(), size_t{2});
  EXPECT_TRUE(telemetry_->IsReady());

  double sigma_r = telemetry_->GetSigmaR();
  EXPECT_NEAR(sigma_r, 400000.0, 10000.0);
}

TEST_F(UplinkTelemetryTest, WindowPruning) {
  int64_t time_ms = 0;
  for (int i = 0; i < 10; i++) {
    telemetry_->OnBytesReceived(1, 10000, time_ms);
    time_ms += config_.sample_interval_ms;
  }
  telemetry_->OnBytesReceived(1, 0, time_ms);
  EXPECT_LE(telemetry_->GetSampleCount(), size_t{6});
  EXPECT_GE(telemetry_->GetSampleCount(), size_t{4});
}

TEST_F(UplinkTelemetryTest, ClearAll) {
  int64_t time_ms = 0;
  telemetry_->OnBytesReceived(1, 10000, time_ms);
  time_ms += config_.sample_interval_ms;
  telemetry_->OnBytesReceived(1, 10000, time_ms);
  time_ms += config_.sample_interval_ms;
  telemetry_->OnBytesReceived(1, 0, time_ms);
  EXPECT_GT(telemetry_->GetUplinkRateBps(), 0.0);

  telemetry_->Clear();
  EXPECT_EQ(telemetry_->GetUplinkRateBps(), 0.0);
  EXPECT_EQ(telemetry_->GetTotalBytesReceived(), size_t{0});
  EXPECT_FALSE(telemetry_->IsReady());
}

TEST_F(UplinkTelemetryTest, ConfigValidation) {
  UplinkTelemetry::Config valid;
  valid.sample_interval_ms = 1000;
  valid.ewma_window_ms = 5000;
  valid.ewma_alpha = 0.2;
  EXPECT_TRUE(valid.IsValid());

  UplinkTelemetry::Config invalid1;
  invalid1.sample_interval_ms = -1;
  EXPECT_FALSE(invalid1.IsValid());

  UplinkTelemetry::Config invalid2;
  invalid2.ewma_alpha = 1.5;
  EXPECT_FALSE(invalid2.IsValid());

  UplinkTelemetry::Config invalid3;
  invalid3.sample_interval_ms = 1000;
  invalid3.ewma_window_ms = 500;
  EXPECT_FALSE(invalid3.IsValid());
}

TEST_F(UplinkTelemetryTest, AccurateRateCalculation) {
  UplinkTelemetry::Config default_config;
  default_config.sample_interval_ms = 1000;
  default_config.ewma_window_ms = 5000;
  default_config.ewma_alpha = 1.0;
  UplinkTelemetry telemetry(default_config);

  telemetry.OnBytesReceived(1, 125000, 0);
  telemetry.OnBytesReceived(1, 0, 1000);
  EXPECT_NEAR(telemetry.GetUplinkRateBps(), 1000000.0, 100.0);
}

// ============================================================================
// FlowRegistry Tests
// ============================================================================

class FlowRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override { registry_ = std::make_unique<FlowRegistry>(); }
  std::unique_ptr<FlowRegistry> registry_;
};

TEST_F(FlowRegistryTest, InitialState) {
  EXPECT_EQ(registry_->GetFlowCount(), size_t{0});
  EXPECT_EQ(registry_->GetActiveFlowCount(), size_t{0});
  EXPECT_FALSE(registry_->IsRegisteredStream(1));
}

TEST_F(FlowRegistryTest, RegisterFlow) {
  uint32_t flow_id = registry_->RegisterFlow(10, "kv_cache", 100000, 1000);
  EXPECT_EQ(flow_id, 1u);
  EXPECT_EQ(registry_->GetFlowCount(), size_t{1});
  EXPECT_TRUE(registry_->IsRegisteredStream(10));

  FlowState* flow = registry_->FindByFlowId(flow_id);
  ASSERT_NE(flow, nullptr);
  EXPECT_EQ(flow->stream_id, 10);
  EXPECT_EQ(flow->label, "kv_cache");
  EXPECT_EQ(flow->total_bytes, size_t{100000});
  EXPECT_TRUE(flow->active);
}

TEST_F(FlowRegistryTest, MultipleFlowRegistration) {
  uint32_t id1 = registry_->RegisterFlow(10, "kv", 100000, 1000);
  uint32_t id2 = registry_->RegisterFlow(11, "lora", 50000, 1000);
  uint32_t id3 = registry_->RegisterFlow(12, "video", 200000, 1000);
  EXPECT_EQ(id1, 1u);
  EXPECT_EQ(id2, 2u);
  EXPECT_EQ(id3, 3u);
  EXPECT_EQ(registry_->GetFlowCount(), size_t{3});
}

TEST_F(FlowRegistryTest, FindByStreamId) {
  uint32_t flow_id = registry_->RegisterFlow(10, "kv", 100000, 1000);
  FlowState* flow = registry_->FindByStreamId(10);
  ASSERT_NE(flow, nullptr);
  EXPECT_EQ(flow->flow_id, flow_id);
  EXPECT_EQ(registry_->FindByStreamId(99), nullptr);
}

TEST_F(FlowRegistryTest, OnFlowDataReceived) {
  registry_->RegisterFlow(10, "kv", 100000, 1000);
  registry_->OnFlowDataReceived(10, 25000);
  FlowState* flow = registry_->FindByStreamId(10);
  ASSERT_NE(flow, nullptr);
  EXPECT_EQ(flow->bytes_received.load(), size_t{25000});
  EXPECT_EQ(flow->BytesRemaining(), size_t{75000});

  registry_->OnFlowDataReceived(10, 25000);
  EXPECT_EQ(flow->bytes_received.load(), size_t{50000});

  registry_->OnFlowDataReceived(99, 10000);  // No crash
}

TEST_F(FlowRegistryTest, SetComputeTime) {
  uint32_t flow_id = registry_->RegisterFlow(10, "kv", 100000, 1000);
  EXPECT_TRUE(registry_->SetComputeTime(flow_id, 150.0, 20.0, 1100));

  FlowState* flow = registry_->FindByFlowId(flow_id);
  ASSERT_NE(flow, nullptr);
  EXPECT_DOUBLE_EQ(flow->P_hat_ms, 150.0);
  EXPECT_DOUBLE_EQ(flow->sigma_P_ms, 20.0);
  EXPECT_TRUE(flow->HasComputeTime());

  EXPECT_FALSE(registry_->SetComputeTime(999, 100.0, 10.0, 1000));
}

TEST_F(FlowRegistryTest, FlowCompletion) {
  registry_->RegisterFlow(10, "kv", 100000, 1000);
  FlowState* flow = registry_->FindByFlowId(1);
  ASSERT_NE(flow, nullptr);
  EXPECT_FALSE(flow->IsComplete());
  EXPECT_TRUE(flow->IsSchedulable());

  registry_->OnFlowDataReceived(10, 100000);
  EXPECT_TRUE(flow->IsComplete());
  EXPECT_FALSE(flow->IsSchedulable());
}

TEST_F(FlowRegistryTest, MarkFlowComplete) {
  uint32_t flow_id = registry_->RegisterFlow(10, "kv", 100000, 1000);
  EXPECT_EQ(registry_->GetActiveFlowCount(), size_t{1});
  registry_->MarkFlowComplete(flow_id);
  EXPECT_EQ(registry_->GetActiveFlowCount(), size_t{0});
  EXPECT_EQ(registry_->GetFlowCount(), size_t{1});
}

TEST_F(FlowRegistryTest, RemoveFlow) {
  uint32_t flow_id = registry_->RegisterFlow(10, "kv", 100000, 1000);
  registry_->RemoveFlow(flow_id);
  EXPECT_EQ(registry_->GetFlowCount(), size_t{0});
  EXPECT_FALSE(registry_->IsRegisteredStream(10));
}

TEST_F(FlowRegistryTest, GetSchedulableFlows) {
  registry_->RegisterFlow(10, "kv", 100000, 1000);
  registry_->RegisterFlow(11, "lora", 50000, 1000);
  registry_->RegisterFlow(12, "video", 200000, 1000);
  EXPECT_EQ(registry_->GetSchedulableFlows().size(), size_t{3});

  registry_->OnFlowDataReceived(11, 50000);
  EXPECT_EQ(registry_->GetSchedulableFlows().size(), size_t{2});
}

TEST_F(FlowRegistryTest, Clear) {
  registry_->RegisterFlow(10, "kv", 100000, 1000);
  registry_->RegisterFlow(11, "lora", 50000, 1000);
  registry_->Clear();
  EXPECT_EQ(registry_->GetFlowCount(), size_t{0});
  EXPECT_FALSE(registry_->IsRegisteredStream(10));
}

TEST_F(FlowRegistryTest, FlowStateHelpers) {
  FlowState flow;
  flow.total_bytes = 1000;
  flow.bytes_received.store(500);
  flow.active = true;
  EXPECT_EQ(flow.BytesRemaining(), size_t{500});
  EXPECT_FALSE(flow.IsComplete());
  EXPECT_TRUE(flow.IsSchedulable());

  flow.bytes_received.store(1000);
  EXPECT_EQ(flow.BytesRemaining(), size_t{0});
  EXPECT_TRUE(flow.IsComplete());

  flow.bytes_received.store(1500);
  EXPECT_EQ(flow.BytesRemaining(), size_t{0});
}

// ============================================================================
// MafsPriorityCalculator Tests
// ============================================================================

class MafsPriorityCalculatorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    MafsPriorityCalculator::Config cfg;
    cfg.min_rate_bps = 1.0;  // Don't clamp rate in tests
    calculator_ = std::make_unique<MafsPriorityCalculator>(cfg);
    registry_ = std::make_unique<FlowRegistry>();
    telemetry_ = std::make_unique<UplinkTelemetry>();
  }

  std::unique_ptr<MafsPriorityCalculator> calculator_;
  std::unique_ptr<FlowRegistry> registry_;
  std::unique_ptr<UplinkTelemetry> telemetry_;
};

TEST_F(MafsPriorityCalculatorTest, DefaultConfig) {
  EXPECT_DOUBLE_EQ(calculator_->GetConfig().beta, 1.0);
  EXPECT_DOUBLE_EQ(calculator_->GetConfig().min_rate_bps, 1.0);  // Test fixture uses low min
  EXPECT_TRUE(calculator_->GetConfig().IsValid());
  // Verify actual defaults
  MafsPriorityCalculator default_calc;
  EXPECT_DOUBLE_EQ(default_calc.GetConfig().min_rate_bps, 100000000.0);
}

TEST_F(MafsPriorityCalculatorTest, CalculateNetworkTime) {
  double N_hat = calculator_->CalculateNetworkTime(100000, 1000000.0);
  EXPECT_NEAR(N_hat, 800.0, 0.01);

  N_hat = calculator_->CalculateNetworkTime(50000, 10000000.0);
  EXPECT_NEAR(N_hat, 40.0, 0.01);
}

TEST_F(MafsPriorityCalculatorTest, CalculateNetworkUncertainty) {
  double sigma_N = calculator_->CalculateNetworkUncertainty(100000, 1000000.0, 100000.0);
  EXPECT_NEAR(sigma_N, 80.0, 0.01);
}

TEST_F(MafsPriorityCalculatorTest, JohnsonGroupA) {
  // N <= P: Group A, rho = MAX_TIME - N
  double rho = calculator_->CalculatePriority(500.0, 20.0, 300.0, 30.0);
  EXPECT_NEAR(rho, 1000000.0 - 300.0, 0.01);
}

TEST_F(MafsPriorityCalculatorTest, JohnsonGroupB) {
  // N > P: Group B, rho = P - MAX_TIME
  double rho = calculator_->CalculatePriority(100.0, 20.0, 300.0, 30.0);
  EXPECT_NEAR(rho, 100.0 - 1000000.0, 0.01);
}

TEST_F(MafsPriorityCalculatorTest, UpdateFlowPrioritiesWithFlows) {
  int64_t now_ms = 1000;
  uint32_t id1 = registry_->RegisterFlow(10, "kv", 100000, now_ms);
  uint32_t id2 = registry_->RegisterFlow(11, "lora", 50000, now_ms);
  registry_->SetComputeTime(id1, 500.0, 20.0, now_ms);
  registry_->SetComputeTime(id2, 100.0, 10.0, now_ms);

  telemetry_->OnBytesReceived(10, 125000, now_ms);
  telemetry_->OnBytesReceived(10, 125000, now_ms + 1000);
  telemetry_->OnBytesReceived(10, 125000, now_ms + 2000);

  size_t updated = calculator_->UpdateFlowPriorities(registry_.get(),
                                                      telemetry_.get(),
                                                      now_ms + 2000);
  EXPECT_EQ(updated, size_t{2});

  FlowState* flow1 = registry_->FindByFlowId(id1);
  FlowState* flow2 = registry_->FindByFlowId(id2);
  EXPECT_NE(flow1->N_hat_ms, 0.0);
  EXPECT_NE(flow2->N_hat_ms, 0.0);
}

TEST_F(MafsPriorityCalculatorTest, GetFlowsByPriority) {
  int64_t now_ms = 1000;
  uint32_t id1 = registry_->RegisterFlow(10, "kv", 100000, now_ms);
  uint32_t id2 = registry_->RegisterFlow(11, "lora", 50000, now_ms);
  uint32_t id3 = registry_->RegisterFlow(12, "video", 200000, now_ms);

  registry_->SetPriority(id1, 100.0, now_ms);
  registry_->SetPriority(id2, -50.0, now_ms);
  registry_->SetPriority(id3, 200.0, now_ms);

  auto sorted = calculator_->GetFlowsByPriority(registry_.get());
  EXPECT_EQ(sorted.size(), size_t{3});
  EXPECT_EQ(sorted[0]->flow_id, id3);  // video: rho=200
  EXPECT_EQ(sorted[1]->flow_id, id1);  // kv: rho=100
  EXPECT_EQ(sorted[2]->flow_id, id2);  // lora: rho=-50
}

// ============================================================================
// StrictPriorityMapper Tests
// ============================================================================

class StrictPriorityMapperTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mapper_ = std::make_unique<StrictPriorityMapper>();
    registry_ = std::make_unique<FlowRegistry>();
  }

  std::unique_ptr<StrictPriorityMapper> mapper_;
  std::unique_ptr<FlowRegistry> registry_;
};

TEST_F(StrictPriorityMapperTest, ExponentialPriorityForRank) {
  EXPECT_EQ(mapper_->PriorityForRank(0), uint16_t{65535});
  EXPECT_EQ(mapper_->PriorityForRank(1), uint16_t{100});
  EXPECT_EQ(mapper_->PriorityForRank(2), uint16_t{10});
  EXPECT_EQ(mapper_->PriorityForRank(7), uint16_t{1});
}

TEST_F(StrictPriorityMapperTest, MapFlowsToPriorities) {
  int64_t now_ms = 1000;
  uint32_t id1 = registry_->RegisterFlow(10, "kv", 100000, now_ms);
  uint32_t id2 = registry_->RegisterFlow(11, "lora", 50000, now_ms);
  uint32_t id3 = registry_->RegisterFlow(12, "video", 200000, now_ms);

  registry_->SetPriority(id1, 100.0, now_ms);
  registry_->SetPriority(id2, -50.0, now_ms);
  registry_->SetPriority(id3, 200.0, now_ms);

  auto flows = registry_->GetSchedulableFlows();
  std::sort(flows.begin(), flows.end(),
            [](const FlowState* a, const FlowState* b) {
              return a->rho > b->rho;
            });

  auto priority_map = mapper_->MapFlowsToPriorities(flows);
  EXPECT_EQ(priority_map.size(), size_t{3});
  EXPECT_EQ(priority_map[12], uint16_t{65535});  // video rank 0
  EXPECT_EQ(priority_map[10], uint16_t{100});     // kv rank 1
  EXPECT_EQ(priority_map[11], uint16_t{10});      // lora rank 2
}

TEST_F(StrictPriorityMapperTest, EmptyFlowList) {
  std::vector<FlowState*> empty_flows;
  EXPECT_TRUE(mapper_->MapFlowsToPriorities(empty_flows).empty());
  EXPECT_TRUE(mapper_->GetPriorityMappings(empty_flows).empty());
}

TEST_F(StrictPriorityMapperTest, SingleFlow) {
  registry_->RegisterFlow(10, "kv", 100000, 1000);
  registry_->SetPriority(1, 100.0, 1000);
  auto flows = registry_->GetSchedulableFlows();
  auto priority_map = mapper_->MapFlowsToPriorities(flows);
  EXPECT_EQ(priority_map.size(), size_t{1});
  EXPECT_EQ(priority_map[10], uint16_t{65535});
}

// ============================================================================
// MultiAgentFlowCoordinator Tests
// ============================================================================

class MultiAgentFlowCoordinatorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    MultiAgentFlowCoordinator::Config config;
    config.update_interval_ms = 1000;
    config.enable_logging = false;
    coordinator_ = std::make_unique<MultiAgentFlowCoordinator>(config);
  }

  std::unique_ptr<MultiAgentFlowCoordinator> coordinator_;
};

TEST_F(MultiAgentFlowCoordinatorTest, DefaultConfig) {
  MultiAgentFlowCoordinator coord;
  EXPECT_EQ(coord.GetConfig().update_interval_ms, int64_t{500});
  EXPECT_TRUE(coord.GetConfig().IsValid());
}

TEST_F(MultiAgentFlowCoordinatorTest, RegisterFlow) {
  uint32_t id = coordinator_->RegisterFlow(10, "kv_cache", 100000, 1000);
  EXPECT_EQ(id, uint32_t{1});
  EXPECT_EQ(coordinator_->GetFlowCount(), size_t{1});
  EXPECT_EQ(coordinator_->GetActiveFlowCount(), size_t{1});
}

TEST_F(MultiAgentFlowCoordinatorTest, OnDataReceived) {
  coordinator_->RegisterFlow(10, "kv", 100000, 1000);
  coordinator_->OnDataReceived(10, 50000, 1000);
  const FlowState* flow = coordinator_->GetFlowStateByStream(10);
  ASSERT_NE(flow, nullptr);
  EXPECT_EQ(flow->bytes_received.load(), size_t{50000});
}

TEST_F(MultiAgentFlowCoordinatorTest, SetComputeTime) {
  uint32_t id = coordinator_->RegisterFlow(10, "kv", 100000, 1000);
  EXPECT_TRUE(coordinator_->SetComputeTime(id, 500.0, 20.0, 1000));
  const FlowState* flow = coordinator_->GetFlowState(id);
  ASSERT_NE(flow, nullptr);
  EXPECT_DOUBLE_EQ(flow->P_hat_ms, 500.0);
  EXPECT_TRUE(flow->HasComputeTime());
}

TEST_F(MultiAgentFlowCoordinatorTest, UpdatePrioritiesInterval) {
  int64_t now_ms = 1000;
  coordinator_->RegisterFlow(10, "kv", 10000000, now_ms);
  coordinator_->SetComputeTime(1, 500.0, 20.0, now_ms);

  coordinator_->OnDataReceived(99, 125000, now_ms);
  coordinator_->OnDataReceived(99, 125000, now_ms + 1000);

  auto map1 = coordinator_->UpdatePriorities(now_ms + 1000);
  EXPECT_EQ(map1.size(), size_t{1});

  auto map2 = coordinator_->UpdatePriorities(now_ms + 1100);
  EXPECT_TRUE(map2.empty());

  auto map3 = coordinator_->UpdatePriorities(now_ms + 2100);
  EXPECT_EQ(map3.size(), size_t{1});
}

TEST_F(MultiAgentFlowCoordinatorTest, ForceUpdatePriorities) {
  int64_t now_ms = 1000;
  coordinator_->RegisterFlow(10, "kv", 10000000, now_ms);
  coordinator_->SetComputeTime(1, 500.0, 20.0, now_ms);

  coordinator_->OnDataReceived(99, 125000, now_ms);
  coordinator_->OnDataReceived(99, 125000, now_ms + 1000);

  coordinator_->UpdatePriorities(now_ms + 1000);
  auto map = coordinator_->ForceUpdatePriorities(now_ms + 1100);
  EXPECT_EQ(map.size(), size_t{1});
}

TEST_F(MultiAgentFlowCoordinatorTest, MultipleFlowPriorityOrdering) {
  int64_t now_ms = 1000;
  uint32_t id1 = coordinator_->RegisterFlow(10, "kv", 10000000, now_ms);
  uint32_t id2 = coordinator_->RegisterFlow(11, "lora", 5000000, now_ms);
  uint32_t id3 = coordinator_->RegisterFlow(12, "video", 20000000, now_ms);

  coordinator_->SetComputeTime(id1, 500.0, 20.0, now_ms);
  coordinator_->SetComputeTime(id2, 100.0, 10.0, now_ms);
  coordinator_->SetComputeTime(id3, 300.0, 30.0, now_ms);

  // Set high downlink rate so Johnson's Rule groups work correctly:
  // At 500Mbps: kv N=160ms<P=500 (GrpA), lora N=80ms<P=100 (GrpA), video N=320ms>P=300 (GrpB)
  // Don't feed telemetry data so uplink_rate=0 and effective_rate=downlink only
  coordinator_->SetDownlinkRate(500000000.0);  // 500 Mbps

  auto map = coordinator_->ForceUpdatePriorities(now_ms + 2000);
  EXPECT_EQ(map.size(), size_t{3});
  // Group A (N<=P): lora(N=80<P=100, rho=MAX-80), kv(N=160<P=500, rho=MAX-160)
  // Group B (N>P): video(N=320>P=300, rho=300-MAX)
  // Priority: lora > kv > video
  EXPECT_GT(map[11], map[10]);
  EXPECT_GT(map[10], map[12]);
}

TEST_F(MultiAgentFlowCoordinatorTest, MarkFlowComplete) {
  uint32_t id = coordinator_->RegisterFlow(10, "kv", 100000, 1000);
  EXPECT_EQ(coordinator_->GetActiveFlowCount(), size_t{1});
  coordinator_->MarkFlowComplete(id);
  EXPECT_EQ(coordinator_->GetActiveFlowCount(), size_t{0});
}

TEST_F(MultiAgentFlowCoordinatorTest, PriorityUpdateCallback) {
  int64_t now_ms = 1000;
  std::map<int, uint16_t> received_map;
  coordinator_->SetPriorityUpdateCallback(
      [&received_map](const std::map<int, uint16_t>& map) {
        received_map = map;
      });

  coordinator_->RegisterFlow(10, "kv", 10000000, now_ms);
  coordinator_->SetComputeTime(1, 500.0, 20.0, now_ms);
  coordinator_->OnDataReceived(99, 125000, now_ms);
  coordinator_->OnDataReceived(99, 125000, now_ms + 1000);
  coordinator_->ForceUpdatePriorities(now_ms + 1000);

  EXPECT_EQ(received_map.size(), size_t{1});
  EXPECT_NE(received_map.find(10), received_map.end());
}

TEST_F(MultiAgentFlowCoordinatorTest, EnableDisable) {
  EXPECT_TRUE(coordinator_->IsEnabled());
  coordinator_->SetEnabled(false);
  EXPECT_FALSE(coordinator_->IsEnabled());
  coordinator_->SetEnabled(true);
  EXPECT_TRUE(coordinator_->IsEnabled());
}

TEST_F(MultiAgentFlowCoordinatorTest, Clear) {
  coordinator_->RegisterFlow(10, "kv", 100000, 1000);
  coordinator_->OnDataReceived(10, 50000, 1000);
  EXPECT_EQ(coordinator_->GetFlowCount(), size_t{1});

  coordinator_->Clear();
  EXPECT_EQ(coordinator_->GetFlowCount(), size_t{0});
  EXPECT_DOUBLE_EQ(coordinator_->GetUplinkRateBps(), 0.0);
}

// ============================================================================
// Integration Test: Strict Priority Ordering with 5 Flows
// ============================================================================

TEST_F(MultiAgentFlowCoordinatorTest, StrictPriorityOrdering5Flows) {
  int64_t now_ms = 1000;

  uint32_t id1 = coordinator_->RegisterFlow(10, "flow_1", 1000000, now_ms);
  uint32_t id2 = coordinator_->RegisterFlow(11, "flow_2", 2000000, now_ms);
  uint32_t id3 = coordinator_->RegisterFlow(12, "flow_3", 3000000, now_ms);
  uint32_t id4 = coordinator_->RegisterFlow(13, "flow_4", 4000000, now_ms);
  uint32_t id5 = coordinator_->RegisterFlow(14, "flow_5", 5000000, now_ms);

  coordinator_->SetComputeTime(id1, 500.0, 20.0, now_ms);
  coordinator_->SetComputeTime(id2, 500.0, 20.0, now_ms);
  coordinator_->SetComputeTime(id3, 500.0, 20.0, now_ms);
  coordinator_->SetComputeTime(id4, 500.0, 20.0, now_ms);
  coordinator_->SetComputeTime(id5, 500.0, 20.0, now_ms);

  // Generate telemetry on separate stream
  for (int i = 0; i < 20; ++i) {
    coordinator_->OnDataReceived(999, 125000, now_ms + i * 100);
  }

  auto map = coordinator_->ForceUpdatePriorities(now_ms + 2000);
  EXPECT_EQ(map.size(), size_t{5});

  // Smallest flow first (Johnson: same P, smaller N -> higher rho)
  EXPECT_GT(map[10], map[11]);
  EXPECT_GT(map[11], map[12]);
  EXPECT_GT(map[12], map[13]);
  EXPECT_GT(map[13], map[14]);
}

}  // namespace
}  // namespace coordinator
}  // namespace webrtc
