#include "sctp_traffic/bulk/bulk_sender.h"

#include <chrono>
#include <iostream>
#include <thread>

#include "examples/peerconnection/client/conductor.h"

namespace {
int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

using Kind = Conductor::TrafficKind;

namespace sctp::bulk {

Sender::Sender(Config cfg) : cfg_(cfg) {}
Sender::~Sender() {
  Stop();
}

void Sender::Start(Conductor& c) {
  conductor_ = &c;

  target_bps_ = cfg_.target_mbps * 1e6;
  payload_.assign(cfg_.chunk_bytes, 0x00);

  credit_bytes_ = 0.0;
  last_ms_ = NowMillis();

  running_.store(true);
  worker_ = std::thread([this]() {
    while (running_.load()) {
      PumpOnce(NowMillis());
      std::this_thread::sleep_for(
          std::chrono::milliseconds(cfg_.pump_interval_ms));
    }
  });

  std::cout << "[BULK][TX] started: " << cfg_.target_mbps
            << " Mbps, chunk=" << cfg_.chunk_bytes << " B" << std::endl;
}

void Sender::Stop() {
  running_.store(false);
  if (worker_.joinable())
    worker_.join();
  conductor_ = nullptr;
  std::cout << "[BULK][TX] stopped" << std::endl;
}

void Sender::PumpOnce(int64_t now_ms) {
  if (!conductor_ || !conductor_->IsFlowOpen(Kind::kBulkTest))
    return;

  const double dt = (now_ms - last_ms_) / 1000.0;
  last_ms_ = now_ms;
  credit_bytes_ += target_bps_ * dt / 8.0;

  if (conductor_->BufferedAmount(Kind::kBulkTest) > cfg_.buffered_cap)
    return;

  size_t sent_bytes = 0;
  while (credit_bytes_ >= static_cast<double>(payload_.size())) {
    if (!conductor_->SendPayload(
            Kind::kBulkTest,
            absl::Span<const uint8_t>(payload_.data(), payload_.size()))) {
      std::cout << "[BULK][TX] send blocked by backpressure" << std::endl;
      break;
    }
    credit_bytes_ -= payload_.size();
    sent_bytes += payload_.size();

    if (conductor_->BufferedAmount(Kind::kBulkTest) > cfg_.buffered_cap)
      break;
  }

  if (sent_bytes > 0 && dt > 0) {
    const double mbps = (sent_bytes * 8.0) / (dt * 1e6);
    std::cout << "[BULK][TX] ~" << mbps << " Mbps, buffered="
              << conductor_->BufferedAmount(Kind::kBulkTest) << std::endl;
  }
}

}  // namespace sctp::bulk
