#ifndef NETWORK_EMULATOR_H_
#define NETWORK_EMULATOR_H_

#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <algorithm> // For sorting
#include <chrono>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include "../../logger/Logger.h"

class NetworkEmulator {
public:
    struct NetworkProfile {
        int64_t timestamp_ms;
        double bandwidth_kbps;
        double latency_ms;
    };

    NetworkEmulator();
    ~NetworkEmulator();

    // Initialize emulator with network profile and interface names
    bool Initialize(const std::string& profile_path,
                   const std::string& interface_name,
                   const std::string& peer_interface_name,
                   const std::string& bandwidth_log_path = "");

    // Interface management
    bool CreateVirtualInterface();
    void DeleteVirtualInterface();

    // Emulation control
    void Start();
    void Stop();

    // Getters for interface info
    std::string GetInterfaceName() const { return interface_name_; }
    std::string GetPeerInterfaceName() const { return peer_interface_name_; }
    bool IsRunning() const { return is_running_; }

private:
    bool ParseProfileFile();
    void EmulationLoop();
    void ApplyNetworkConditions(double bandwidth_kbps, double latency_ms);

    // Async TC command execution
    void TcWorkerLoop();
    void EnqueueTcCommand(std::function<void()> command);
    void ApplyNetworkConditionsAsync(double bandwidth_kbps, double latency_ms);
    void ApplyNetworkConditionsSync(double bandwidth_kbps, double latency_ms, int limit);
    void LogBandwidthChange(double bandwidth_kbps, double latency_ms);
    void ReportBandwidthLoggingStatus() const;

    std::string profile_path_;
    std::string interface_name_;
    std::string peer_interface_name_;
    std::string bandwidth_log_path_;
    std::vector<NetworkProfile> network_profiles_;
    std::thread emulation_thread_;
    std::atomic<bool> is_running_;
    size_t current_profile_index_;
    bool qdisc_installed_;
    double last_bandwidth_kbps_;
    double last_latency_ms_;
    std::chrono::steady_clock::time_point last_update_time_;
    std::chrono::steady_clock::time_point start_time_;
    bool start_time_initialized_;
    bool bandwidth_log_header_written_;
    std::mutex bandwidth_log_mutex_;

    // Async TC command execution members
    std::thread tc_worker_thread_;
    std::queue<std::function<void()>> tc_command_queue_;
    std::mutex tc_queue_mutex_;
    std::condition_variable tc_queue_cv_;
    std::atomic<bool> tc_worker_running_;
};

#endif // NETWORK_EMULATOR_H_
