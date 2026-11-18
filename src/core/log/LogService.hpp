#pragma once

#include "LogEvent.hpp"
#include "LogRepository.hpp"
#include "BoundedLogQueue.hpp"
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QString>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>

namespace desk32::core::log {

/// LogService: Asynchronous, Non-blocking Audio-safe Logging Engine
/// Fulfills Section 10.5, Decision D-1 (180 days / 300,000 events), and Decision D-3.
class LogService : public QObject {
    Q_OBJECT

public:
    static LogService& instance();

    explicit LogService(QObject* parent = nullptr);
    ~LogService() override;

    /// Initialize logging database and start background LogWorkerThread
    bool start(const QString& dbPath = "");

    /// Flush all pending logs and terminate worker thread cleanly
    void stop();

    /// Post log from GUI or non-realtime thread (normal push)
    void postLog(const LogEvent& ev);

    /// Post log from real-time Audio Thread (strictly non-blocking, zero allocation)
    void postRealtimeLog(const LogEvent& ev);

    /// Synchronously flush all queued events to disk (for unit testing and clean shutdown)
    void flush();

    /// Direct query methods
    std::vector<LogEvent> queryLogs(const LogFilter& filter = {});
    std::vector<LogEvent> queryRelatedLogs(const std::string& operationId);
    std::vector<LogEvent> queryPlaybackHistory(int limit = 200);
    bool exportJsonl(const QString& filePath, const LogFilter& filter = {});

    int64_t countEvents();
    int64_t countOperations();
    int purgeOldEvents(int maxRetentionDays = 180, int64_t maxEvents = 300000);
    int purgeOldOperations(int maxRetentionDays = 180, int64_t maxOperations = 300000);

    [[nodiscard]] size_t droppedLogCount() const noexcept {
        return m_queue.overflowCount();
    }

    Q_INVOKABLE [[nodiscard]] QString sessionId() const { return m_sessionId; }

    // QML-invokable APIs
    Q_INVOKABLE void log(
        const QString& category,
        const QString& operation,
        const QString& status,
        const QString& message,
        const QString& targetType = "System",
        const QString& target = "",
        const QString& detailsJson = "{}");

    void log(
        const QString& category,
        const QString& operation,
        const QString& status,
        const QString& message,
        const QString& targetType,
        const QString& target,
        const QString& detailsJson,
        const QString& operationId,
        const QString& parentOperationId = "",
        int32_t errorCode = 0,
        double durationMs = 0.0);

    Q_INVOKABLE QVariantList fetchLogs(const QVariantMap& filter);
    Q_INVOKABLE QVariantList fetchRelatedLogs(const QString& operationId);
    Q_INVOKABLE QVariantList fetchPlaybackHistory(int limit = 200);
    Q_INVOKABLE bool exportToJsonl(const QString& filePath);
    Q_INVOKABLE bool exportFilteredToJsonl(const QString& filePath, const QVariantMap& filter);

signals:
    void logAppended(const QString& category, const QString& operation, const QString& status, const QString& message);

private:
    void workerLoop();
    void processBatch();

    QString m_dbPath;
    QString m_sessionId;
    BoundedLogQueue<LogEvent, 4096> m_queue;

    std::thread m_workerThread;
    std::mutex m_cvMutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_flushRequested{false};
    std::condition_variable m_flushCv;

    std::unique_ptr<LogRepository> m_workerRepo;
    std::unique_ptr<LogRepository> m_queryRepo;
};

} // namespace desk32::core::log

