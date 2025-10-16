#pragma once

#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>

namespace trdp_sim {

enum class LogLevel {
    Error = 0,
    Warn,
    Info,
    Debug
};

class Logger {
public:
    explicit Logger(LogLevel level = LogLevel::Info);

    void set_level(LogLevel level);
    void enable_console(bool enable);
    void set_file(std::ostream *stream);

    void error(const std::string &message);
    void warn(const std::string &message);
    void info(const std::string &message);
    void debug(const std::string &message);

private:
    void log(LogLevel level, const std::string &message);

    LogLevel level_;
    bool consoleEnabled_;
    std::ostream *fileStream_;
    std::mutex mutex_;
};

std::string log_level_to_string(LogLevel level);

}  // namespace trdp_sim
