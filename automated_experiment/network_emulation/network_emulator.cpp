#include "network_emulator.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstdlib>
#include <atomic>
#include <cmath>


static const char* NETWORK_EMULATOR_MODULE_NAME = "PHY";

NetworkEmulator::NetworkEmulator()
    : is_running_(false),
      current_profile_index_(0),
      qdisc_installed_(false),
      last_bandwidth_kbps_(0.0),
      last_latency_ms_(0.0),
      last_update_time_(std::chrono::steady_clock::time_point::min()),
      start_time_(std::chrono::steady_clock::time_point::min()),
      start_time_initialized_(false),
      bandwidth_log_header_written_(false),
      tc_worker_running_(false) {
    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "NetworkEmulator initialized");
}

NetworkEmulator::~NetworkEmulator() {
    Stop();
    DeleteVirtualInterface();
    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "NetworkEmulator destroyed");
}

bool NetworkEmulator::Initialize(const std::string& profile_path,
                                  const std::string& interface_name,
                                  const std::string& peer_interface_name,
                                  const std::string& bandwidth_log_path) {
    profile_path_ = profile_path;
    interface_name_ = interface_name;
    peer_interface_name_ = peer_interface_name;
    bandwidth_log_path_ = bandwidth_log_path;
    bandwidth_log_header_written_ = false;

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Initializing NetworkEmulator");
    if (!profile_path_.empty()) {
        LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Profile path provided: ", profile_path_);
    } else {
        LOG_WARNING(NETWORK_EMULATOR_MODULE_NAME, "No profile path provided. Running without profile.");
    }

    if (!CreateVirtualInterface()) {
        LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to create virtual interface");
        return false;
    }

    if (!profile_path_.empty() && !ParseProfileFile()) {
        LOG_WARNING(NETWORK_EMULATOR_MODULE_NAME, "Profile parsing failed or no valid profiles found");
    }

    if (!bandwidth_log_path_.empty()) {
        std::lock_guard<std::mutex> lock(bandwidth_log_mutex_);
        std::ofstream file(bandwidth_log_path_, std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            file << "elapsed_ms,bandwidth_kbps,latency_ms" << std::endl;
            bandwidth_log_header_written_ = true;
            LOG_INFO(NETWORK_EMULATOR_MODULE_NAME,
                     "Bandwidth changes will be logged to ", bandwidth_log_path_);
        } else {
            LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME,
                      "Failed to open bandwidth log file: ", bandwidth_log_path_);
        }
    }

    ReportBandwidthLoggingStatus();

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME,
             "Type 'start' to begin traffic shaping...");

    std::ofstream ready_file("/tmp/emulator_ready.signal");
    if (ready_file.is_open()) {
        ready_file << "ready" << std::endl;
        ready_file.close();
        LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Signal file created at /tmp/emulator_ready.signal");
    } else {
        LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to create signal file");
    }

    return true;
}

bool NetworkEmulator::CreateVirtualInterface() {
    // Clean up any existing setup
    std::string cleanup_cmd = R"(
        sudo ip netns pids ns1 2>/dev/null | xargs -r kill
        sudo ip netns del ns1 2>/dev/null
        sudo ip link del veth0 2>/dev/null
        sudo ip link del veth1 2>/dev/null
        sudo ip link del veth_host 2>/dev/null
        sudo ip link del veth_ns 2>/dev/null
        sudo rm -rf /etc/netns/ns1 2>/dev/null
    )";
    system(cleanup_cmd.c_str());

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Create network namespace
    std::string cmd = "sudo ip netns add ns1";
    if (system(cmd.c_str()) != 0) {
        LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to create namespace ns1");
        return false;
    }

    // Create veth pair for connectivity
    cmd = "sudo ip link add veth_host type veth peer name veth_ns";
    if (system(cmd.c_str()) != 0) {
        LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to create veth pair");
        return false;
    }

    // Move veth_ns to namespace
    cmd = "sudo ip link set veth_ns netns ns1";
    if (system(cmd.c_str()) != 0) {
        LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to move veth_ns to namespace");
        return false;
    }

    // Configure IP addresses
    cmd = "sudo ip addr add 192.168.100.1/24 dev veth_host";
    system(cmd.c_str());
    cmd = "sudo ip netns exec ns1 ip addr add 192.168.100.2/24 dev veth_ns";
    system(cmd.c_str());

    // Bring up interfaces
    cmd = "sudo ip link set veth_host up";
    system(cmd.c_str());
    cmd = "sudo ip netns exec ns1 ip link set veth_ns up";
    system(cmd.c_str());
    cmd = "sudo ip netns exec ns1 ip link set lo up";
    system(cmd.c_str());

    // Set up NAT in host to allow internet access from namespace
    cmd = "sudo sysctl -w net.ipv4.ip_forward=1";
    system(cmd.c_str());
    cmd = "sudo iptables -t nat -A POSTROUTING -s 192.168.100.0/24 -o " + interface_name_ + " -j MASQUERADE";
    system(cmd.c_str());
    cmd = "sudo iptables -A FORWARD -i " + interface_name_ + " -o veth_host -j ACCEPT";
    system(cmd.c_str());
    cmd = "sudo iptables -A FORWARD -o " + interface_name_ + " -i veth_host -j ACCEPT";
    system(cmd.c_str());

    // Set up routing in namespace
    cmd = "sudo ip netns exec ns1 ip route add default via 192.168.100.1";
    system(cmd.c_str());

    // Set up DNS in namespace
    cmd = "sudo mkdir -p /etc/netns/ns1";
    system(cmd.c_str());
    cmd = "sudo bash -c 'echo \"nameserver 8.8.8.8\nnameserver 8.8.4.4\" > /etc/netns/ns1/resolv.conf'";
    system(cmd.c_str());

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Virtual interfaces created and configured successfully");
    return true;
}

