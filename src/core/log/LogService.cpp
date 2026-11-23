#include "LogService.hpp"
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <chrono>
#include <atomic>

namespace desk32::core::log {

namespace {
std::atomic<uint64_t> g_connSeq{0};

QString generateSessionId() {
    return QStringLiteral("sess-") +
           QString::number(QDateTime::currentMSecsSinceEpoch()) +
           QStringLiteral("-") +
           QUuid::createUuid().toString(QUuid::Id128).left(8);
}

std::string enrichDetailsJson(const QString& detailsJson, const QString& sessionId) {
    QJsonObject obj;
    if (!detailsJson.isEmpty()) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(detailsJson.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            obj = doc.object();
        }
    }
    if (!obj.contains(QStringLiteral("v"))) {
        obj.insert(QStringLiteral("v"), 1);
    }
    if (!sessionId.isEmpty() && !obj.contains(QStringLiteral("session_id"))) {
        obj.insert(QStringLiteral("session_id"), sessionId);
    }
    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}
} // namespace

LogService& LogService::instance() {
    static LogService s_instance;
    return s_instance;
}

QString LogService::defaultDatabasePath() {
    // Section 11.1: XDG Base Directory specification ($XDG_STATE_HOME/desk32/)
    QString stateHome = qEnvironmentVariable("XDG_STATE_HOME");
    if (stateHome.isEmpty()) {
        stateHome = QDir::homePath() + QStringLiteral("/.local/state");
    }
    const QString stateDir = QDir(stateHome).filePath(QStringLiteral("desk32"));
    QDir().mkpath(stateDir);
    return QDir(stateDir).filePath(QStringLiteral("operation_logs.db"));
}

LogService::LogService(QObject* parent)
    : QObject(parent)
    , m_sessionId(generateSessionId())
{}

LogService::~LogService() {
    stop();
}

