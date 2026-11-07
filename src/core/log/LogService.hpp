#pragma once

#include "LogEvent.hpp"
#include "LogRepository.hpp"
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

/// Bounded Log Queue for RT-safe log transfer
template <typename T, size_t Capacity = 4096>
class BoundedLogQueue {
public:
    /// Non-blocking push for Real-time Audio Thread
    /// Returns false immediately if lock cannot be acquired or queue is full.
    bool try_push(const T& item) {
        std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            m_overflowCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (m_queue.size() >= Capacity) {
            m_overflowCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_queue.push_back(item);
        return true;
    }

    /// Normal thread push (GUI / worker threads)
    bool push(const T& item) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.size() >= Capacity) {
            m_overflowCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_queue.push_back(item);
        return true;
    }

    /// Drain all pending items into a vector
    std::vector<T> drain() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<T> result;
        result.swap(m_queue);
        return result;
    }

    [[nodiscard]] size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

    [[nodiscard]] size_t overflowCount() const noexcept {
        return m_overflowCount.load(std::memory_order_relaxed);
    }

    size_t resetOverflowCount() noexcept {
        return m_overflowCount.exchange(0, std::memory_order_relaxed);
    }

private:
    mutable std::mutex m_mutex;
    std::vector<T> m_queue;
    std::atomic<size_t> m_overflowCount{0};
};

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
    bool exportJsonl(const QString& filePath, const LogFilter& filter = {});

    int64_t countEvents();
    int purgeOldEvents(int maxRetentionDays = 180, int64_t maxEvents = 300000);

    [[nodiscard]] size_t droppedLogCount() const noexcept {
        return m_queue.overflowCount();
    }

    // QML-invokable APIs
    Q_INVOKABLE void log(
        const QString& category,
        const QString& operation,
        const QString& status,
        const QString& message,
        const QString& targetType = "System",
        const QString& target = "",
        const QString& detailsJson = "{}");

    Q_INVOKABLE QVariantList fetchLogs(const QVariantMap& filter);
    Q_INVOKABLE bool exportToJsonl(const QString& filePath);

signals:
    void logAppended(const QString& category, const QString& operation, const QString& status, const QString& message);

private:
    void workerLoop();
    void processBatch();

    QString m_dbPath;
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