void NetworkEmulator::DeleteVirtualInterface() {
    // TC qdiscs first
    std::string cmd = "sudo ip netns exec ns1 tc qdisc del dev veth_ns root 2>/dev/null";
    system(cmd.c_str());

    // Clean up NAT rules
    cmd = "sudo iptables -t nat -D POSTROUTING -s 192.168.100.0/24 -o " + interface_name_ + " -j MASQUERADE 2>/dev/null";
    system(cmd.c_str());
    cmd = "sudo iptables -D FORWARD -i " + interface_name_ + " -o veth_host -j ACCEPT 2>/dev/null";
    system(cmd.c_str());
    cmd = "sudo iptables -D FORWARD -o " + interface_name_ + " -i veth_host -j ACCEPT 2>/dev/null";
    system(cmd.c_str());

    // Kill all processes in namespace
    cmd = "sudo ip netns pids ns1 2>/dev/null | xargs -r kill -9";
    system(cmd.c_str());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Delete virtual interfaces and namespace
    cmd = "sudo ip link del veth_host 2>/dev/null";
    system(cmd.c_str());

    cmd = "sudo ip netns del ns1 2>/dev/null";
    system(cmd.c_str());
    cmd = "sudo rm -rf /etc/netns/ns1 2>/dev/null";
    system(cmd.c_str());

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Virtual interfaces deleted and cleaned up");
}

void NetworkEmulator::Start() {
    if (is_running_)
        return;

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Starting emulation loop");
    is_running_ = true;
    tc_worker_running_ = true;
    start_time_ = std::chrono::steady_clock::now();
    start_time_initialized_ = true;

    // Start TC worker thread first
    tc_worker_thread_ = std::thread(&NetworkEmulator::TcWorkerLoop, this);

    // Then start emulation thread
    emulation_thread_ = std::thread(&NetworkEmulator::EmulationLoop, this);

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Emulation and TC worker threads created");
}

void NetworkEmulator::Stop() {
    if (!is_running_)
        return;

    is_running_ = false;
    tc_worker_running_ = false;

    // Wake up TC worker thread
    tc_queue_cv_.notify_all();

    if (emulation_thread_.joinable())
        emulation_thread_.join();
    if (tc_worker_thread_.joinable())
        tc_worker_thread_.join();

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Stopped network emulation");
}

bool NetworkEmulator::ParseProfileFile() {
    std::ifstream file(profile_path_);
    if (!file.is_open()) {
        LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to open network profile file: ", profile_path_);
        return false;
    }

    std::string line;
    // Skip header
    std::getline(file, line);

    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string token;
        NetworkProfile profile;

        // Parse CSV format: timestamp,bandwidth,latency
        std::getline(ss, token, ',');
        profile.timestamp_ms = std::stoll(token);
        std::getline(ss, token, ',');
        profile.bandwidth_kbps = std::stod(token);
        std::getline(ss, token, ',');
        profile.latency_ms = std::stod(token);

        network_profiles_.push_back(profile);
    }

    if (network_profiles_.empty()) {
        LOG_WARNING(NETWORK_EMULATOR_MODULE_NAME, "No valid profiles found in network profile file");
        return false;
    }

    if (!network_profiles_.empty()) {
        // Normalize timestamps relative to first entry
        int64_t base_timestamp = network_profiles_[0].timestamp_ms;
        for (auto& profile : network_profiles_) {
            profile.timestamp_ms -= base_timestamp;
        }
    }

    // Sort profiles by timestamp to ensure correct order
    std::sort(network_profiles_.begin(), network_profiles_.end(),
              [](const NetworkProfile& a, const NetworkProfile& b) {
                  return a.timestamp_ms < b.timestamp_ms;
              });

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Parsed and sorted ", network_profiles_.size(), " profiles from file");
    return true;
}

