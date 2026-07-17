#include "examples/peerconnection/client/sctp_traffic/file/file_sender.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include "absl/types/span.h"
#include "examples/peerconnection/client/conductor.h"
#include "modules/rtp_rtcp/source/byte_io.h"
#include "rtc_base/byte_order.h"
#include "rtc_base/logging.h"

static bool ParseIntStrict(const std::string& s, int& out) {
  if (s.empty()) return false;

  size_t i = 0, j = s.size();
  while (i < j && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  while (j > i && std::isspace(static_cast<unsigned char>(s[j-1]))) --j;
  if (i >= j) return false;

  errno = 0;
  char* end = nullptr;
  long v = std::strtol(s.c_str() + i, &end, 10);

  if (errno != 0) return false;
  if (end == (s.c_str() + i)) return false;
  while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
  if (*end != '\0') return false;

  if (v < INT_MIN || v > INT_MAX) return false;
  out = static_cast<int>(v);
  return true;
}

namespace sctp::file {

Sender::Sender(int kind, int file_size, int periodicity_ms)
    : kind_(kind), file_size_(file_size), periodicity_ms_(periodicity_ms),
      mode_(Mode::kPeriodic) {}

Sender::Sender(int kind, const std::string& trace_path)
    : kind_(kind), mode_(Mode::kCustom) {
  LoadTrace(trace_path);
}

Sender::~Sender() { 
  Stop(); 
}

void Sender::EnableHighSpeedMode(bool enable) {
  high_speed_mode_ = enable;
  if (enable) {
    std::cout << "[SCTP][FILE][Sender] Ultra high-speed mode enabled - "
              << "max_batch=" << Config::MAX_BATCH_SIZE
              << " buffer_threshold=" << (Config::MAX_BUFFER_THRESHOLD / (1024*1024)) << "MB"
              << std::endl;
  }
}

void Sender::Start(Conductor& c) {
  conductor_ = &c;
  running_.store(true);

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_chunks_.clear();
    pump_active_ = false;
    current_chunk_count_ = 0;
    current_chunks_sent_ = 0;
    current_bytes_sent_ = 0;
    last_progress_logged_chunk_ = 0;
  }

  high_water_mark_bytes_ = Config::MAX_BUFFER_THRESHOLD;
  low_water_mark_bytes_ = Config::MAX_BUFFER_THRESHOLD / 2;

  const auto kind_enum = static_cast<Conductor::TrafficKind>(kind_);
  conductor_->ConfigureBufferedAmountLowCallback(
      kind_enum, low_water_mark_bytes_, [this]() { PumpMoreData(); });
  callback_registered_ = true;

  // Initialize logging
  {
    std::lock_guard<std::mutex> lock(log_mutex_);
    total_data_bytes_sent_ = 0;
    log_started_ = false;
    log_path_ = MakeLogPath();
    csv_log_.open(log_path_, std::ios::out | std::ios::trunc);
    if (csv_log_.is_open()) {
      csv_log_ << "timestamp_ms,chunk_bytes,total_data_bytes,file_bytes,sequence,chunk_index,chunk_count,send_time_ms\n";
      csv_log_.flush();
    }
  }
  
  std::cout << "[SCTP][FILE][Sender] Starting sender kind=" << kind_ 
            << " mode=" << (mode_ == Mode::kPeriodic ? "periodic" : "custom")
            << " high_speed=" << high_speed_mode_ << std::endl;
  
  if (mode_ == Mode::kPeriodic) {
    worker_ = std::thread([this]() { RunPeriodic(); });
  } else {
    worker_ = std::thread([this]() { RunCustom(); });
  }
}

void Sender::Stop() {
  running_.store(false);
  queue_cv_.notify_all();

  const auto kind_enum = static_cast<Conductor::TrafficKind>(kind_);
  if (callback_registered_ && conductor_) {
    conductor_->ConfigureBufferedAmountLowCallback(kind_enum,
                                                   low_water_mark_bytes_,
                                                   nullptr);
    callback_registered_ = false;
  }

  if (worker_.joinable()) {
    worker_.join();
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_chunks_.clear();
  }

  conductor_ = nullptr;

  uint64_t total_bytes = 0;
  {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (csv_log_.is_open()) {
      csv_log_.flush();
      csv_log_.close();
    }
    total_bytes = total_data_bytes_sent_;
  }
  
  std::cout << "[SCTP][FILE][Sender] Stopped. Total bytes sent: " 
            << total_bytes << std::endl;
}

void Sender::RunPeriodic() {
  while (running_.load()) {
    if (!IsFlowReady()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    
    SendFileBatched(file_size_);
    
    if (periodicity_ms_ > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(periodicity_ms_));
    }
  }
}

void Sender::RunCustom() {
  using clock = std::chrono::steady_clock;

  // Wait for channel to open
  while (running_.load() && !IsFlowReady()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!running_.load()) return;

  const auto t0 = clock::now();

  for (const auto& ev : custom_events_) {
    if (!running_.load()) break;

    const auto due = t0 + std::chrono::milliseconds(ev.first);
    
    // Precise timing loop
    while (running_.load() && clock::now() < due) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (!running_.load()) break;

    if (IsFlowReady()) {
      SendFileBatched(ev.second);
    }
  }
}

void Sender::LoadTrace(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) return;

  auto trim = [](std::string& s) {
    auto issp = [](unsigned char c){ return std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char c){ return !issp(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [&](unsigned char c){ return !issp(c); }).base(), s.end());
  };

  std::string line;
  bool first = true;
  while (std::getline(file, line)) {
    if (line.empty()) continue;

    if (first) {
      first = false;
      if (std::any_of(line.begin(), line.end(),
                      [](unsigned char c){ return std::isalpha(c); })) {
        continue;
      }
    }

    std::istringstream ss(line);
    std::string t_s, size_s;
    if (!std::getline(ss, t_s, ',')) continue;
    if (!std::getline(ss, size_s, ',')) continue;
    trim(t_s); trim(size_s);

    int t = 0, sz = 0;
    if (!ParseIntStrict(t_s, t)) continue;
    if (!ParseIntStrict(size_s, sz)) continue;
    if (t < 0 || sz < 0) continue;

    custom_events_.emplace_back(t, sz);
  }

  std::sort(custom_events_.begin(), custom_events_.end(),
            [](const auto& a, const auto& b){ return a.first < b.first; });
}

