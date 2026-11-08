#include "RenderSequenceManager.hpp"
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QThread>
#include <algorithm>

namespace desk32::engine::render {

RenderSequenceManager& RenderSequenceManager::instance() {
    static RenderSequenceManager s_instance("Desk32RenderSequenceMainConn");
    return s_instance;
}

RenderSequenceManager::RenderSequenceManager(const QString& connectionName)
    : m_connectionName(connectionName)
{
    if (m_connectionName.isEmpty()) {
        m_connectionName = QString("Desk32RenderSeqConn_%1")
                               .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    }
}

RenderSequenceManager::~RenderSequenceManager() {
    close();
}

void RenderSequenceManager::close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    closeLocked();
}

void RenderSequenceManager::closeLocked() {
    if (m_db.isOpen()) {
        m_db.close();
    }
    m_db = QSqlDatabase();
    if (QSqlDatabase::contains(m_connectionName)) {
        QSqlDatabase::removeDatabase(m_connectionName);
    }
    m_initialized = false;
}

bool RenderSequenceManager::isInitialized() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_initialized && m_db.isOpen();
}

bool RenderSequenceManager::initialize(const QString& dbPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return initializeLocked(dbPath);
}

bool RenderSequenceManager::initializeLocked(const QString& dbPath) {
    if (m_initialized && m_db.isOpen()) {
        return true;
    }

    QString actualPath = dbPath.isEmpty() ? m_dbPath : dbPath;
    if (actualPath.isEmpty()) {
        QString stateDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (stateDir.isEmpty() || !QDir().mkpath(stateDir)) {
            stateDir = QDir::tempPath() + "/desk32";
            QDir().mkpath(stateDir);
        }
        actualPath = stateDir + "/operation_logs.db";
    }
    m_dbPath = actualPath;

    if (QSqlDatabase::contains(m_connectionName)) {
        m_db = QSqlDatabase::database(m_connectionName);
    } else {
        m_db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    }

    m_db.setDatabaseName(actualPath);
    if (!m_db.open()) {
        qWarning() << "RenderSequenceManager: Failed to open SQLite DB at" << actualPath << m_db.lastError().text();
        m_initialized = false;
        return false;
    }

    QSqlQuery q(m_db);
    q.exec("PRAGMA journal_mode = WAL;");
    q.exec("PRAGMA synchronous = NORMAL;");
    q.exec("PRAGMA busy_timeout = 5000;");

    QString createSql = R"(
        CREATE TABLE IF NOT EXISTS render_sequences (
            folder_path TEXT PRIMARY KEY,
            next_sequence INTEGER NOT NULL DEFAULT 0
        );
    )";

    if (!q.exec(createSql)) {
        qWarning() << "RenderSequenceManager: Failed to create render_sequences table" << q.lastError().text();
        m_db.close();
        m_initialized = false;
        return false;
    }

    m_initialized = true;
    return true;
}

std::string RenderSequenceManager::indexToSerial(uint64_t index) {
    // Bijective base-26 conversion (0 -> A, 25 -> Z, 26 -> AA, 27 -> AB, 52 -> BA, 701 -> ZZ, 702 -> AAA)
    std::string result;
    uint64_t n = index + 1; // 1-based index
    while (n > 0) {
        uint64_t rem = (n - 1) % 26;
        result.push_back(static_cast<char>('A' + rem));
        n = (n - 1) / 26;
    }
    std::reverse(result.begin(), result.end());
    return result;
}

QString RenderSequenceManager::formatFolderName(const QString& serial, const QDateTime& localDateTime) {
    // Decision D-3: Output folder name uses local date/time YYYYMMDD_HHMMSS_zzz
    return QString("%1%2").arg(serial, localDateTime.toString("yyyyMMdd_hhmmss_zzz"));
}

QString RenderSequenceManager::allocateNextSerial(const QString& sourceFolder) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        if (!initializeLocked(m_dbPath)) {
            // Fail safely: DO NOT return "A" or fallback!
            return QString();
        }
    }

    return allocateNextSerialLocked(sourceFolder);
}

QString RenderSequenceManager::allocateNextSerialLocked(const QString& sourceFolder) {
    if (!m_db.isOpen()) {
        return QString();
    }

    // Canonical clean path
    QString cleanPath = QFileInfo(sourceFolder).canonicalFilePath();
    if (cleanPath.isEmpty()) {
        cleanPath = sourceFolder;
    }

    // Retry loop on SQLite contention across concurrent threads/workers
    for (int attempt = 0; attempt < 50; ++attempt) {
        uint64_t currentSeq = 0;
        bool inTx = m_db.transaction();
        if (!inTx && attempt < 49) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        QSqlQuery selectQuery(m_db);
        selectQuery.prepare("SELECT next_sequence FROM render_sequences WHERE folder_path = :path;");
        selectQuery.bindValue(":path", cleanPath);

        if (!selectQuery.exec()) {
            if (inTx) m_db.rollback();
            if (attempt < 49) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            return QString();
        }

        if (selectQuery.next()) {
            currentSeq = selectQuery.value(0).toULongLong();
            QSqlQuery updateQuery(m_db);
            updateQuery.prepare("UPDATE render_sequences SET next_sequence = :next WHERE folder_path = :path;");
            updateQuery.bindValue(":next", QVariant::fromValue(currentSeq + 1));
            updateQuery.bindValue(":path", cleanPath);
            if (!updateQuery.exec()) {
                if (inTx) m_db.rollback();
                if (attempt < 49) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                return QString();
            }
        } else {
            currentSeq = 0;
            QSqlQuery insertQuery(m_db);
            insertQuery.prepare("INSERT INTO render_sequences (folder_path, next_sequence) VALUES (:path, 1);");
            insertQuery.bindValue(":path", cleanPath);
            if (!insertQuery.exec()) {
                if (inTx) m_db.rollback();
                if (attempt < 49) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                return QString();
            }
        }

        if (inTx) {
            if (!m_db.commit()) {
                m_db.rollback();
                if (attempt < 49) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                return QString();
            }
        }

        return QString::fromStdString(indexToSerial(currentSeq));
    }

    return QString();
}

} // namespace desk32::engine::render

