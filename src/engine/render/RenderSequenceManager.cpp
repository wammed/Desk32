#include "RenderSequenceManager.hpp"
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <algorithm>

namespace desk32::engine::render {

RenderSequenceManager& RenderSequenceManager::instance() {
    static RenderSequenceManager s_instance;
    return s_instance;
}

RenderSequenceManager::RenderSequenceManager(const QString& connectionName)
    : m_connectionName(connectionName)
{}

RenderSequenceManager::~RenderSequenceManager() {
    close();
}

void RenderSequenceManager::close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_db.isOpen()) {
        m_db.close();
    }
    m_initialized = false;
}

bool RenderSequenceManager::initialize(const QString& dbPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized && m_db.isOpen()) {
        return true;
    }

    QString actualPath = dbPath;
    if (actualPath.isEmpty()) {
        QString stateDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (stateDir.isEmpty()) {
            stateDir = QDir::homePath() + "/.local/share/desk32";
        }
        QDir().mkpath(stateDir);
        actualPath = stateDir + "/operation_logs.db";
    }

    if (QSqlDatabase::contains(m_connectionName)) {
        m_db = QSqlDatabase::database(m_connectionName);
    } else {
        m_db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    }

    m_db.setDatabaseName(actualPath);
    if (!m_db.open()) {
        qWarning() << "RenderSequenceManager: Failed to open SQLite DB at" << actualPath << m_db.lastError().text();
        return false;
    }

    QSqlQuery q(m_db);
    q.exec("PRAGMA journal_mode = WAL;");
    q.exec("PRAGMA synchronous = NORMAL;");

    QString createSql = R"(
        CREATE TABLE IF NOT EXISTS render_sequences (
            folder_path TEXT PRIMARY KEY,
            next_sequence INTEGER NOT NULL DEFAULT 0
        );
    )";

    if (!q.exec(createSql)) {
        qWarning() << "RenderSequenceManager: Failed to create render_sequences table" << q.lastError().text();
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
    if (!m_initialized && !initialize()) {
        // Fallback in-memory serial if DB unavailable
        return "A";
    }

    // Canonical clean path
    QString cleanPath = QFileInfo(sourceFolder).canonicalFilePath();
    if (cleanPath.isEmpty()) {
        cleanPath = sourceFolder;
    }

    uint64_t currentSeq = 0;
    bool inTx = m_db.transaction();

    QSqlQuery selectQuery(m_db);
    selectQuery.prepare("SELECT next_sequence FROM render_sequences WHERE folder_path = :path;");
    selectQuery.bindValue(":path", cleanPath);

    if (selectQuery.exec() && selectQuery.next()) {
        currentSeq = selectQuery.value(0).toULongLong();
        QSqlQuery updateQuery(m_db);
        updateQuery.prepare("UPDATE render_sequences SET next_sequence = :next WHERE folder_path = :path;");
        updateQuery.bindValue(":next", QVariant::fromValue(currentSeq + 1));
        updateQuery.bindValue(":path", cleanPath);
        updateQuery.exec();
    } else {
        currentSeq = 0;
        QSqlQuery insertQuery(m_db);
        insertQuery.prepare("INSERT INTO render_sequences (folder_path, next_sequence) VALUES (:path, 1);");
        insertQuery.bindValue(":path", cleanPath);
        insertQuery.exec();
    }

    if (inTx) {
        m_db.commit();
    }

    return QString::fromStdString(indexToSerial(currentSeq));
}

} // namespace desk32::engine::render

