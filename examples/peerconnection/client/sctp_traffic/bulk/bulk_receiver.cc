#include "sctp_traffic/bulk/bulk_receiver.h"

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

Receiver::Receiver(const std::string& log_dir, int log_period_ms)
    : period_ms_(log_period_ms) {
  std::string path = log_dir + "/sctp_traffic.csv";
  log_file_.open(path);
  if (log_file_.is_open()) {
    log_file_ << "Time,Throughput,Start,Stop\n";
    log_file_.flush();
  }
}
Receiver::~Receiver() {
  Detach();
}

void Receiver::Attach(Conductor& c) {
  conductor_ = &c;

  // 1) Register payload handler
  conductor_->RegisterPayloadHandler(Kind::kBulkTest,
                                     [this](absl::Span<const uint8_t> bytes) {
                                       rx_accum_ += bytes.size();
                                       rx_total_ += bytes.size();
                                     });

  // 2) Periodic log timer
  last_ms_ = NowMillis();
  running_.store(true);
  worker_ = std::thread([this]() {
    while (running_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(period_ms_));
      Tick();
    }
  });
}

void Receiver::Tick() {
  const int64_t now = NowMillis();
  const double dt = (now - last_ms_) / 1000.0;
  last_ms_ = now;

  const uint64_t bytes = rx_accum_;
  rx_accum_ = 0;
  const double mbps = dt > 0 ? (bytes * 8.0) / (dt * 1e6) : 0.0;

  if (logging_.load() && log_file_.is_open()) {
    log_file_ << now << "," << mbps << ",0,0\n";
    log_file_.flush();
  }

  std::cout << "[BULK][RX] " << mbps << " Mbps (" << bytes << " B / " << dt
            << " s), total=" << rx_total_ << " B" << std::endl;
}

void Receiver::Detach() {
  running_.store(false);
  if (worker_.joinable())
    worker_.join();
  conductor_ = nullptr;
  if (log_file_.is_open()) {
    log_file_.close();
  }
}

void Receiver::LogStart() {
  int64_t now = NowMillis();
  if (log_file_.is_open()) {
    log_file_ << now << ",0,1,0\n";
    log_file_.flush();
  }
  rx_accum_ = 0;
  last_ms_ = now;
  logging_.store(true);
}

void Receiver::LogStop() {
  int64_t now = NowMillis();
  if (log_file_.is_open()) {
    log_file_ << now << ",0,0,1\n";
    log_file_.flush();
  }
  logging_.store(false);
}

}  // namespace sctp::bulk
