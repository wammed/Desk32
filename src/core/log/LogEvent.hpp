#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <atomic>

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
        case LogLevel::Error: return "Failed"; // Section 10.5 specifies "Failed"
        case LogLevel::Success: return "Success";
        case LogLevel::Cancelled: return "Cancelled";
    }
    return "Info";
}

inline LogLevel stringToLogLevel(const std::string& str) {
    if (str == "Warning") return LogLevel::Warning;
    if (str == "Error" || str == "Failed") return LogLevel::Error;
    if (str == "Success") return LogLevel::Success;
    if (str == "Cancelled") return LogLevel::Cancelled;
    return LogLevel::Info;
}

/// Operation Log Event Record
/// Strictly adheres to Section 10.5, Decision D-1, and Decision D-3 (Full 14-field schema).
struct LogEvent {
    int64_t eventId = 0;              ///< 1. event_id (INTEGER PRIMARY KEY)
    std::string operationId;          ///< 2. operation_id (UUID / Unique operation identifier)
    std::string parentOperationId;    ///< 3. parent_operation_id (Nullable parent identifier for hierarchical ops)
    std::string timestampUtc;         ///< 4. timestamp_utc (ISO 8601 UTC string: YYYY-MM-DDTHH:MM:SS.sssZ)
    std::string localUtcOffset;       ///< 5. local_utc_offset (Local timezone offset: +HH:MM or -HH:MM)
    std::string category;             ///< 6. category (Deck, FolderSession, AudioBackend, DSP, Render, System)
    std::string operation;            ///< 7. operation (LoadTrack, SwitchDeck, Seek, StartSession, RenderTrack, etc.)
    std::string status = "Info";      ///< 8. status (Success, Failed, Warning, Cancelled, Info)
    std::string targetType = "System";///< 9. target_type (Deck, File, Folder, Plugin, Device, DSP, System)
    std::string target;               ///< 10. target ("Deck A", file path, plugin name, etc.)
    std::string message;              ///< 11. message (Human-readable summary message)
    int32_t errorCode = 0;            ///< 12. error_code (Numeric error code or 0 for success)
    std::string detailsJson = "{}";   ///< 13. details_json (Structured JSON parameters or error details)
    double durationMs = 0.0;          ///< 14. duration_ms (Operation execution duration in milliseconds)

    // Backward compatibility members for Phase 0 Step 4 tests
    int64_t id = 0;
    LogLevel level = LogLevel::Info;

    static std::string generateOperationId() {
        static std::atomic<uint64_t> seq{1};
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        return "op-" + std::to_string(ms) + "-" + std::to_string(seq.fetch_add(1, std::memory_order_relaxed));
    }

    static LogEvent create(
        LogLevel lvl,
        const std::string& cat,
        const std::string& msg,
        const std::string& json = "{}",
        const std::string& op = "",
        const std::string& tgtType = "System",
        const std::string& tgt = "",
        int32_t errCode = 0,
        double durMs = 0.0,
        const std::string& opId = "",
        const std::string& parentOpId = "")
    {
        LogEvent ev;
        ev.level = lvl;
        ev.category = cat;
        ev.operation = op.empty() ? cat : op;
        ev.status = (lvl == LogLevel::Error) ? "Failed" : logLevelToString(lvl);
        ev.targetType = tgtType;
        ev.target = tgt;
        ev.message = msg;
        ev.errorCode = errCode;
        ev.detailsJson = json.empty() ? "{}" : json;
        ev.durationMs = durMs;
        ev.operationId = opId.empty() ? generateOperationId() : opId;
        ev.parentOperationId = parentOpId;

        // Current time
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        std::time_t tt = std::chrono::system_clock::to_time_t(now);

        // UTC timestamp (ISO 8601 UTC with ms)
        std::tm gmTm{};
        gmtime_r(&tt, &gmTm);
        std::ostringstream utcOss;
        utcOss << std::put_time(&gmTm, "%Y-%m-%dT%H:%M:%S")
               << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
        ev.timestampUtc = utcOss.str();

        // Local UTC offset (+HH:MM or -HH:MM)
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