bool Sender::IsFlowReady() {
  return conductor_ &&
         conductor_->IsFlowOpen(static_cast<Conductor::TrafficKind>(kind_));
}

void Sender::PumpMoreData() {
  if (!running_.load()) {
    return;
  }

  Conductor* conductor = conductor_;
  if (!conductor) {
    return;
  }

  const auto kind_enum = static_cast<Conductor::TrafficKind>(kind_);
  std::unique_lock<std::mutex> lock(queue_mutex_);
  if (pump_active_) {
    return;
  }
  pump_active_ = true;

  while (running_.load() && conductor && !pending_chunks_.empty()) {
    if (conductor->BufferedAmount(kind_enum) >= high_water_mark_bytes_) {
      break;
    }

    if (!conductor->IsFlowOpen(kind_enum)) {
      std::cout << "[SCTP][FILE][Sender] Flow closed while draining buffer"
                << std::endl;
      pending_chunks_.clear();
      queue_cv_.notify_all();
      break;
    }

    PendingChunk chunk = std::move(pending_chunks_.front());
    pending_chunks_.pop_front();

    lock.unlock();
    bool sent = conductor->SendPayload(
        kind_enum,
        absl::Span<const uint8_t>(chunk.payload.data(), chunk.payload.size()));
    lock.lock();

    if (!sent) {
      pending_chunks_.emplace_front(std::move(chunk));
      break;
    }

    current_chunks_sent_++;
    current_bytes_sent_ += chunk.metadata.chunk_bytes;

    const size_t chunk_count = current_chunk_count_;
    const size_t chunks_sent = current_chunks_sent_;
    const uint64_t bytes_sent = current_bytes_sent_;
    const size_t chunk_number = chunk.metadata.chunk_index + 1;
    bool should_log_progress = false;
    if (chunk_count > 0) {
      const size_t log_interval =
          std::max(static_cast<size_t>(1), chunk_count / 20);
      const bool final_chunk = chunk_number == chunk_count;
      if (final_chunk || (chunk_number % log_interval) == 0) {
        if (final_chunk || chunk_number > last_progress_logged_chunk_) {
          last_progress_logged_chunk_ = chunk_number;
          should_log_progress = true;
        }
      }
    }

    queue_cv_.notify_all();
    const uint64_t current_buffer = conductor->BufferedAmount(kind_enum);
    const auto chunk_time = std::chrono::steady_clock::now();
    auto metadata = chunk.metadata;

    lock.unlock();
    LogSendEvent(metadata);
    if (should_log_progress) {
      MaybeLogProgress(chunk_count, chunks_sent, bytes_sent, current_buffer,
                       chunk_time);
    }
    lock.lock();
  }

  pump_active_ = false;
  if (pending_chunks_.empty()) {
    queue_cv_.notify_all();
  }
}

