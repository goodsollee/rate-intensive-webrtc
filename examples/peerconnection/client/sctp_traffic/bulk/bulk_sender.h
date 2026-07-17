#pragma once
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "sctp_traffic/traffic.h"

class Conductor;

namespace sctp::bulk {

struct Config {
  double target_mbps = 500.0;
  size_t chunk_bytes = 16 * 1024;
  uint64_t buffered_cap = 8 * 1024 * 1024;
  int pump_interval_ms = 10;
};

class Sender final : public sctp::Sender {
 public:
  explicit Sender(Config cfg = {});
  ~Sender() override;

  void Start(Conductor& c) override;
  void Stop() override;

 private:
  void PumpOnce(int64_t now_ms);

  Conductor* conductor_ = nullptr;
  Config cfg_;
  std::thread worker_;
  std::atomic<bool> running_{false};

  std::vector<uint8_t> payload_;
  double target_bps_ = 0.0;
  double credit_bytes_ = 0.0;
  int64_t last_ms_ = 0;
};

}  // namespace sctp::bulk
