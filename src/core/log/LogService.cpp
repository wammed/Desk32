#include "LogService.hpp"
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>
#include <QDateTime>
#include <chrono>

namespace desk32::core::log {

LogService& LogService::instance() {
    static LogService s_instance;
    return s_instance;
}

LogService::LogService(QObject* parent)
    : QObject(parent)
{}

LogService::~LogService() {
    stop();
}

bool LogService::start(const QString& dbPath) {
    if (m_running.load()) {
        return true;
    }

    if (dbPath.isEmpty()) {
        // Section 11.1: XDG Base Directory specification ($XDG_STATE_HOME/desk32/)
        QString stateDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QDir().mkpath(stateDir);
        m_dbPath = stateDir + "/operation_logs.db";
    } else {
        m_dbPath = dbPath;
    }

    // Initialize query repository for main/GUI thread reads
    m_queryRepo = std::make_unique<LogRepository>("Desk32LogQueryConn");
    if (!m_queryRepo->open(m_dbPath)) {
        return false;
    }

    m_running.store(true);
    m_workerThread = std::thread(&LogService::workerLoop, this);
    return true;
}

void LogService::stop() {
    if (!m_running.load()) {
        return;
    }

    m_running.store(false);
    {
        std::lock_guard<std::mutex> lock(m_cvMutex);
        m_cv.notify_all();
    }

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    if (m_queryRepo) {
        m_queryRepo->close();
        m_queryRepo.reset();
    }
}

void LogService::postLog(const LogEvent& ev) {
    m_queue.push(ev);
    {
        std::lock_guard<std::mutex> lock(m_cvMutex);
        m_cv.notify_one();

        // Notify UI via queued signal under m_cvMutex to serialize QPostEventList access across std::threads
        QMetaObject::invokeMethod(this, [this, ev]() {
            emit logAppended(
                QString::fromStdString(ev.category),
                QString::fromStdString(ev.operation),
                QString::fromStdString(ev.status),
                QString::fromStdString(ev.message)
            );
        }, Qt::QueuedConnection);
    }
}

void LogService::postRealtimeLog(const LogEvent& ev) {
    // Strictly non-blocking push for real-time Audio Thread (workerLoop drains periodically or on flush)
    m_queue.try_push(ev);
}

void LogService::flush() {
    if (!m_running.load()) return;

    std::unique_lock<std::mutex> lock(m_cvMutex);
    m_flushRequested.store(true);
    m_cv.notify_all();
    m_flushCv.wait(lock, [this]() {
        return m_queue.size() == 0 && !m_flushRequested.load();
    });
}

void LogService::workerLoop() {
    // Dedicated worker SQLite connection
    m_workerRepo = std::make_unique<LogRepository>("Desk32LogWorkerConn");
    if (!m_workerRepo->open(m_dbPath)) {
        return;
    }

    // Initial D-1 retention enforcement on startup
    m_workerRepo->purgeOldEvents(180, 300000);

    auto lastPurgeTime = std::chrono::steady_clock::now();

    while (m_running.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lock(m_cvMutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(50), [this]() {
            return !m_running.load(std::memory_order_relaxed) ||
                   m_queue.size() > 0 ||
                   m_flushRequested.load(std::memory_order_relaxed);
        });

        processBatch();

        // Periodic retention enforcement (every 30 minutes)
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::minutes>(now - lastPurgeTime).count() >= 30) {
            m_workerRepo->purgeOldEvents(180, 300000);
            lastPurgeTime = now;
        }

        if (m_flushRequested.load(std::memory_order_relaxed)) {
            m_flushRequested.store(false);
            m_flushCv.notify_all();
        }
    }

    // Final drain before shutdown
    processBatch();

    m_workerRepo->close();
    m_workerRepo.reset();
}

void LogService::processBatch() {
    auto batch = m_queue.drain();
    if (!batch.empty() && m_workerRepo) {
        m_workerRepo->insertBatch(batch);
    }

    // Check if any audio-thread real-time logs were dropped
    size_t dropped = m_queue.resetOverflowCount();
    if (dropped > 0 && m_workerRepo) {
        LogEvent overflowEv = LogEvent::create(
            LogLevel::Warning,
            "System",
            "Log queue dropped " + std::to_string(dropped) + " events to preserve Audio Thread real-time safety.",
            "{\"dropped_events\":" + std::to_string(dropped) + "}",
            "QueueOverflow",
            "Queue",
            "",
            1001
        );
        m_workerRepo->insertEvent(overflowEv);
    }
}

