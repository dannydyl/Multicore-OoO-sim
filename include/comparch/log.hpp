#pragma once

#include <iostream>
#include <optional>
#include <string_view>

namespace comparch {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

namespace log_detail {

inline LogLevel& threshold() {
    static LogLevel t = LogLevel::Info;
    return t;
}

const char* tag(LogLevel level);

} // namespace log_detail

inline void set_log_level(LogLevel level) {
    log_detail::threshold() = level;
}

inline LogLevel get_log_level() {
    return log_detail::threshold();
}

inline bool log_enabled(LogLevel level) {
    return level >= log_detail::threshold();
}

std::optional<LogLevel> parse_log_level(std::string_view s);

} // namespace comparch

#define CASIM_LOG(level, expr)                                                    \
    do {                                                                          \
        if (::comparch::log_enabled(level)) {                                     \
            std::cerr << "[" << ::comparch::log_detail::tag(level) << "] "        \
                      << expr << '\n';                                            \
        }                                                                         \
    } while (0)

#define LOG_TRACE(expr) CASIM_LOG(::comparch::LogLevel::Trace, expr)
#define LOG_DEBUG(expr) CASIM_LOG(::comparch::LogLevel::Debug, expr)
#define LOG_INFO(expr)  CASIM_LOG(::comparch::LogLevel::Info,  expr)
#define LOG_WARN(expr)  CASIM_LOG(::comparch::LogLevel::Warn,  expr)
#define LOG_ERROR(expr) CASIM_LOG(::comparch::LogLevel::Error, expr)