void Sender::MaybeLogProgress(
    size_t chunk_count,
    size_t chunks_sent,
    uint64_t bytes_sent,
    uint64_t current_buffer,
    std::chrono::steady_clock::time_point chunk_time) {
  if (chunk_count == 0) {
    return;
  }


  // Per-chunk logs
  /*
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      chunk_time - current_file_start_time_);
  double rate_mbps = 0.0;
  if (elapsed.count() > 0) {
    rate_mbps =
        (bytes_sent * 8.0 / 1'000'000.0) /
        (static_cast<double>(elapsed.count()) / 1000.0);
  }

  std::ostringstream oss;
  oss << "[SCTP][FILE][Sender] Progress: " << chunks_sent << "/"
      << chunk_count << " (" << std::fixed << std::setprecision(1)
      << (100.0 * chunks_sent / chunk_count) << "%) "
      << "rate=" << std::setprecision(1) << rate_mbps << "Mbps "
      << "buffer=" << current_buffer
      << " limit=" << high_water_mark_bytes_;
  std::cout << oss.str() << std::endl; */
}

void Sender::SendFileBatched(size_t file_bytes) {
  using clock = std::chrono::steady_clock;

  if (file_bytes == 0) {
    return;
  }

  const auto now = clock::now();
  if (!flow_start_time_initialized_) {
    flow_start_time_ = now;
    flow_start_time_initialized_ = true;
  }

  const uint64_t sequence = next_sequence_++;
  const auto file_send_start_time = now;
  const uint64_t send_time_ms = rtc::TimeMillis();

  const auto kind_enum = static_cast<Conductor::TrafficKind>(kind_);

  size_t max_message = conductor_->MaxSctpMessageSize(kind_enum);
  size_t max_chunk_payload = file_bytes;
  if (max_message > kFileChunkHeaderSize + 1) {
    max_chunk_payload = max_message - kFileChunkHeaderSize - 1;
  }
  if (max_chunk_payload == 0) {
    max_chunk_payload = 256 * 1024;
  }
  max_chunk_payload =
      std::min(max_chunk_payload, static_cast<size_t>(8*1024));
  size_t chunk_count =
      (file_bytes + max_chunk_payload - 1) / max_chunk_payload;

  std::cout << "[SCTP][FILE][Sender] HIGH-THROUGHPUT transmission: "
            << "seq=" << sequence << " file_bytes=" << file_bytes
            << " chunks=" << chunk_count
            << " chunk_size=" << max_chunk_payload << std::endl;

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    current_chunk_count_ = chunk_count;
    current_chunks_sent_ = 0;
    current_bytes_sent_ = 0;
    current_file_start_time_ = file_send_start_time;
    last_progress_logged_chunk_ = 0;
  }

  for (size_t chunk_index = 0;
       chunk_index < chunk_count && running_.load(); ++chunk_index) {
    if (!conductor_->IsFlowOpen(kind_enum)) {
      std::cout << "[SCTP][FILE][Sender] Flow closed, aborting transmission"
                << std::endl;
      break;
    }

    const size_t chunk_start_byte = chunk_index * max_chunk_payload;
    const size_t remaining_bytes = file_bytes - chunk_start_byte;
    const size_t chunk_bytes =
        std::min(max_chunk_payload, remaining_bytes);

    //const auto chunk_time = clock::now();

    PayloadMetadata metadata;
    metadata.sequence = sequence;
    metadata.send_time_ms = send_time_ms;
    metadata.file_bytes = file_bytes;
    metadata.chunk_bytes = chunk_bytes;
    metadata.chunk_index = static_cast<uint32_t>(chunk_index);
    metadata.chunk_count = static_cast<uint32_t>(chunk_count);

    auto payload = BuildChunkPayload(metadata);
    PendingChunk pending;
    pending.payload = std::move(payload);
    pending.metadata = metadata;

    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      pending_chunks_.push_back(std::move(pending));
    }

    PumpMoreData();

    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_cv_.wait(lock, [this]() {
      return !running_.load() ||
             pending_chunks_.size() < Config::MAX_BATCH_SIZE;
    });
    if (!running_.load()) {
      break;
    }
  }

  PumpMoreData();

  std::unique_lock<std::mutex> drain_lock(queue_mutex_);
  queue_cv_.wait(drain_lock, [this]() {
    return !running_.load() || pending_chunks_.empty();
  });
  const size_t chunks_sent = current_chunks_sent_;
  const size_t chunk_total = current_chunk_count_;
  const uint64_t bytes_sent = current_bytes_sent_;
  drain_lock.unlock();

  const auto file_send_end_time = clock::now();
  const uint64_t total_duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          file_send_end_time - file_send_start_time)
          .count();

  double effective_rate_mbps = 0.0;
  if (total_duration_ms > 0) {
    effective_rate_mbps =
        (bytes_sent * 8.0 / 1'000'000.0) /
        (static_cast<double>(total_duration_ms) / 1000.0);
  }

  const double completion_rate =
      chunk_total > 0 ? (100.0 * chunks_sent / chunk_total) : 100.0;

  const char* result =
      (chunks_sent == chunk_total && chunk_total > 0) ? "COMPLETED" : "PARTIAL";
  std::ostringstream summary;
  summary << "[SCTP][FILE][Sender] " << result << ": seq=" << sequence
          << " chunks=" << chunks_sent << "/" << chunk_total
          << " (" << std::fixed << std::setprecision(1) << completion_rate
          << "%) "
          << " bytes=" << bytes_sent << "/" << file_bytes
          << " duration_ms=" << total_duration_ms
          << " rate_mbps=" << std::setprecision(2) << effective_rate_mbps
          << " buffer_high_water=" << high_water_mark_bytes_;
  std::cout << summary.str() << std::endl;
}