std::vector<LogEvent> LogService::queryLogs(const LogFilter& filter) {
    if (!m_queryRepo) return {};
    return m_queryRepo->queryEvents(filter);
}

std::vector<LogEvent> LogService::queryRelatedLogs(const std::string& operationId) {
    if (!m_queryRepo) return {};
    return m_queryRepo->queryRelatedEvents(operationId);
}

bool LogService::exportJsonl(const QString& filePath, const LogFilter& filter) {
    if (!m_queryRepo) return false;
    return m_queryRepo->exportToJsonl(filePath, filter);
}

int64_t LogService::countEvents() {
    if (!m_queryRepo) return 0;
    return m_queryRepo->countEvents();
}

int64_t LogService::countOperations() {
    if (!m_queryRepo) return 0;
    return m_queryRepo->countOperations();
}

int LogService::purgeOldEvents(int maxRetentionDays, int64_t maxEvents) {
    if (!m_queryRepo) return 0;
    return m_queryRepo->purgeOldEvents(maxRetentionDays, maxEvents);
}

int LogService::purgeOldOperations(int maxRetentionDays, int64_t maxOperations) {
    if (!m_queryRepo) return 0;
    return m_queryRepo->purgeOldOperations(maxRetentionDays, maxOperations);
}

void LogService::log(
    const QString& category,
    const QString& operation,
    const QString& status,
    const QString& message,
    const QString& targetType,
    const QString& target,
    const QString& detailsJson)
{
    log(category, operation, status, message, targetType, target, detailsJson, "", "");
}

void LogService::log(
    const QString& category,
    const QString& operation,
    const QString& status,
    const QString& message,
    const QString& targetType,
    const QString& target,
    const QString& detailsJson,
    const QString& operationId,
    const QString& parentOperationId)
{
    LogLevel lvl = LogLevel::Info;
    if (status == "Failed" || status == "Error") lvl = LogLevel::Error;
    else if (status == "Warning") lvl = LogLevel::Warning;
    else if (status == "Success") lvl = LogLevel::Success;
    else if (status == "Cancelled") lvl = LogLevel::Cancelled;

    LogEvent ev = LogEvent::create(
        lvl,
        category.toStdString(),
        message.toStdString(),
        detailsJson.toStdString(),
        operation.toStdString(),
        targetType.toStdString(),
        target.toStdString(),
        0,
        0.0,
        operationId.toStdString(),
        parentOperationId.toStdString()
    );
    postLog(ev);
}

QVariantList LogService::fetchLogs(const QVariantMap& filterMap) {
    LogFilter filter;
    if (filterMap.contains("category")) filter.category = filterMap.value("category").toString().toStdString();
    if (filterMap.contains("status")) filter.status = filterMap.value("status").toString().toStdString();
    if (filterMap.contains("searchText")) filter.searchText = filterMap.value("searchText").toString().toStdString();
    if (filterMap.contains("limit")) filter.limit = filterMap.value("limit").toInt();
    if (filterMap.contains("offset")) filter.offset = filterMap.value("offset").toInt();

    auto events = queryLogs(filter);
    QVariantList list;
    for (const auto& ev : events) {
        QVariantMap m;
        m["eventId"] = QVariant::fromValue(ev.eventId);
        m["operationId"] = QString::fromStdString(ev.operationId);
        m["parentOperationId"] = QString::fromStdString(ev.parentOperationId);
        m["timestampUtc"] = QString::fromStdString(ev.timestampUtc);
        m["localUtcOffset"] = QString::fromStdString(ev.localUtcOffset);
        m["category"] = QString::fromStdString(ev.category);
        m["operation"] = QString::fromStdString(ev.operation);
        m["status"] = QString::fromStdString(ev.status);
        m["targetType"] = QString::fromStdString(ev.targetType);
        m["target"] = QString::fromStdString(ev.target);
        m["message"] = QString::fromStdString(ev.message);
        m["errorCode"] = ev.errorCode;
        m["detailsJson"] = QString::fromStdString(ev.detailsJson);
        m["durationMs"] = ev.durationMs;
        list.append(m);
    }
    return list;
}

bool LogService::exportToJsonl(const QString& filePath) {
    return exportJsonl(filePath);
}

} // namespace desk32::core::log