void NetworkEmulator::TcWorkerLoop() {
    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "TC worker thread started");

    while (tc_worker_running_) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(tc_queue_mutex_);
            tc_queue_cv_.wait(lock, [this] {
                return !tc_command_queue_.empty() || !tc_worker_running_;
            });

            if (!tc_worker_running_ && tc_command_queue_.empty()) {
                break;
            }

            if (!tc_command_queue_.empty()) {
                task = std::move(tc_command_queue_.front());
                tc_command_queue_.pop();
            }
        }

        if (task) {
            task();
        }
    }

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "TC worker thread stopped");
}

void NetworkEmulator::EnqueueTcCommand(std::function<void()> command) {
    {
        std::lock_guard<std::mutex> lock(tc_queue_mutex_);
        tc_command_queue_.push(std::move(command));
    }
    tc_queue_cv_.notify_one();
}

void NetworkEmulator::EmulationLoop() {
    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Entering emulation loop");

    using Clock = std::chrono::steady_clock;
    using Ms = std::chrono::milliseconds;

    auto start_time = Clock::now();

    while (is_running_) {
        if (network_profiles_.empty()) {
            LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "No profiles loaded, stopping emulation");
            break;
        }

        if (current_profile_index_ >= network_profiles_.size()) {
            LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "End of traffic shaping");
            break;
        }

        const auto& current_profile = network_profiles_[current_profile_index_];

        // Calculate absolute time when this profile should be applied
        auto target_time = start_time + Ms(current_profile.timestamp_ms);
        auto now = Clock::now();

        // Check if we're behind schedule
        if (now > target_time && current_profile_index_ > 0) {
            auto delay = std::chrono::duration_cast<Ms>(now - target_time);
            if (delay.count() > 50) { // Warn if more than 50ms behind
                LOG_WARNING(NETWORK_EMULATOR_MODULE_NAME,
                           "Behind schedule by ", delay.count(),
                           " ms at profile index ", current_profile_index_);
            }
        }

        // Wait until target time (precise sleep)
        if (target_time > now) {
            std::this_thread::sleep_until(target_time);
        }

        // Apply network conditions asynchronously (non-blocking!)
        ApplyNetworkConditionsAsync(current_profile.bandwidth_kbps,
                                    current_profile.latency_ms);

        current_profile_index_++;
    }

    is_running_ = false;

    // Signal TC worker to stop and wait for pending commands to finish
    tc_worker_running_ = false;
    tc_queue_cv_.notify_all();

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME, "Exiting emulation loop");
}

void NetworkEmulator::ApplyNetworkConditionsAsync(double bandwidth_kbps, double latency_ms) {
    constexpr double kEpsilon = 1e-3;

    // Skip if no change
    if (qdisc_installed_) {
        if (std::fabs(bandwidth_kbps - last_bandwidth_kbps_) < kEpsilon &&
            std::fabs(latency_ms - last_latency_ms_) < kEpsilon) {
            return;
        }
    }

    // Calculate buffer size (BDP-based)
    double bdp_packets = (bandwidth_kbps * latency_ms) / (8.0 * 1.5);
    int limit = static_cast<int>(std::max(2000.0, bdp_packets * 5.0)); // 5x BDP
    limit = std::min(limit, 1000000);

    // Enqueue TC command to worker thread
    EnqueueTcCommand([this, bandwidth_kbps, latency_ms, limit]() {
        ApplyNetworkConditionsSync(bandwidth_kbps, latency_ms, limit);
    });

    // Update last values immediately (for next comparison)
    last_bandwidth_kbps_ = bandwidth_kbps;
    last_latency_ms_ = latency_ms;
}