std::vector<uint8_t> Sender::BuildChunkPayload(const PayloadMetadata& metadata) {
  std::vector<uint8_t> payload(kFileChunkHeaderSize + metadata.chunk_bytes, 0);
  FileChunkHeader header;
  header.sequence = metadata.sequence;
  header.send_time_ms = metadata.send_time_ms;
  header.file_size_bytes = metadata.file_bytes;
  header.chunk_size_bytes = metadata.chunk_bytes;
  header.chunk_index = metadata.chunk_index;
  header.chunk_count = metadata.chunk_count;
  WriteFileChunkHeader(payload.data(), header);
  return payload;
}

void Sender::LogSendEvent(const PayloadMetadata& metadata) {
  using clock = std::chrono::steady_clock;
  
  std::lock_guard<std::mutex> lock(log_mutex_);
  
  const auto now = clock::now();
  if (!log_started_) {
    log_start_time_ = now;
    log_started_ = true;
  }
  
  double timestamp_ms =
      std::chrono::duration<double, std::milli>(now - log_start_time_).count();
  
  total_data_bytes_sent_ += metadata.chunk_bytes;
  
  if (csv_log_.is_open()) {
    csv_log_ << std::fixed << std::setprecision(3) << timestamp_ms << ","
             << metadata.chunk_bytes << "," << total_data_bytes_sent_ << ","
             << metadata.file_bytes << "," << metadata.sequence << ","
             << metadata.chunk_index << "," << metadata.chunk_count << ","
             << metadata.send_time_ms << "\n";
    
    // Flush less frequently for performance
    if (metadata.chunk_index % 100 == 0) {
      csv_log_.flush();
    }
  }
}

std::string Sender::MakeLogPath() const {
  std::ostringstream oss;
  oss << "sctp_file_sender_kind" << kind_;
  if (mode_ == Mode::kPeriodic) {
    oss << "_periodic";
  } else {
    oss << "_custom";
  }
  if (high_speed_mode_) {
    oss << "_highspeed";
  }
  oss << ".csv";
  return oss.str();
}

}  // namespace sctp::file