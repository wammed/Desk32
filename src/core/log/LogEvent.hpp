#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace desk32::core::log {

enum class LogLevel {
    Info,
    Warning,
    Error,
    Success,
    Cancelled
};

inline std::string logLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Info: return "Info";
        case LogLevel::Warning: return "Warning";
        case LogLevel::Error: return "Error";
        case LogLevel::Success: return "Success";
        case LogLevel::Cancelled: return "Cancelled";
    }
    return "Info";
}

inline LogLevel stringToLogLevel(const std::string& str) {
    if (str == "Warning") return LogLevel::Warning;
    if (str == "Error") return LogLevel::Error;
    if (str == "Success") return LogLevel::Success;
    if (str == "Cancelled") return LogLevel::Cancelled;
    return LogLevel::Info;
}

struct LogEvent {
    int64_t id = 0;
    std::string timestampUtc;    // e.g. "2026-11-07T06:25:00.123Z"
    std::string localUtcOffset;  // e.g. "+09:00"
    LogLevel level = LogLevel::Info;
    std::string category;        // e.g. "AudioEngine", "Deck", "ExternalInsert", "Render"
    std::string message;
    std::string detailsJson;

    static LogEvent create(LogLevel lvl, const std::string& cat, const std::string& msg, const std::string& json = "{}") {
        LogEvent ev;
        ev.level = lvl;
        ev.category = cat;
        ev.message = msg;
        ev.detailsJson = json;

        // Current time
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        std::time_t tt = std::chrono::system_clock::to_time_t(now);

        // UTC timestamp
        std::tm gmTm{};
        gmtime_r(&tt, &gmTm);
        std::ostringstream utcOss;
        utcOss << std::put_time(&gmTm, "%Y-%m-%dT%H:%M:%S")
               << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
        ev.timestampUtc = utcOss.str();

        // Local UTC offset
        std::tm locTm{};
        localtime_r(&tt, &locTm);
        long offsetSec = locTm.tm_gmtoff;
        long offsetHours = offsetSec / 3600;
        long offsetMinutes = std::abs(offsetSec % 3600) / 60;
        std::ostringstream offOss;
        offOss << (offsetSec >= 0 ? '+' : '-')
               << std::setfill('0') << std::setw(2) << std::abs(offsetHours) << ':'
               << std::setfill('0') << std::setw(2) << offsetMinutes;
        ev.localUtcOffset = offOss.str();

        return ev;
    }
};

} // namespace desk32::core::log

