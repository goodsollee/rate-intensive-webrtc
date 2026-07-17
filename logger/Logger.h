#pragma once

#include <string>
#include <iostream>
#include <fstream>
#include <mutex>
#include <ctime>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <set>
#include <iomanip>

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    CRITICAL
};

class Logger {
public:
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    void setLogLevel(LogLevel level) { currentLevel = level; }
    void setLogFile(const std::string& filename);

    // Add methods to control module filtering
    void setModuleFilter(const std::string& moduleName) {
        std::lock_guard<std::mutex> lock(logMutex);
        enabledModules.clear();
        if (moduleName != "all") {
            enabledModules.insert(moduleName);
        }
    }

    void addModuleFilter(const std::string& moduleName) {
        std::lock_guard<std::mutex> lock(logMutex);
        enabledModules.insert(moduleName);
    }

    void clearModuleFilters() {
        std::lock_guard<std::mutex> lock(logMutex);
        enabledModules.clear();
    }

    template<typename... Args>
    void log(LogLevel level, const char* module, const char* file, const int line, Args... args) {
        if (level < currentLevel) return;

        // Check if module is enabled for logging
        {
            std::lock_guard<std::mutex> lock(logMutex);
            if (!enabledModules.empty() && enabledModules.find(module) == enabledModules.end()) {
                return;
            }
        }

        std::stringstream ss;

        // Get the current time with milliseconds precision
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        // Format the timestamp with milliseconds
        std::stringstream timestamp;
        timestamp << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S")
                << ':' << std::setw(3) << std::setfill('0') << milliseconds.count();

        // Extract just the filename from the path
        std::string filename = std::filesystem::path(file).filename().string();

        ss << timestamp.str() << " [" << getLevelString(level) << "] "
           << "[" << module << "] "  // Add module name to log output
           << "[" << filename << ":" << line << "] ";
        logOne(ss, std::forward<Args>(args)...);
        ss << std::endl;

        std::lock_guard<std::mutex> lock(logMutex);
        std::cout << ss.str();
        if (logFile.is_open()) {
            logFile << ss.str();
            logFile.flush();
        }
    }

private:
    Logger() : currentLevel(LogLevel::INFO) {}
    ~Logger() { if(logFile.is_open()) logFile.close(); }

    template<typename T>
    void logOne(std::stringstream& ss, T&& arg) {
        ss << std::forward<T>(arg);
    }

    template<typename T, typename... Args>
    void logOne(std::stringstream& ss, T&& arg, Args&&... args) {
        ss << std::forward<T>(arg);
        logOne(ss, std::forward<Args>(args)...);
    }

    const char* getLevelString(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO: return "INFO";
            case LogLevel::WARNING: return "WARNING";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::CRITICAL: return "CRITICAL";
            default: return "UNKNOWN";
        }
    }

    LogLevel currentLevel;
    std::ofstream logFile;
    std::mutex logMutex;
    std::set<std::string> enabledModules;  // Store enabled module names
};

// Updated macros to include module name
#define LOG_DEBUG(module, ...) Logger::getInstance().log(LogLevel::DEBUG, module, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(module, ...) Logger::getInstance().log(LogLevel::INFO, module, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARNING(module, ...) Logger::getInstance().log(LogLevel::WARNING, module, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(module, ...) Logger::getInstance().log(LogLevel::ERROR, module, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_CRITICAL(module, ...) Logger::getInstance().log(LogLevel::CRITICAL, module, __FILE__, __LINE__, __VA_ARGS__)
