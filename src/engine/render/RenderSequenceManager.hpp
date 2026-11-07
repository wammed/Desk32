#pragma once

#include <QString>
#include <QSqlDatabase>
#include <QDateTime>
#include <string>
#include <mutex>

namespace desk32::engine::render {

/// Persistent sequential uppercase letter folder naming manager (Decision D-2).
/// Manages base-26 bijective sequences (A..Z, AA..AZ, BA..BZ, AAA..) per source directory.
/// Issued sequence numbers are permanently tracked in SQLite and NEVER reused.
class RenderSequenceManager {
public:
    static RenderSequenceManager& instance();

    explicit RenderSequenceManager(const QString& connectionName = "Desk32RenderSequenceConn");
    ~RenderSequenceManager();

    RenderSequenceManager(const RenderSequenceManager&) = delete;
    RenderSequenceManager& operator=(const RenderSequenceManager&) = delete;

    /// Open/initialize persistent database for sequence management
    bool initialize(const QString& dbPath = QString());

    /// Allocate next strictly non-reusable uppercase serial letter for the given source folder.
    /// Thread-safe and persistent in SQLite.
    QString allocateNextSerial(const QString& sourceFolder);

    /// Format standard output folder name: {Serial}{YYYYMMDD_HHMMSS_zzz} in local time (Decision D-3)
    static QString formatFolderName(const QString& serial, const QDateTime& localDateTime = QDateTime::currentDateTime());

    /// Mathematical Bijective Base-26 converter (0 -> "A", 25 -> "Z", 26 -> "AA", 27 -> "AB"...)
    static std::string indexToSerial(uint64_t index);

    /// Close database
    void close();

private:
    QString m_connectionName;
    QSqlDatabase m_db;
    mutable std::mutex m_mutex;
    bool m_initialized = false;
};

} // namespace desk32::engine::render

