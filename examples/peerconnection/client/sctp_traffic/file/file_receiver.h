#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "examples/peerconnection/client/sctp_traffic/traffic.h"
#include "examples/peerconnection/client/sctp_traffic/file/file_message.h"

class Conductor;  // Forward declaration.

namespace sctp::file {

// Receives SCTP "file" traffic and optionally persists it to disk.
class Receiver final : public sctp::Receiver {
 public:
  Receiver(int kind, std::string label, std::string output_dir, int slo_ms);
  ~Receiver() override;

  void Attach(Conductor& c) override;
  void Detach() override;

 private:
  void HandlePayload(absl::Span<const uint8_t> bytes);
  std::string MakeOutputPath() const;

  struct PendingFile {
    uint64_t file_size_bytes = 0;
    uint32_t chunk_count = 0;
    // Removed: uint32_t next_chunk_index = 0;  // This enforced sequential ordering
    uint32_t chunks_received = 0;  // Added: Count of chunks received so far
    std::vector<bool> received_chunks;  // Added: Bitmap to track which chunks we've received
    uint64_t received_bytes = 0;
    uint64_t first_send_time_ms = 0;  // send_time_ms of first chunk
    uint64_t latest_send_time_ms = 0;
    int64_t first_arrival_time_ms = 0;  // arrival time of first chunk
    int64_t last_arrival_time_ms = 0;
  };

  Conductor* conductor_ = nullptr;
  int kind_;
  std::string label_;
  std::string output_dir_;
  int slo_ms_;

  std::mutex state_mutex_;
  std::ofstream csv_file_;
  std::unordered_map<uint64_t, PendingFile> in_flight_files_;
  uint64_t total_data_bytes_ = 0;
  uint64_t total_files_received_ = 0;
  uint64_t slo_met_count_ = 0;
  bool log_started_ = false;
  bool sender_time_initialized_ = false;
  uint64_t first_sender_timestamp_ms_ = 0;
  bool sender_start_time_initialized_ = false;
  int64_t sender_start_time_ms_ = 0;
};

}  // namespace sctp::file