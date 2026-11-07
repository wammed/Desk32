#pragma once

#include "LogEvent.hpp"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <vector>
#include <memory>
#include <mutex>

namespace desk32::core::log {

class LogRepository {
public:
    explicit LogRepository(const QString& connectionName = "Desk32LogDb")
        : m_connectionName(connectionName)
    {}

    ~LogRepository() {
        close();
    }

    bool open(const QString& dbPath) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (QSqlDatabase::contains(m_connectionName)) {
            m_db = QSqlDatabase::database(m_connectionName);
        } else {
            m_db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
        }

        m_db.setDatabaseName(dbPath);
        if (!m_db.open()) {
            m_lastError = m_db.lastError().text().toStdString();
            return false;
        }

        // Enable WAL mode for high concurrency and crash resilience
        QSqlQuery pragma(m_db);
        pragma.exec("PRAGMA journal_mode = WAL;");
        pragma.exec("PRAGMA synchronous = NORMAL;");

        // Create table and indices
        QSqlQuery q(m_db);
        QString createTableSql = R"(
            CREATE TABLE IF NOT EXISTS operation_logs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp_utc TEXT NOT NULL,
                local_utc_offset TEXT NOT NULL,
                level TEXT NOT NULL,
                category TEXT NOT NULL,
                message TEXT NOT NULL,
                details_json TEXT
            );
        )";
        if (!q.exec(createTableSql)) {
            m_lastError = q.lastError().text().toStdString();
            return false;
        }

        if (!q.exec("CREATE INDEX IF NOT EXISTS idx_logs_timestamp ON operation_logs (timestamp_utc);")) {
            m_lastError = q.lastError().text().toStdString();
            return false;
        }

        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_db.isOpen()) {
            m_db.close();
        }
    }

    bool insertEvent(const LogEvent& ev) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_db.isOpen()) return false;

        QSqlQuery q(m_db);
        q.prepare(R"(
            INSERT INTO operation_logs (timestamp_utc, local_utc_offset, level, category, message, details_json)
            VALUES (?, ?, ?, ?, ?, ?)
        )");
        q.addBindValue(QString::fromStdString(ev.timestampUtc));
        q.addBindValue(QString::fromStdString(ev.localUtcOffset));
        q.addBindValue(QString::fromStdString(logLevelToString(ev.level)));
        q.addBindValue(QString::fromStdString(ev.category));
        q.addBindValue(QString::fromStdString(ev.message));
        q.addBindValue(QString::fromStdString(ev.detailsJson));

        if (!q.exec()) {
            m_lastError = q.lastError().text().toStdString();
            return false;
        }
        return true;
    }

    int64_t countEvents() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_db.isOpen()) return 0;

        QSqlQuery q(m_db);
        if (q.exec("SELECT COUNT(*) FROM operation_logs") && q.next()) {
            return q.value(0).toLongLong();
        }
        return 0;
    }

    /// Enforce retention policy per Decision D-1:
    /// 180 days or 300,000 events, whichever is reached first.
    int purgeOldEvents(int maxRetentionDays = 180, int64_t maxEvents = 300000) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_db.isOpen()) return 0;

        int purgedCount = 0;

        // 1. Purge events older than maxRetentionDays (180 days)
        QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-maxRetentionDays);
        QString cutoffStr = cutoff.toString(Qt::ISODateWithMs);

        QSqlQuery qDate(m_db);
        qDate.prepare("DELETE FROM operation_logs WHERE timestamp_utc < ?");
        qDate.addBindValue(cutoffStr);
        if (qDate.exec()) {
            purgedCount += qDate.numRowsAffected();
        }

        // 2. Purge events exceeding maxEvents (300,000 limit) from the oldest
        QSqlQuery qCount(m_db);
        if (qCount.exec("SELECT COUNT(*) FROM operation_logs") && qCount.next()) {
            int64_t currentCount = qCount.value(0).toLongLong();
            if (currentCount > maxEvents) {
                int64_t excess = currentCount - maxEvents;
                QSqlQuery qPurge(m_db);
                qPurge.prepare(R"(
                    DELETE FROM operation_logs
                    WHERE id IN (
                        SELECT id FROM operation_logs ORDER BY id ASC LIMIT ?
                    )
                )");
                qPurge.addBindValue(static_cast<qlonglong>(excess));
                if (qPurge.exec()) {
                    purgedCount += qPurge.numRowsAffected();
                }
            }
        }

        return purgedCount;
    }

    /// Export logs to JSONL file
    bool exportToJsonl(const QString& filePath, int limit = 10000) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_db.isOpen()) return false;

        QFile file(filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            m_lastError = file.errorString().toStdString();
            return false;
        }

        QTextStream out(&file);
        QSqlQuery q(m_db);
        q.prepare("SELECT id, timestamp_utc, local_utc_offset, level, category, message, details_json FROM operation_logs ORDER BY id ASC LIMIT ?");
        q.addBindValue(limit);

        if (!q.exec()) {
            m_lastError = q.lastError().text().toStdString();
            return false;
        }

        while (q.next()) {
            int64_t id = q.value(0).toLongLong();
            QString ts = q.value(1).toString();
            QString off = q.value(2).toString();
            QString lvl = q.value(3).toString();
            QString cat = q.value(4).toString();
            QString msg = q.value(5).toString().replace("\"", "\\\"");
            QString json = q.value(6).toString();
            if (json.isEmpty()) json = "{}";

            out << "{\"id\":" << id
                << ",\"timestamp_utc\":\"" << ts << "\""
                << ",\"local_utc_offset\":\"" << off << "\""
                << ",\"level\":\"" << lvl << "\""
                << ",\"category\":\"" << cat << "\""
                << ",\"message\":\"" << msg << "\""
                << ",\"details\":" << json << "}\n";
        }

        file.close();
        return true;
    }

    [[nodiscard]] std::string lastError() const { return m_lastError; }

private:
    QString m_connectionName;
    QSqlDatabase m_db;
    std::mutex m_mutex;
    std::string m_lastError;
};

} // namespace desk32::core::log
