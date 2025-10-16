#include "trdp_simulator/logger.hpp"

namespace trdp_sim {

namespace {
std::string timestamp()
{
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    auto time = clock::to_time_t(now);
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tmBuffer{};
#ifdef _WIN32
    localtime_s(&tmBuffer, &time);
#else
    localtime_r(&time, &tmBuffer);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tmBuffer, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
        << millis.count();
    return oss.str();
}
}  // namespace

Logger::Logger(LogLevel level)
    : level_(level), consoleEnabled_(true), fileStream_(nullptr)
{
}

void Logger::set_level(LogLevel level)
{
    level_ = level;
}

void Logger::enable_console(bool enable)
{
    consoleEnabled_ = enable;
}

void Logger::set_file(std::ostream *stream)
{
    std::scoped_lock lock(mutex_);
    fileStream_ = stream;
}

void Logger::error(const std::string &message)
{
    log(LogLevel::Error, message);
}

void Logger::warn(const std::string &message)
{
    log(LogLevel::Warn, message);
}

void Logger::info(const std::string &message)
{
    log(LogLevel::Info, message);
}

void Logger::debug(const std::string &message)
{
    log(LogLevel::Debug, message);
}

void Logger::log(LogLevel level, const std::string &message)
{
    if (level > level_) {
        return;
    }

    std::ostringstream oss;
    oss << '[' << timestamp() << "] [" << log_level_to_string(level) << "] " << message;
    const auto formatted = oss.str();

    std::scoped_lock lock(mutex_);
    if (consoleEnabled_) {
        std::ostream &stream = (level == LogLevel::Error || level == LogLevel::Warn) ? std::cerr : std::cout;
        stream << formatted << std::endl;
    }
    if (fileStream_) {
        (*fileStream_) << formatted << std::endl;
    }
}

std::string log_level_to_string(LogLevel level)
{
    switch (level) {
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Debug:
        return "DEBUG";
    }
    return "UNKNOWN";
}

}  // namespace trdp_sim