bool LogService::start(const QString& dbPath) {
    if (m_running.load()) {
        return true;
    }

    m_sessionId = generateSessionId();

    if (dbPath.isEmpty()) {
        m_dbPath = defaultDatabasePath();
    } else {
        m_dbPath = dbPath;
    }

    // Initialize query repository for main/GUI thread reads
    const uint64_t seq = g_connSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    m_queryRepo = std::make_unique<LogRepository>(QStringLiteral("Desk32LogQueryConn_%1").arg(seq));
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
    const uint64_t seq = g_connSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    m_workerRepo = std::make_unique<LogRepository>(QStringLiteral("Desk32LogWorkerConn_%1").arg(seq));
    if (!m_workerRepo->open(m_dbPath)) {
        std::lock_guard<std::mutex> lock(m_cvMutex);
        m_running.store(false);
        m_flushRequested.store(false);
        m_flushCv.notify_all();
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
    if (m_running.load(std::memory_order_relaxed) && m_queue.size() > 0) {
        flush();
    }
    return m_queryRepo->queryEvents(filter);
}

std::vector<LogEvent> LogService::queryRelatedLogs(const std::string& operationId) {
    if (!m_queryRepo) return {};
    if (m_running.load(std::memory_order_relaxed) && m_queue.size() > 0) {
        flush();
    }
    return m_queryRepo->queryRelatedEvents(operationId);
}

std::vector<LogEvent> LogService::queryPlaybackHistory(int limit) {
    if (!m_queryRepo) return {};
    if (m_running.load(std::memory_order_relaxed) && m_queue.size() > 0) {
        flush();
    }
    return m_queryRepo->queryPlaybackHistory(limit);
}

bool LogService::exportJsonl(const QString& filePath, const LogFilter& filter) {
    if (!m_queryRepo) return false;
    if (m_running.load(std::memory_order_relaxed) && m_queue.size() > 0) {
        flush();
    }
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
    const QString& parentOperationId,
    int32_t errorCode,
    double durationMs)
{
    LogLevel lvl = LogLevel::Info;
    if (status == "Failed" || status == "Error") lvl = LogLevel::Error;
    else if (status == "Warning") lvl = LogLevel::Warning;
    else if (status == "Success") lvl = LogLevel::Success;
    else if (status == "Cancelled") lvl = LogLevel::Cancelled;

    const std::string enrichedJson = enrichDetailsJson(detailsJson, m_sessionId);

    LogEvent ev = LogEvent::create(
        lvl,
        category.toStdString(),
        message.toStdString(),
        enrichedJson,
        operation.toStdString(),
        targetType.toStdString(),
        target.toStdString(),
        errorCode,
        durationMs,
        operationId.toStdString(),
        parentOperationId.toStdString()
    );
    postLog(ev);
}

namespace {
LogFilter toLogFilter(const QVariantMap& filterMap) {
    LogFilter filter;
    if (filterMap.contains("fromUtc")) filter.fromUtc = filterMap.value("fromUtc").toString().toStdString();
    if (filterMap.contains("toUtc")) filter.toUtc = filterMap.value("toUtc").toString().toStdString();
    if (filterMap.contains("category")) filter.category = filterMap.value("category").toString().toStdString();
    if (filterMap.contains("status")) filter.status = filterMap.value("status").toString().toStdString();
    if (filterMap.contains("operationId")) filter.operationId = filterMap.value("operationId").toString().toStdString();
    if (filterMap.contains("parentOperationId")) filter.parentOperationId = filterMap.value("parentOperationId").toString().toStdString();
    if (filterMap.contains("target")) filter.target = filterMap.value("target").toString().toStdString();
    if (filterMap.contains("searchText")) filter.searchText = filterMap.value("searchText").toString().toStdString();
    if (filterMap.contains("sessionId")) filter.sessionId = filterMap.value("sessionId").toString().toStdString();
    if (filterMap.contains("playbackHistoryOnly")) filter.playbackHistoryOnly = filterMap.value("playbackHistoryOnly").toBool();
    if (filterMap.contains("limit")) filter.limit = filterMap.value("limit").toInt();
    if (filterMap.contains("offset")) filter.offset = filterMap.value("offset").toInt();
    return filter;
}
} // namespace

QVariantList LogService::fetchLogs(const QVariantMap& filterMap) {
    const LogFilter filter = toLogFilter(filterMap);

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

QVariantList LogService::fetchRelatedLogs(const QString& operationId) {
    if (operationId.isEmpty()) return {};
    auto events = queryRelatedLogs(operationId.toStdString());
    // If operationId is a child operation (e.g. RENDER_xxx_TRACK_1), resolve its parentOperationId
    // so the entire parent + sibling child hierarchy is returned.
    std::string rootOpId = operationId.toStdString();
    for (const auto& ev : events) {
        if (ev.operationId == rootOpId && !ev.parentOperationId.empty()) {
            rootOpId = ev.parentOperationId;
            events = queryRelatedLogs(rootOpId);
            break;
        }
    }

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

QVariantList LogService::fetchPlaybackHistory(int limit) {
    auto events = queryPlaybackHistory(limit);
    QVariantList list;
    for (const auto& ev : events) {
        QVariantMap m;
        m["eventId"] = QVariant::fromValue(ev.eventId);
        m["timestampUtc"] = QString::fromStdString(ev.timestampUtc);
        m["localUtcOffset"] = QString::fromStdString(ev.localUtcOffset);
        m["category"] = QString::fromStdString(ev.category);
        m["operation"] = QString::fromStdString(ev.operation);
        m["message"] = QString::fromStdString(ev.message);

        QJsonObject det;
        if (!ev.detailsJson.empty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(ev.detailsJson));
            if (doc.isObject()) det = doc.object();
        }
        const QString deck = det.value(QStringLiteral("deck")).toString(QString::fromStdString(ev.target));
        QString trackPath = det.value(QStringLiteral("path")).toString();
        if (trackPath.isEmpty()) {
            trackPath = det.value(QStringLiteral("first_track_path")).toString();
        }
        if (trackPath.isEmpty() && ev.targetType == "File") {
            trackPath = QString::fromStdString(ev.target);
        }
        m["deck"] = deck;
        m["trackPath"] = trackPath;
        m["fileName"] = trackPath.isEmpty() ? QString() : QFileInfo(trackPath).fileName();
        m["durationMs"] = det.value(QStringLiteral("duration_ms_track")).toDouble(0.0);
        m["positionMs"] = det.value(QStringLiteral("position_ms")).toDouble(0.0);
        m["detailsJson"] = QString::fromStdString(ev.detailsJson);
        list.append(m);
    }
    return list;
}

bool LogService::exportToJsonl(const QString& filePath) {
    return exportJsonl(filePath);
}

bool LogService::exportFilteredToJsonl(const QString& filePath, const QVariantMap& filterMap) {
    return exportJsonl(filePath, toLogFilter(filterMap));
}

} // namespace desk32::core::log

