#pragma once

#include "LogEvent.hpp"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <optional>

namespace desk32::core::log {

struct LogFilter {
    std::string fromUtc;
    std::string toUtc;
    std::string category;
    std::string status;
    std::string operationId;
    std::string parentOperationId;
    std::string target;
    std::string searchText;
    int limit = 100;
    int offset = 0;
};

/// Log SQLite WAL Repository
/// Implements full 14-field schema (Section 10.5), D-1 retention policy, and D-3 time recording.
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

        // Verify and migrate schema if legacy table exists
        QSqlQuery checkCols(m_db);
        if (checkCols.exec("PRAGMA table_info(operation_logs);")) {
            bool hasEventId = false;
            bool tableExists = false;
            while (checkCols.next()) {
                tableExists = true;
                if (checkCols.value(1).toString() == "event_id") {
                    hasEventId = true;
                    break;
                }
            }
            if (tableExists && !hasEventId) {
                QSqlQuery drop(m_db);
                drop.exec("DROP TABLE IF EXISTS operation_logs;");
            }
        }

        // Create Section 10.5 production table (all 14 required fields)
        QSqlQuery q(m_db);
        QString createTableSql = R"(
            CREATE TABLE IF NOT EXISTS operation_logs (
                event_id INTEGER PRIMARY KEY AUTOINCREMENT,
                operation_id TEXT NOT NULL,
                parent_operation_id TEXT,
                timestamp_utc TEXT NOT NULL,
                local_utc_offset TEXT NOT NULL,
                category TEXT NOT NULL,
                operation TEXT NOT NULL,
                status TEXT NOT NULL,
                target_type TEXT NOT NULL,
                target TEXT,
                message TEXT NOT NULL,
                error_code INTEGER DEFAULT 0,
                details_json TEXT,
                duration_ms REAL DEFAULT 0.0
            );
        )";
        if (!q.exec(createTableSql)) {
            m_lastError = q.lastError().text().toStdString();
            return false;
        }

        // Create required indices for query performance
        q.exec("CREATE INDEX IF NOT EXISTS idx_logs_timestamp ON operation_logs (timestamp_utc);");
        q.exec("CREATE INDEX IF NOT EXISTS idx_logs_op_id ON operation_logs (operation_id);");
        q.exec("CREATE INDEX IF NOT EXISTS idx_logs_parent_op_id ON operation_logs (parent_operation_id);");
        q.exec("CREATE INDEX IF NOT EXISTS idx_logs_category ON operation_logs (category);");
        q.exec("CREATE INDEX IF NOT EXISTS idx_logs_status ON operation_logs (status);");

        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_db.isOpen()) {
            m_db.close();
        }
    }

    [[nodiscard]] bool isOpen() const {
        return m_db.isOpen();
    }

    /// Insert single log event
    bool insertEvent(const LogEvent& ev) {
        return insertBatch({ev});
    }

    /// Transactional batch insertion (called by LogWorkerThread)
    bool insertBatch(const std::vector<LogEvent>& events) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_db.isOpen() || events.empty()) return false;

        if (!m_db.transaction()) {
            m_lastError = m_db.lastError().text().toStdString();
            return false;
        }

        QSqlQuery q(m_db);
        q.prepare(R"(
            INSERT INTO operation_logs (
                operation_id, parent_operation_id, timestamp_utc, local_utc_offset,
                category, operation, status, target_type, target, message,
                error_code, details_json, duration_ms
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )");

        for (const auto& ev : events) {
            std::string opId = ev.operationId.empty() ? LogEvent::generateOperationId() : ev.operationId;
            std::string st = ev.status;
            if (st.empty()) {
                st = logLevelToString(ev.level);
            }

            q.addBindValue(QString::fromStdString(opId));
            if (ev.parentOperationId.empty()) {
                q.addBindValue(QVariant(QMetaType(QMetaType::QString)));
            } else {
                q.addBindValue(QString::fromStdString(ev.parentOperationId));
            }
            q.addBindValue(QString::fromStdString(ev.timestampUtc));
            q.addBindValue(QString::fromStdString(ev.localUtcOffset));
            q.addBindValue(QString::fromStdString(ev.category));
            q.addBindValue(QString::fromStdString(ev.operation.empty() ? ev.category : ev.operation));
            q.addBindValue(QString::fromStdString(st));
            q.addBindValue(QString::fromStdString(ev.targetType.empty() ? "System" : ev.targetType));
            q.addBindValue(QString::fromStdString(ev.target));
            q.addBindValue(QString::fromStdString(ev.message));
            q.addBindValue(ev.errorCode);
            q.addBindValue(QString::fromStdString(ev.detailsJson.empty() ? "{}" : ev.detailsJson));
            q.addBindValue(ev.durationMs);

            if (!q.exec()) {
                m_lastError = q.lastError().text().toStdString();
                m_db.rollback();
                return false;
            }
        }

        if (!m_db.commit()) {
            m_lastError = m_db.lastError().text().toStdString();
            m_db.rollback();
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

    /// Count distinct root operations (SYS-05)
    int64_t countOperations() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_db.isOpen()) return 0;

        QSqlQuery q(m_db);
        if (q.exec("SELECT COUNT(DISTINCT operation_id) FROM operation_logs WHERE parent_operation_id IS NULL OR parent_operation_id = ''") && q.next()) {
            return q.value(0).toLongLong();
        }
        return 0;
    }

    /// Enforce retention policy per Decision D-1 and SYS-05:
    /// 180 days or 300,000 logical operations, whichever is reached first.
    /// Purges are performed atomically per root operation tree via recursive CTE cascade.
    int purgeOldOperations(int maxRetentionDays = 180, int64_t maxOperations = 300000) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_db.isOpen()) return 0;

        if (!m_db.transaction()) {
            m_lastError = m_db.lastError().text().toStdString();
            return 0;
        }

        std::vector<QString> rootsToPurge;
        qint64 purgedRows = 0;

        // 1. Collect root operations older than maxRetentionDays
        QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-maxRetentionDays);
        QString cutoffStr = cutoff.toString(Qt::ISODateWithMs);

        QSqlQuery qDate(m_db);
        qDate.prepare(R"(
            SELECT operation_id
            FROM operation_logs
            WHERE parent_operation_id IS NULL OR parent_operation_id = ''
            GROUP BY operation_id
            HAVING MIN(timestamp_utc) < ?
        )");
        qDate.addBindValue(cutoffStr);
        if (qDate.exec()) {
            while (qDate.next()) {
                rootsToPurge.push_back(qDate.value(0).toString());
            }
        } else {
            m_lastError = qDate.lastError().text().toStdString();
            m_db.rollback();
            return 0;
        }

        // 2. Collect excess root operations over maxOperations
        QSqlQuery qCount(m_db);
        if (qCount.exec("SELECT COUNT(DISTINCT operation_id) FROM operation_logs WHERE parent_operation_id IS NULL OR parent_operation_id = ''") && qCount.next()) {
            int64_t totalRoots = qCount.value(0).toLongLong();
            if (totalRoots > maxOperations) {
                int64_t excess = totalRoots - maxOperations;
                QSqlQuery qExcess(m_db);
                qExcess.prepare(R"(
                    SELECT operation_id
                    FROM operation_logs
                    WHERE parent_operation_id IS NULL OR parent_operation_id = ''
                    GROUP BY operation_id
                    ORDER BY MIN(timestamp_utc) ASC, MIN(event_id) ASC
                    LIMIT ?
                )");
                qExcess.addBindValue(static_cast<qlonglong>(excess));
                if (qExcess.exec()) {
                    while (qExcess.next()) {
                        QString opId = qExcess.value(0).toString();
                        if (std::find(rootsToPurge.begin(), rootsToPurge.end(), opId) == rootsToPurge.end()) {
                            rootsToPurge.push_back(opId);
                        }
                    }
                } else {
                    m_lastError = qExcess.lastError().text().toStdString();
                    m_db.rollback();
                    return 0;
                }
            }
        } else {
            m_lastError = qCount.lastError().text().toStdString();
            m_db.rollback();
            return 0;
        }

        if (!rootsToPurge.empty()) {
            // 3. Atomically delete all events belonging to the root operations and all child operations via recursive CTE
            QSqlQuery qDelete(m_db);
            qDelete.prepare(R"(
                WITH RECURSIVE op_tree(op) AS (
                    SELECT ? AS op
                    UNION
                    SELECT l.operation_id
                    FROM operation_logs l
                    JOIN op_tree t ON l.parent_operation_id = t.op
                )
                DELETE FROM operation_logs
                WHERE operation_id IN (SELECT op FROM op_tree)
                   OR parent_operation_id IN (SELECT op FROM op_tree)
            )");

            for (const auto& rootOp : rootsToPurge) {
                qDelete.bindValue(0, rootOp);
                if (!qDelete.exec()) {
                    m_lastError = qDelete.lastError().text().toStdString();
                    m_db.rollback();
                    return 0;
                }
                purgedRows += qDelete.numRowsAffected();
            }
        }

        // 4. Purge orphan child events older than retention cutoff whose parent operation is absent
        QSqlQuery qOrphans(m_db);
        qOrphans.prepare(R"(
            DELETE FROM operation_logs
            WHERE parent_operation_id IS NOT NULL
              AND parent_operation_id != ''
              AND timestamp_utc < ?
              AND parent_operation_id NOT IN (
                  SELECT operation_id FROM operation_logs
              )
        )");
        qOrphans.addBindValue(cutoffStr);
        if (!qOrphans.exec()) {
            m_lastError = qOrphans.lastError().text().toStdString();
            m_db.rollback();
            return 0;
        }
        purgedRows += qOrphans.numRowsAffected();

        if (!m_db.commit()) {
            m_lastError = m_db.lastError().text().toStdString();
            m_db.rollback();
            return 0;
        }

        return static_cast<int>(purgedRows);
    }

    /// Enforce retention policy per Decision D-1 and SYS-05.
    /// Operates at logical operation granularity.
    int purgeOldEvents(int maxRetentionDays = 180, int64_t maxEvents = 300000) {
        return purgeOldOperations(maxRetentionDays, maxEvents);
    }

    /// Query logs with filtering, sorting, and pagination
    std::vector<LogEvent> queryEvents(const LogFilter& filter = {}) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<LogEvent> results;
        if (!m_db.isOpen()) return results;

        QString sql = "SELECT event_id, operation_id, parent_operation_id, timestamp_utc, local_utc_offset, "
                      "category, operation, status, target_type, target, message, error_code, details_json, duration_ms "
                      "FROM operation_logs WHERE 1=1";
        QList<QVariant> binds;
        appendFilterConditions(filter, sql, binds);

        sql += " ORDER BY event_id DESC";
        if (filter.limit > 0) {
            sql += " LIMIT ?";
            binds.append(filter.limit);
            if (filter.offset > 0) {
                sql += " OFFSET ?";
                binds.append(filter.offset);
            }
        }

        QSqlQuery q(m_db);
        q.prepare(sql);
        for (const auto& b : binds) {
            q.addBindValue(b);
        }

        if (q.exec()) {
            while (q.next()) {
                results.push_back(readEventFromQuery(q));
            }
        } else {
            m_lastError = q.lastError().text().toStdString();
        }

        return results;
    }

    /// Query parent operation and all its child operations (Section 10.5 hierarchical tracking)
    std::vector<LogEvent> queryRelatedEvents(const std::string& operationId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<LogEvent> results;
        if (!m_db.isOpen() || operationId.empty()) return results;

        QSqlQuery q(m_db);
        q.prepare(R"(
            SELECT event_id, operation_id, parent_operation_id, timestamp_utc, local_utc_offset,
                   category, operation, status, target_type, target, message, error_code, details_json, duration_ms
            FROM operation_logs
            WHERE operation_id = ? OR parent_operation_id = ?
            ORDER BY event_id ASC
        )");
        q.addBindValue(QString::fromStdString(operationId));
        q.addBindValue(QString::fromStdString(operationId));

        if (q.exec()) {
            while (q.next()) {
                results.push_back(readEventFromQuery(q));
            }
        }
        return results;
    }

    /// Query chronological playback & load history from operation_logs (Spec v3.8.1 §9.5 / §10.5)
    std::vector<LogEvent> queryPlaybackHistory(int limit = 200) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<LogEvent> results;
        if (!m_db.isOpen()) return results;

        QSqlQuery q(m_db);
        q.prepare(R"(
            SELECT event_id, operation_id, parent_operation_id, timestamp_utc, local_utc_offset,
                   category, operation, status, target_type, target, message, error_code, details_json, duration_ms
            FROM operation_logs
            WHERE status = 'Success'
              AND (
                    (category = 'Transport' AND operation IN ('Play', 'AutoAdvance'))
                 OR (category = 'Deck' AND operation = 'LoadTrack')
                 OR (category = 'FolderSession' AND operation IN ('StartSession', 'AutoAdvance'))
              )
            ORDER BY event_id DESC
            LIMIT ?
        )");
        q.addBindValue(std::clamp(limit, 1, 2000));

        if (q.exec()) {
            while (q.next()) {
                results.push_back(readEventFromQuery(q));
            }
        } else {
            m_lastError = q.lastError().text().toStdString();
        }
        return results;
    }

    /// Export logs to JSONL file (Section 10.5: 1 line per event, preserving structured information)
    bool exportToJsonl(const QString& filePath, const LogFilter& filter = {}, int limit = 10000) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_db.isOpen()) return false;

        QFile file(filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            m_lastError = file.errorString().toStdString();
            return false;
        }

        QString sql = "SELECT event_id, operation_id, parent_operation_id, timestamp_utc, local_utc_offset, "
                      "category, operation, status, target_type, target, message, error_code, details_json, duration_ms "
                      "FROM operation_logs WHERE 1=1";
        QList<QVariant> binds;
        appendFilterConditions(filter, sql, binds);

        const int effectiveLimit = (filter.limit > 0 && filter.limit < limit) ? filter.limit : limit;
        sql += " ORDER BY event_id ASC LIMIT ?";
        binds.append(effectiveLimit);
        if (filter.offset > 0) {
            sql += " OFFSET ?";
            binds.append(filter.offset);
        }

        QSqlQuery q(m_db);
        q.prepare(sql);
        for (const auto& b : binds) {
            q.addBindValue(b);
        }

        if (!q.exec()) {
            m_lastError = q.lastError().text().toStdString();
            return false;
        }

        while (q.next()) {
            const qint64 eid = q.value(0).toLongLong();
            const QString opId = q.value(1).toString();
            const bool hasParentOp = !q.value(2).isNull() && !q.value(2).toString().isEmpty();
            const QString pOpId = q.value(2).toString();
            const QString ts = q.value(3).toString();
            const QString off = q.value(4).toString();
            const QString cat = q.value(5).toString();
            const QString op = q.value(6).toString();
            const QString status = q.value(7).toString();
            const QString level = (status == QStringLiteral("Failed")) ? QStringLiteral("Error") : status;
            const QString tgtType = q.value(8).toString();
            const QString tgt = q.value(9).isNull() ? QString() : q.value(9).toString();
            const QString msg = q.value(10).toString();
            const int errCode = q.value(11).toInt();
            const QString rawDetails = q.value(12).toString();
            const double durMs = q.value(13).toDouble();

            QJsonValue detailsVal = QJsonObject();
            if (!rawDetails.isEmpty()) {
                QJsonParseError parseErr{};
                const QJsonDocument detailsDoc = QJsonDocument::fromJson(rawDetails.toUtf8(), &parseErr);
                if (parseErr.error == QJsonParseError::NoError && !detailsDoc.isNull()) {
                    detailsVal = detailsDoc.isObject() ? QJsonValue(detailsDoc.object())
                                                       : QJsonValue(detailsDoc.array());
                } else {
                    detailsVal = QJsonValue(rawDetails);
                }
            }

            QJsonObject rowObj;
            rowObj.insert(QStringLiteral("event_id"), eid);
            rowObj.insert(QStringLiteral("id"), eid);
            rowObj.insert(QStringLiteral("operation_id"), opId);
            rowObj.insert(QStringLiteral("parent_operation_id"), hasParentOp ? QJsonValue(pOpId) : QJsonValue(QJsonValue::Null));
            rowObj.insert(QStringLiteral("timestamp_utc"), ts);
            rowObj.insert(QStringLiteral("local_utc_offset"), off);
            rowObj.insert(QStringLiteral("category"), cat);
            rowObj.insert(QStringLiteral("operation"), op);
            rowObj.insert(QStringLiteral("status"), status);
            rowObj.insert(QStringLiteral("level"), level);
            rowObj.insert(QStringLiteral("target_type"), tgtType);
            rowObj.insert(QStringLiteral("target"), tgt);
            rowObj.insert(QStringLiteral("message"), msg);
            rowObj.insert(QStringLiteral("error_code"), errCode);
            rowObj.insert(QStringLiteral("details"), detailsVal);
            rowObj.insert(QStringLiteral("details_json"), detailsVal);
            rowObj.insert(QStringLiteral("duration_ms"), durMs);

            QByteArray line = QJsonDocument(rowObj).toJson(QJsonDocument::Compact);
            line.append('\n');
            if (file.write(line) != line.size()) {
                m_lastError = file.errorString().toStdString();
                file.close();
                return false;
            }
        }

        file.close();
        return true;
    }

    [[nodiscard]] std::string lastError() const { return m_lastError; }

