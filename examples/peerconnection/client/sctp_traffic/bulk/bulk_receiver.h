#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>

#include "sctp_traffic/traffic.h"

class Conductor;

namespace sctp::bulk {

class Receiver final : public sctp::Receiver {
 public:
  Receiver(const std::string& log_dir, int log_period_ms = 1000);
  ~Receiver() override;

  void Attach(Conductor& c) override;
  void Detach() override;

  // Log start/stop events triggered by UI buttons.
  void LogStart();
  void LogStop();

 private:
  void Tick();

  Conductor* conductor_ = nullptr;
  int period_ms_;
  std::thread worker_;
  std::atomic<bool> running_{false};

  uint64_t rx_accum_ = 0;
  uint64_t rx_total_ = 0;
  int64_t last_ms_ = 0;

  std::ofstream log_file_;
  std::atomic<bool> logging_{false};
};

}  // namespace sctp::bulk