void NetworkEmulator::ApplyNetworkConditionsSync(double bandwidth_kbps,
                                                 double latency_ms,
                                                 int limit) {
    auto start = std::chrono::steady_clock::now();

    std::string cmd;

    if (!qdisc_installed_) {
        // Initial setup: HTB + netem hierarchy

        // 1. HTB root
        cmd = "sudo ip netns exec ns1 tc qdisc add dev veth_ns root handle 1: htb default 1 2>/dev/null";
        if (system(cmd.c_str()) != 0) {
            LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to add HTB root");
            return;
        }

        // 2. HTB class (bandwidth control)
        cmd = "sudo ip netns exec ns1 tc class add dev veth_ns parent 1: classid 1:1 htb rate " +
              std::to_string(bandwidth_kbps) + "kbit ceil " +
              std::to_string(bandwidth_kbps * 1.2) + "kbit burst 15k cburst 15k";
        if (system(cmd.c_str()) != 0) {
            LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to add HTB class");
            return;
        }

        // 3. netem child (latency control)
        cmd = "sudo ip netns exec ns1 tc qdisc add dev veth_ns parent 1:1 handle 10: netem delay " +
              std::to_string(latency_ms) + "ms limit " + std::to_string(limit);
        if (system(cmd.c_str()) != 0) {
            LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME, "Failed to add netem qdisc");
            return;
        }

        qdisc_installed_ = true;
    } else {
        // Update: use 'change' command (preserves queue)

        // Change HTB rate
        cmd = "sudo ip netns exec ns1 tc class change dev veth_ns parent 1: classid 1:1 htb rate " +
              std::to_string(bandwidth_kbps) + "kbit ceil " +
              std::to_string(bandwidth_kbps * 1.2) + "kbit burst 15k cburst 15k 2>/dev/null";

        int ret = system(cmd.c_str());
        if (ret != 0) {
            LOG_WARNING(NETWORK_EMULATOR_MODULE_NAME, "HTB class change failed (code: ", ret, ")");
        }

        // Change netem delay
        cmd = "sudo ip netns exec ns1 tc qdisc change dev veth_ns parent 1:1 handle 10: netem delay " +
              std::to_string(latency_ms) + "ms limit " + std::to_string(limit) + " 2>/dev/null";

        ret = system(cmd.c_str());
        if (ret != 0) {
            LOG_WARNING(NETWORK_EMULATOR_MODULE_NAME, "netem qdisc change failed (code: ", ret, ")");
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    LOG_INFO(NETWORK_EMULATOR_MODULE_NAME,
             "Applied - Rate: ", bandwidth_kbps, " kbps, Delay: ", latency_ms,
             " ms, Limit: ", limit, " packets (took ", duration.count(), " ms)");

    last_update_time_ = end;
    LogBandwidthChange(bandwidth_kbps, latency_ms);
}

// Legacy synchronous method kept for compatibility
void NetworkEmulator::ApplyNetworkConditions(double bandwidth_kbps, double latency_ms) {
    // Redirect to async version
    ApplyNetworkConditionsAsync(bandwidth_kbps, latency_ms);
}

void NetworkEmulator::LogBandwidthChange(double bandwidth_kbps, double latency_ms) {
    if (bandwidth_log_path_.empty() || !start_time_initialized_) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto zero =  std::chrono::steady_clock::time_point{};
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - zero);

    std::lock_guard<std::mutex> lock(bandwidth_log_mutex_);
    std::ofstream file;
    std::ios_base::openmode mode = std::ios::out | std::ios::app;
    file.open(bandwidth_log_path_, mode);

    if (!file.is_open()) {
        LOG_ERROR(NETWORK_EMULATOR_MODULE_NAME,
                  "Unable to append bandwidth log entry to ", bandwidth_log_path_);
        return;
    }

    if (!bandwidth_log_header_written_) {
        file << "timestamp_ms,bandwidth_kbps,latency_ms" << std::endl;
        bandwidth_log_header_written_ = true;
    }

    file << elapsed.count() << "," << bandwidth_kbps << "," << latency_ms << std::endl;
}

void NetworkEmulator::ReportBandwidthLoggingStatus() const {
    if (bandwidth_log_path_.empty()) {
        LOG_INFO(NETWORK_EMULATOR_MODULE_NAME,
                 "Bandwidth logging disabled; run with --bandwidth_log_path=<path> to enable CSV output.");
        return;
    }

    if (bandwidth_log_header_written_) {
        LOG_INFO(NETWORK_EMULATOR_MODULE_NAME,
                 "Bandwidth logging initialized successfully.");
    } else {
        LOG_WARNING(NETWORK_EMULATOR_MODULE_NAME,
                    "Bandwidth logging requested but the log file could not be prepared.");
    }
}