private:
    static void appendFilterConditions(const LogFilter& filter, QString& sql, QList<QVariant>& binds) {
        if (!filter.fromUtc.empty()) {
            sql += " AND timestamp_utc >= ?";
            binds.append(QString::fromStdString(filter.fromUtc));
        }
        if (!filter.toUtc.empty()) {
            sql += " AND timestamp_utc <= ?";
            binds.append(QString::fromStdString(filter.toUtc));
        }
        if (!filter.category.empty()) {
            sql += " AND category = ?";
            binds.append(QString::fromStdString(filter.category));
        }
        if (!filter.status.empty()) {
            sql += " AND status = ?";
            binds.append(QString::fromStdString(filter.status));
        }
        if (!filter.operationId.empty()) {
            sql += " AND operation_id = ?";
            binds.append(QString::fromStdString(filter.operationId));
        }
        if (!filter.parentOperationId.empty()) {
            sql += " AND parent_operation_id = ?";
            binds.append(QString::fromStdString(filter.parentOperationId));
        }
        if (!filter.target.empty()) {
            sql += " AND target LIKE ?";
            binds.append("%" + QString::fromStdString(filter.target) + "%");
        }
        if (!filter.searchText.empty()) {
            sql += " AND (message LIKE ? OR details_json LIKE ? OR target LIKE ? OR category LIKE ? OR CAST(error_code AS TEXT) LIKE ? OR operation_id LIKE ?)";
            const QString pat = "%" + QString::fromStdString(filter.searchText) + "%";
            for (int i = 0; i < 6; ++i) {
                binds.append(pat);
            }
        }
    }
    static LogEvent readEventFromQuery(const QSqlQuery& q) {
        LogEvent ev;
        ev.eventId = q.value(0).toLongLong();
        ev.id = ev.eventId;
        ev.operationId = q.value(1).toString().toStdString();
        ev.parentOperationId = q.value(2).isNull() ? "" : q.value(2).toString().toStdString();
        ev.timestampUtc = q.value(3).toString().toStdString();
        ev.localUtcOffset = q.value(4).toString().toStdString();
        ev.category = q.value(5).toString().toStdString();
        ev.operation = q.value(6).toString().toStdString();
        ev.status = q.value(7).toString().toStdString();
        ev.level = stringToLogLevel(ev.status);
        ev.targetType = q.value(8).toString().toStdString();
        ev.target = q.value(9).isNull() ? "" : q.value(9).toString().toStdString();
        ev.message = q.value(10).toString().toStdString();
        ev.errorCode = q.value(11).toInt();
        ev.detailsJson = q.value(12).toString().toStdString();
        ev.durationMs = q.value(13).toDouble();
        return ev;
    }

    QString m_connectionName;
    QSqlDatabase m_db;
    std::mutex m_mutex;
    std::string m_lastError;
};

} // namespace desk32::core::log
