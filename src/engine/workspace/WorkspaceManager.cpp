#include "WorkspaceManager.hpp"
#include "core/log/LogErrorCodes.hpp"
#include "core/log/LogService.hpp"
#include "engine/decoder/FFmpegAudioDecoder.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>
#include <QTimeZone>
#include <QUuid>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace desk32::engine::workspace {

namespace {

QString localIsoWithOffsetNow() {
    return QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
}

QString utcIsoNow() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString localOffsetStringNow() {
    const QDateTime now = QDateTime::currentDateTime();
    const int secs = now.offsetFromUtc();
    const char sign = (secs >= 0) ? '+' : '-';
    const int absSecs = std::abs(secs);
    const int hrs = absSecs / 3600;
    const int mins = (absSecs % 3600) / 60;
    return QString("%1%2:%3")
        .arg(sign)
        .arg(hrs, 2, 10, QChar('0'))
        .arg(mins, 2, 10, QChar('0'));
}

bool isInternalDesk32EntryName(const QString& name) {
    return name == QStringLiteral(".desk32") ||
           name.startsWith(QStringLiteral(".desk32_staging_"));
}

bool isSupportedAudioFileName(const QString& fileName) {
    const QString low = fileName.toLower();
    return low.endsWith(QStringLiteral(".flac")) ||
           low.endsWith(QStringLiteral(".wav")) ||
           low.endsWith(QStringLiteral(".mp3")) ||
           low.endsWith(QStringLiteral(".ogg")) ||
           low.endsWith(QStringLiteral(".opus")) ||
           low.endsWith(QStringLiteral(".m4a")) ||
           low.endsWith(QStringLiteral(".aac")) ||
           low.endsWith(QStringLiteral(".aiff")) ||
           low.endsWith(QStringLiteral(".aif"));
}

QVariantMap metadataToVariantMap(const metadata::TrackMetadata& m) {
    QVariantMap v;
    v["title"] = QString::fromStdString(m.title);
    v["artist"] = QString::fromStdString(m.artist);
    v["album"] = QString::fromStdString(m.album);
    v["albumArtist"] = QString::fromStdString(m.albumArtist);
    v["trackNumber"] = m.trackNumber;
    v["totalTracks"] = m.totalTracks;
    v["discNumber"] = m.discNumber;
    v["totalDiscs"] = m.totalDiscs;
    v["dateOrYear"] = QString::fromStdString(m.dateOrYear);
    v["genre"] = QString::fromStdString(m.genre);
    v["composer"] = QString::fromStdString(m.composer);
    v["conductor"] = QString::fromStdString(m.conductor);
    v["performer"] = QString::fromStdString(m.performer);
    v["arranger"] = QString::fromStdString(m.arranger);
    v["label"] = QString::fromStdString(m.label);
    v["publisher"] = QString::fromStdString(m.publisher);
    v["catalogNumber"] = QString::fromStdString(m.catalogNumber);
    v["isrc"] = QString::fromStdString(m.isrc);
    v["comment"] = QString::fromStdString(m.comment);

    v["sampleRate"] = m.sampleRate;
    v["channels"] = static_cast<int>(m.channels);
    v["bitDepth"] = static_cast<int>(m.bitDepth);
    v["durationSeconds"] = m.durationSeconds;
    v["codecName"] = QString::fromStdString(m.codecName);

    v["hasProvenance"] = m.hasProvenance;
    v["provenanceValid"] = m.provenanceValid;
    v["provenanceCorrupt"] = m.provenanceCorrupt;
    v["provenanceEmbedSupported"] = m.provenanceEmbedSupported;
    v["provenanceOriginalPath"] = QString::fromStdString(m.provenanceOriginalPath);
    v["provenanceCopyTimestamp"] = QString::fromStdString(m.provenanceCopyTimestamp);
    v["provenanceOriginalSha256"] = QString::fromStdString(m.provenanceOriginalSha256);
    v["provenanceMarker"] = QString::fromStdString(m.provenanceMarker);

    QString provSummary;
    if (!m.provenanceEmbedSupported) {
        provSummary = QStringLiteral("Log only (format cannot embed metadata)");
    } else if (!m.hasProvenance) {
        provSummary = QStringLiteral("No provenance (external placement)");
    } else if (m.provenanceValid) {
        provSummary = QStringLiteral("Verified provenance");
    } else {
        provSummary = QStringLiteral("Corrupt/incomplete provenance");
    }
    v["provenanceStatusText"] = provSummary;
    return v;
}

bool removeEmptyParentDirsUpTo(const QString& rootCanonical, const QString& startDir) {
    QDir root(rootCanonical);
    QDir cur(startDir);
    while (cur.exists() && QDir::cleanPath(cur.absolutePath()) != QDir::cleanPath(root.absolutePath())) {
        if (!QDir::cleanPath(cur.absolutePath()).startsWith(QDir::cleanPath(root.absolutePath()) + "/")) {
            break;
        }
        const QStringList entries = cur.entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System);
        if (!entries.isEmpty()) {
            break;
        }
        const QString dirName = cur.dirName();
        if (!cur.cdUp()) break;
        if (!cur.rmdir(dirName)) break;
    }
    return true;
}

} // namespace

WorkspaceManager& WorkspaceManager::instance() {
    static WorkspaceManager s_instance;
    return s_instance;
}

WorkspaceManager::WorkspaceManager(QObject* parent)
    : QObject(parent)
{
    m_watcher = std::make_unique<QFileSystemWatcher>(this);
    connect(m_watcher.get(), &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        Q_UNUSED(path);
        const QStringList msgs = verifyWorkspaceIntegrity();
        Q_UNUSED(msgs);
    });
    connect(m_watcher.get(), &QFileSystemWatcher::directoryChanged, this, [this](const QString& path) {
        Q_UNUSED(path);
        const QStringList msgs = verifyWorkspaceIntegrity();
        Q_UNUSED(msgs);
    });
    startWorkerThread();
}

WorkspaceManager::~WorkspaceManager() {
    closeWorkspaceForShutdown();
    stopWorkerThread();
}

void WorkspaceManager::startWorkerThread() {
    std::lock_guard<std::mutex> lock(m_workerQueueMutex);
    if (m_workerRunning) return;
    m_workerRunning = true;
    m_workerThread = std::thread([this]() {
        {
            std::lock_guard<std::mutex> qLock(m_workerQueueMutex);
            m_workerThreadId = std::this_thread::get_id();
        }
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> qLock(m_workerQueueMutex);
                m_workerCv.wait(qLock, [this]() {
                    return !m_workerRunning || !m_workerQueue.empty();
                });
                if (!m_workerRunning && m_workerQueue.empty()) {
                    break;
                }
                job = std::move(m_workerQueue.front());
                m_workerQueue.pop();
            }
            if (job) {
                job();
            }
        }
    });
}

void WorkspaceManager::stopWorkerThread() {
    {
        std::lock_guard<std::mutex> lock(m_workerQueueMutex);
        if (!m_workerRunning) return;
        m_workerRunning = false;
    }
    m_workerCv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    std::lock_guard<std::mutex> lock(m_workerQueueMutex);
    m_workerThreadId = std::thread::id{};
}

void WorkspaceManager::setConfigFilePathForTesting(const QString& path) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_customConfigPath = path;
}

QString WorkspaceManager::configFilePath() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!m_customConfigPath.isEmpty()) {
        return m_customConfigPath;
    }
    QString cfgDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (cfgDir.isEmpty()) {
        cfgDir = QDir::homePath() + "/.config";
    }
    return cfgDir + "/desk32/workspaces.json";
}

void WorkspaceManager::resetForTesting() {
    closeWorkspaceForShutdown();
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_selectionFinalized = false;
        m_readOnlyMode = true;
        m_openWorkspacePath.clear();
        m_lastError.clear();
        m_externalModificationCount = 0;
        m_externalChangeDetails.clear();
        m_faultCallback = nullptr;
        m_stopPlaybackCallback = nullptr;
        m_metadataReloadCallback = nullptr;
        if (!m_customConfigPath.isEmpty()) {
            QFile::remove(m_customConfigPath);
        }
    }
    emit workspaceStateChanged();
}

void WorkspaceManager::simulateAppRestartForTesting() {
    closeWorkspaceForShutdown();
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_selectionFinalized = false;
        m_readOnlyMode = true;
        m_openWorkspacePath.clear();
        m_lastError.clear();
        m_externalModificationCount = 0;
        m_externalChangeDetails.clear();
        m_logOnlyProvenanceRelPaths.clear();
    }
    emit workspaceStateChanged();
}

void WorkspaceManager::setTestFaultCallback(FaultCallback cb) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_faultCallback = std::move(cb);
}

void WorkspaceManager::setStopPlaybackCallback(StopPlaybackCallback cb) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_stopPlaybackCallback = std::move(cb);
}

void WorkspaceManager::setMetadataReloadCallback(MetadataReloadCallback cb) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_metadataReloadCallback = std::move(cb);
}

void WorkspaceManager::refreshIndexAfterInternalWrite() {
    if (!hasOpenWorkspace()) return;
    runOnWorkerThread([this]() {
        std::vector<WorkspaceFileEntry> current;
        scanCurrentWorkspaceLocked(current, false);
        saveStateIndexLocked(current);
        refreshWatcherLocked();
    });
    emit workspaceContentsChanged();
}

void WorkspaceManager::acknowledgeExternalModifications() {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_externalChangeDetails.clear();
        m_externalModificationCount = 0;
    }
    emit workspaceContentsChanged();
}

bool WorkspaceManager::triggerFaultIfSet(WorkspaceFaultPoint point, const QString& detailPath) const {
    FaultCallback cbCopy;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        cbCopy = m_faultCallback;
    }
    if (cbCopy) {
        return cbCopy(point, detailPath);
    }
    return false;
}

void WorkspaceManager::setLastError(const QString& err) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_lastError = err;
}

QString WorkspaceManager::lastError() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_lastError;
}

bool WorkspaceManager::hasOpenWorkspace() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return !m_readOnlyMode && !m_openWorkspacePath.isEmpty() && m_lockFd >= 0;
}

bool WorkspaceManager::isReadOnlyMode() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_readOnlyMode || m_openWorkspacePath.isEmpty();
}

bool WorkspaceManager::isSelectionFinalized() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_selectionFinalized;
}

QString WorkspaceManager::openWorkspacePath() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_openWorkspacePath;
}

int WorkspaceManager::externalModificationCount() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_externalModificationCount;
}

QString WorkspaceManager::resolveRealPath(const QString& path) {
    if (path.isEmpty()) return QString();
    const QByteArray raw = QFile::encodeName(QDir::cleanPath(path));
    char* resolved = ::realpath(raw.constData(), nullptr);
    if (resolved) {
        QString out = QFile::decodeName(resolved);
        std::free(resolved);
        return QDir::cleanPath(out);
    }
    return QString();
}

bool WorkspaceManager::isSameOrSubpath(const QString& candidate, const QString& base) {
    if (candidate.isEmpty() || base.isEmpty()) return false;
    const QString c = QDir::cleanPath(candidate);
    const QString b = QDir::cleanPath(base);
    if (c == b) return true;
    if (b == QStringLiteral("/")) return c.startsWith('/');
    return c.startsWith(b + QStringLiteral("/"));
}

bool WorkspaceManager::computeFileSha256(const QString& absPath, QString& outHex, qint64* outSize, qint64* outMtimeNs) {
    outHex.clear();
    const QByteArray encoded = QFile::encodeName(absPath);
    struct stat st{};
    if (::lstat(encoded.constData(), &st) != 0) {
        return false;
    }
    if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) {
        return false;
    }

    const int fd = ::open(encoded.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    struct stat fst{};
    if (::fstat(fd, &fst) != 0 || !S_ISREG(fst.st_mode)) {
        ::close(fd);
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, 65536> buf{};
    qint64 totalRead = 0;
    while (true) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n > 0) {
            hash.addData(QByteArrayView(buf.data(), static_cast<qsizetype>(n)));
            totalRead += static_cast<qint64>(n);
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
    }
    ::close(fd);

    if (outSize) *outSize = totalRead;
    if (outMtimeNs) {
        *outMtimeNs = static_cast<qint64>(fst.st_mtim.tv_sec) * 1000000000LL +
                      static_cast<qint64>(fst.st_mtim.tv_nsec);
    }
    outHex = QString::fromLatin1(hash.result().toHex());
    return true;
}

bool WorkspaceManager::checkRegularNoHardlink(const QString& absPath, QString& outError, qint64* outSize, qint64* outMtimeNs) {
    const QByteArray encoded = QFile::encodeName(absPath);
    struct stat st{};
    if (::lstat(encoded.constData(), &st) != 0) {
        outError = QString("File does not exist or cannot be stated: %1 (%2)").arg(absPath, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        outError = QString("Symbolic links are excluded from workspace editing: %1").arg(absPath);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        outError = QString("Target is not a regular file: %1").arg(absPath);
        return false;
    }
    if (st.st_nlink >= 2) {
        outError = QString("Hard-linked file (link count=%1 >= 2) refused for content modification: %2").arg(st.st_nlink).arg(absPath);
        return false;
    }
    if (outSize) *outSize = static_cast<qint64>(st.st_size);
    if (outMtimeNs) {
        *outMtimeNs = static_cast<qint64>(st.st_mtim.tv_sec) * 1000000000LL +
                      static_cast<qint64>(st.st_mtim.tv_nsec);
    }
    return true;
}

bool WorkspaceManager::copyFileReadOnlyVerified(const QString& srcAbsPath,
                                              const QString& dstAbsPath,
                                              const QString& expectedSha256,
                                              QString& outActualSha256,
                                              QString& outError)
{
    outActualSha256.clear();
    outError.clear();

    const QByteArray srcEnc = QFile::encodeName(srcAbsPath);
    struct stat srcSt{};
    if (::lstat(srcEnc.constData(), &srcSt) != 0) {
        outError = QString("Cannot stat source file: %1").arg(srcAbsPath);
        return false;
    }
    if (S_ISLNK(srcSt.st_mode) || !S_ISREG(srcSt.st_mode)) {
        outError = QString("Source is not a regular file (symlinks rejected): %1").arg(srcAbsPath);
        return false;
    }

    QStorageInfo storage(QFileInfo(dstAbsPath).absolutePath());
    if (storage.isValid() && storage.bytesAvailable() >= 0 &&
        storage.bytesAvailable() < static_cast<qint64>(srcSt.st_size) + 4096) {
        outError = QString("Insufficient disk space to copy '%1' (need %2 bytes, available %3 bytes)")
                       .arg(srcAbsPath)
                       .arg(srcSt.st_size)
                       .arg(storage.bytesAvailable());
        return false;
    }

    const int srcFd = ::open(srcEnc.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (srcFd < 0) {
        outError = QString("Failed to open source read-only: %1 (%2)").arg(srcAbsPath, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }

    const QByteArray dstEnc = QFile::encodeName(dstAbsPath);
    const int dstFd = ::open(dstEnc.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (dstFd < 0) {
        ::close(srcFd);
        outError = QString("Failed to create temp destination file: %1 (%2)").arg(dstAbsPath, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }

    QCryptographicHash writeHash(QCryptographicHash::Sha256);
    std::array<char, 65536> buf{};
    bool ioOk = true;
    while (true) {
        const ssize_t n = ::read(srcFd, buf.data(), buf.size());
        if (n > 0) {
            writeHash.addData(QByteArrayView(buf.data(), static_cast<qsizetype>(n)));
            ssize_t writtenTotal = 0;
            while (writtenTotal < n) {
                const ssize_t w = ::write(dstFd, buf.data() + writtenTotal, static_cast<size_t>(n - writtenTotal));
                if (w < 0) {
                    if (errno == EINTR) continue;
                    outError = QString("Write failure during verified copy to '%1': %2")
                                   .arg(dstAbsPath, QString::fromLocal8Bit(std::strerror(errno)));
                    ioOk = false;
                    break;
                }
                writtenTotal += w;
            }
            if (!ioOk) break;
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            outError = QString("Read failure during verified copy from '%1': %2")
                           .arg(srcAbsPath, QString::fromLocal8Bit(std::strerror(errno)));
            ioOk = false;
            break;
        }
    }

    if (ioOk && ::fsync(dstFd) != 0) {
        outError = QString("fsync failed on destination '%1': %2").arg(dstAbsPath, QString::fromLocal8Bit(std::strerror(errno)));
        ioOk = false;
    }

    ::close(srcFd);
    ::close(dstFd);

    if (!ioOk) {
        QFile::remove(dstAbsPath);
        return false;
    }

    // Re-read written file and recompute SHA-256 independently
    QString readBackSha;
    if (!computeFileSha256(dstAbsPath, readBackSha)) {
        outError = QString("Failed to re-hash destination file '%1'").arg(dstAbsPath);
        QFile::remove(dstAbsPath);
        return false;
    }

    const QString expected = expectedSha256.isEmpty() ? QString::fromLatin1(writeHash.result().toHex()) : expectedSha256;
    if (readBackSha != expected) {
        outError = QString("Hash mismatch after verified copy (%1 != %2)").arg(readBackSha, expected);
        QFile::remove(dstAbsPath);
        return false;
    }

    outActualSha256 = readBackSha;
    return true;
}

std::vector<RegisteredWorkspaceInfo> WorkspaceManager::loadRegisteredWorkspacesLocked() const {
    std::vector<RegisteredWorkspaceInfo> result;
    const QString cfgPath = m_customConfigPath.isEmpty()
        ? ([&]() {
            QString cfgDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
            if (cfgDir.isEmpty()) cfgDir = QDir::homePath() + "/.config";
            return cfgDir + "/desk32/workspaces.json";
        })()
        : m_customConfigPath;

    QFile f(cfgPath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        return result;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return result;
    }

    const QJsonArray arr = doc.object().value(QStringLiteral("workspaces")).toArray();
    for (const QJsonValue& val : arr) {
        if (!val.isObject()) continue;
        const QJsonObject o = val.toObject();
        const QString p = QDir::cleanPath(o.value(QStringLiteral("path")).toString());
        if (p.isEmpty()) continue;
        RegisteredWorkspaceInfo info;
        info.path = p;
        info.lastUsedUtc = o.value(QStringLiteral("last_used_utc")).toString();
        result.push_back(std::move(info));
    }
    return result;
}

bool WorkspaceManager::saveRegisteredWorkspacesLocked(const std::vector<RegisteredWorkspaceInfo>& list) const {
    const QString cfgPath = m_customConfigPath.isEmpty()
        ? ([&]() {
            QString cfgDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
            if (cfgDir.isEmpty()) cfgDir = QDir::homePath() + "/.config";
            return cfgDir + "/desk32/workspaces.json";
        })()
        : m_customConfigPath;

    const QFileInfo fi(cfgPath);
    QDir().mkpath(fi.absolutePath());

    QJsonArray arr;
    for (const auto& w : list) {
        QJsonObject o;
        o.insert(QStringLiteral("path"), w.path);
        o.insert(QStringLiteral("last_used_utc"), w.lastUsedUtc);
        arr.append(o);
    }

    QJsonObject root;
    root.insert(QStringLiteral("format_version"), 1);
    root.insert(QStringLiteral("workspaces"), arr);

    QSaveFile save(cfgPath);
    if (!save.open(QIODevice::WriteOnly)) {
        return false;
    }
    save.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return save.commit();
}

bool WorkspaceManager::isWorkspaceInUseByOtherProcess(const QString& workspacePath) const {
    const QString canonical = resolveRealPath(workspacePath);
    if (canonical.isEmpty()) return false;

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!m_openWorkspacePath.isEmpty() && canonical == m_openWorkspacePath && m_lockFd >= 0) {
            return false;
        }
    }

    const QString lockFilePath = canonical + QStringLiteral("/.desk32/workspace.lock");
    if (!QFileInfo::exists(lockFilePath)) {
        return false;
    }

    const QByteArray enc = QFile::encodeName(lockFilePath);
    const int fd = ::open(enc.constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    const int rc = ::flock(fd, LOCK_EX | LOCK_NB);
    if (rc == 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return false;
    }
    const int e = errno;
    ::close(fd);
    return (e == EWOULDBLOCK || e == EAGAIN);
}

std::vector<RegisteredWorkspaceInfo> WorkspaceManager::listRegisteredWorkspaces() const {
    std::vector<RegisteredWorkspaceInfo> list;
    QString openPath;
    int openFd = -1;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        list = loadRegisteredWorkspacesLocked();
        openPath = m_openWorkspacePath;
        openFd = m_lockFd;
    }

    for (auto& info : list) {
        const QString canon = resolveRealPath(info.path);
        const QString effective = canon.isEmpty() ? info.path : canon;
        if (!openPath.isEmpty() && openFd >= 0 && effective == openPath) {
            info.lockState = WorkspaceLockState::Open;
        } else if (isWorkspaceInUseByOtherProcess(effective)) {
            info.lockState = WorkspaceLockState::InUse;
        } else {
            info.lockState = WorkspaceLockState::Locked;
        }
    }
    return list;
}

QVariantList WorkspaceManager::getRegisteredWorkspaces() const {
    QVariantList out;
    for (const auto& w : listRegisteredWorkspaces()) {
        QVariantMap m;
        m["path"] = w.path;
        m["lastUsedUtc"] = w.lastUsedUtc;
        m["lockState"] = static_cast<int>(w.lockState);
        m["lockStateText"] = (w.lockState == WorkspaceLockState::Open)
                                 ? QStringLiteral("Open")
                                 : ((w.lockState == WorkspaceLockState::InUse)
                                        ? QStringLiteral("InUse")
                                        : QStringLiteral("Locked"));
        m["selectable"] = (w.lockState != WorkspaceLockState::InUse);
        out.append(m);
    }
    return out;
}

bool WorkspaceManager::registerWorkspace(const QString& dirPath, QString& outError) {
    outError.clear();
    if (dirPath.trimmed().isEmpty()) {
        outError = QStringLiteral("Workspace directory path is empty.");
        setLastError(outError);
        return false;
    }

    const QFileInfo rawInfo(dirPath);
    if (!rawInfo.exists() || !rawInfo.isDir()) {
        outError = QString("Workspace directory does not exist or is not a directory: %1").arg(dirPath);
        setLastError(outError);
        return false;
    }

    const QString canonical = resolveRealPath(dirPath);
    if (canonical.isEmpty()) {
        outError = QString("Failed to resolve realpath for workspace: %1").arg(dirPath);
        setLastError(outError);
        return false;
    }

    if (canonical == QStringLiteral("/")) {
        outError = QStringLiteral("Filesystem root '/' cannot be registered as a dedicated workspace.");
        setLastError(outError);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        auto list = loadRegisteredWorkspacesLocked();
        for (const auto& existing : list) {
            const QString existingCanon = resolveRealPath(existing.path);
            const QString cmpPath = existingCanon.isEmpty() ? existing.path : existingCanon;
            if (canonical == cmpPath) {
                outError = QString("Workspace path matches an already registered workspace: %1").arg(existing.path);
                m_lastError = outError;
                core::log::LogService::instance().log(
                    "Workspace", "RegisterWorkspace", "Failed", outError, "Workspace", canonical,
                    "{}", "", "", core::log::ErrorCode::WorkspaceRegistrationConflict, 0.0);
                return false;
            }
            if (isSameOrSubpath(canonical, cmpPath) || isSameOrSubpath(cmpPath, canonical)) {
                outError = QString("Workspace path '%1' has a parent/child conflict with registered workspace '%2'.")
                               .arg(canonical, cmpPath);
                m_lastError = outError;
                core::log::LogService::instance().log(
                    "Workspace", "RegisterWorkspace", "Failed", outError, "Workspace", canonical,
                    "{}", "", "", core::log::ErrorCode::WorkspaceRegistrationConflict, 0.0);
                return false;
            }
        }

        RegisteredWorkspaceInfo entry;
        entry.path = canonical;
        entry.lastUsedUtc = QString();
        list.push_back(entry);

        if (!saveRegisteredWorkspacesLocked(list)) {
            outError = QStringLiteral("Failed to save workspace registry configuration.");
            m_lastError = outError;
            return false;
        }
    }

    core::log::LogService::instance().log(
        "Workspace", "RegisterWorkspace", "Success",
        QString("Registered dedicated workspace: %1").arg(canonical),
        "Workspace", canonical);
    emit workspaceStateChanged();
    return true;
}

bool WorkspaceManager::openWorkspace(const QString& dirPath, QString& outError) {
    outError.clear();
    const QString canonical = resolveRealPath(dirPath);
    if (canonical.isEmpty() || !QFileInfo(canonical).isDir()) {
        outError = QString("Invalid workspace path: %1").arg(dirPath);
        setLastError(outError);
        return false;
    }

    int acquiredFd = -1;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_selectionFinalized) {
            outError = QStringLiteral("Workspace selection is already finalized for this session; restart Desk32 to switch workspaces.");
            m_lastError = outError;
            return false;
        }

        auto registered = loadRegisteredWorkspacesLocked();
        auto regIt = std::find_if(registered.begin(), registered.end(), [&](const RegisteredWorkspaceInfo& r) {
            const QString rc = resolveRealPath(r.path);
            return (rc.isEmpty() ? r.path : rc) == canonical;
        });
        if (regIt == registered.end()) {
            outError = QString("Workspace is not registered: %1").arg(canonical);
            m_lastError = outError;
            return false;
        }

        const QString deskDir = canonical + QStringLiteral("/.desk32");
        if (!QDir().mkpath(deskDir + QStringLiteral("/saves")) ||
            !QDir().mkpath(deskDir + QStringLiteral("/objects")) ||
            !QDir().mkpath(deskDir + QStringLiteral("/tmp"))) {
            outError = QString("Failed to initialize .desk32 metadata directories in workspace: %1").arg(canonical);
            m_lastError = outError;
            return false;
        }

        const QString lockFilePath = deskDir + QStringLiteral("/workspace.lock");
        const QByteArray lockEnc = QFile::encodeName(lockFilePath);
        acquiredFd = ::open(lockEnc.constData(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (acquiredFd < 0) {
            outError = QString("Failed to open workspace lock file: %1 (%2)")
                           .arg(lockFilePath, QString::fromLocal8Bit(std::strerror(errno)));
            m_lastError = outError;
            return false;
        }

        if (::flock(acquiredFd, LOCK_EX | LOCK_NB) != 0) {
            const int e = errno;
            ::close(acquiredFd);
            acquiredFd = -1;
            if (e == EWOULDBLOCK || e == EAGAIN) {
                outError = QString("Workspace '%1' is currently in use by another Desk32 process.").arg(canonical);
            } else {
                outError = QString("Failed to acquire flock on workspace '%1': %2")
                               .arg(canonical, QString::fromLocal8Bit(std::strerror(e)));
            }
            m_lastError = outError;
            core::log::LogService::instance().log(
                "Workspace", "OpenWorkspace", "Failed", outError, "Workspace", canonical,
                "{}", "", "", core::log::ErrorCode::WorkspaceInUseByOtherProcess, 0.0);
            return false;
        }

        // Write diagnostic PID note into lockfile (never used as lock state itself; kernel flock is authoritative)
        if (::ftruncate(acquiredFd, 0) == 0) {
            const QByteArray pidText = QString("pid=%1 opened_utc=%2\n")
                                           .arg(QCoreApplication::applicationPid())
                                           .arg(utcIsoNow())
                                           .toUtf8();
            [[maybe_unused]] ssize_t ignored = ::write(acquiredFd, pidText.constData(), static_cast<size_t>(pidText.size()));
        }

        m_openWorkspacePath = canonical;
        m_lockFd = acquiredFd;
        m_selectionFinalized = true;
        m_readOnlyMode = false;
        m_externalChangeDetails.clear();
        m_externalModificationCount = 0;

        regIt->lastUsedUtc = utcIsoNow();
        saveRegisteredWorkspacesLocked(registered);
    }

    // Run startup cleanup, Stage 2 integrity verification against last known index, GC & Stage 1 watcher on worker thread
    runOnWorkerThread([this]() {
        cleanTempDirectoryLocked();
        std::map<QString, StateIndexEntry> seededIndex;
        if (loadStateIndexLocked(seededIndex)) {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_logOnlyProvenanceRelPaths.clear();
            for (const auto& [rp, se] : seededIndex) {
                if (se.logOnlyProvenance) {
                    m_logOnlyProvenanceRelPaths.insert(rp);
                }
            }
        }
        QStringList changes;
        std::vector<WorkspaceFileEntry> current;
        scanCurrentWorkspaceLocked(current, false);
        detectExternalChangesAgainstIndexLocked(current, changes);
        saveStateIndexLocked(current);
        refreshWatcherLocked();
    });
    runGarbageCollection();

    core::log::LogService::instance().log(
        "Workspace", "OpenWorkspace", "Success",
        QString("Opened dedicated workspace with exclusive flock: %1").arg(canonical),
        "Workspace", canonical);
    emit workspaceStateChanged();
    emit workspaceContentsChanged();
    return true;
}

bool WorkspaceManager::registerAndOpenWorkspace(const QString& dirPath, QString& outError) {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_selectionFinalized) {
            outError = QStringLiteral("Workspace selection is already finalized for this session; restart Desk32 to switch workspaces.");
            m_lastError = outError;
            return false;
        }
    }
    const QString canonical = resolveRealPath(dirPath);
    if (!canonical.isEmpty()) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        const auto reg = loadRegisteredWorkspacesLocked();
        const bool alreadyReg = std::any_of(reg.begin(), reg.end(), [&](const RegisteredWorkspaceInfo& r) {
            const QString rc = resolveRealPath(r.path);
            return (rc.isEmpty() ? r.path : rc) == canonical;
        });
        if (alreadyReg) {
            outError = QString("Workspace '%1' is already registered; select it from the registered list.").arg(canonical);
            m_lastError = outError;
            return false;
        }
    }
    if (!registerWorkspace(dirPath, outError)) {
        return false;
    }
    return openWorkspace(dirPath, outError);
}

void WorkspaceManager::enterReadOnlyMode() {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_selectionFinalized) {
            return;
        }
        m_selectionFinalized = true;
        m_readOnlyMode = true;
        m_openWorkspacePath.clear();
    }
    core::log::LogService::instance().log(
        "Workspace", "EnterReadOnlyMode", "Info",
        QStringLiteral("Started Desk32 without opening a workspace (fail-closed read-only mode)."),
        "Workspace", "ReadOnly");
    emit workspaceStateChanged();
}

void WorkspaceManager::closeWorkspaceForShutdown() {
    QString closedPath;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        closedPath = m_openWorkspacePath;
        if (m_lockFd >= 0) {
            ::flock(m_lockFd, LOCK_UN);
            ::close(m_lockFd);
            m_lockFd = -1;
        }
        m_openWorkspacePath.clear();
        m_readOnlyMode = true;
        m_externalChangeDetails.clear();
    }

    if (m_watcher) {
        auto clearWatcher = [this]() {
            if (!m_watcher) return;
            const QStringList files = m_watcher->files();
            if (!files.isEmpty()) m_watcher->removePaths(files);
            const QStringList dirs = m_watcher->directories();
            if (!dirs.isEmpty()) m_watcher->removePaths(dirs);
        };
        if (QThread::currentThread() == this->thread()) {
            clearWatcher();
        } else {
            QMetaObject::invokeMethod(this, clearWatcher, Qt::QueuedConnection);
        }
    }

    if (!closedPath.isEmpty()) {
        core::log::LogService::instance().log(
            "Workspace", "CloseWorkspace", "Info",
            QString("Released workspace lock on shutdown: %1").arg(closedPath),
            "Workspace", closedPath);
    }
}

bool WorkspaceManager::isPathBlockedByLockedWorkspace(const QString& targetPath, QString* outReason) const {
    if (targetPath.isEmpty()) return false;
    // Allow synthetic test URIs
    if (targetPath.startsWith(QStringLiteral("synthetic://")) ||
        targetPath.startsWith(QStringLiteral("/path/to/"))) {
        return false;
    }

    std::vector<RegisteredWorkspaceInfo> reg;
    QString openPath;
    bool isOpen = false;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        reg = loadRegisteredWorkspacesLocked();
        openPath = m_openWorkspacePath;
        isOpen = (!m_readOnlyMode && !m_openWorkspacePath.isEmpty() && m_lockFd >= 0);
    }

    const QString cleanAbs = QDir::cleanPath(QFileInfo(targetPath).absoluteFilePath());
    const QString canon = resolveRealPath(targetPath);

    // Also resolve deepest existing ancestor for paths whose leaf doesn't exist yet
    QString ancestorCanon;
    if (canon.isEmpty()) {
        QDir probe(cleanAbs);
        while (!probe.exists() && !probe.isRoot()) {
            if (!probe.cdUp()) break;
        }
        ancestorCanon = resolveRealPath(probe.absolutePath());
    }

    for (const auto& w : reg) {
        const QString wCanon = resolveRealPath(w.path);
        const QString wEff = wCanon.isEmpty() ? QDir::cleanPath(w.path) : wCanon;
        const bool isCurrentlyOpen = isOpen && (wEff == openPath);
        if (isCurrentlyOpen) continue;

        if (isSameOrSubpath(cleanAbs, wEff) ||
            (!canon.isEmpty() && isSameOrSubpath(canon, wEff)) ||
            (!ancestorCanon.isEmpty() && isSameOrSubpath(ancestorCanon, wEff))) {
            if (outReason) {
                *outReason = QString("Access denied: path '%1' resides inside locked workspace '%2'.")
                                 .arg(targetPath, wEff);
            }
            return true;
        }
    }
    return false;
}

bool WorkspaceManager::resolveInsideOpenWorkspace(const QString& inputPath,
                                                bool allowNonExistentLeaf,
                                                QString& outAbsPath,
                                                QString& outRelPath,
                                                QString& outError) const
{
    // Copy input before clearing out-params in case caller passes outRelPath as inputPath
    const QString inputCopy = inputPath;
    outAbsPath.clear();
    outRelPath.clear();
    outError.clear();

    QString wsRoot;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_readOnlyMode || m_openWorkspacePath.isEmpty() || m_lockFd < 0) {
            outError = QStringLiteral("No workspace is currently open (fail-closed read-only mode).");
            return false;
        }
        wsRoot = m_openWorkspacePath;
    }

    if (inputCopy.isEmpty()) {
        outError = QStringLiteral("Target path is empty.");
        return false;
    }

    QString relCandidate;
    if (QFileInfo(inputCopy).isAbsolute()) {
        const QString cleaned = QDir::cleanPath(inputCopy);
        if (!isSameOrSubpath(cleaned, wsRoot) || cleaned == wsRoot) {
            outError = QString("Path '%1' is outside open workspace '%2'.").arg(inputCopy, wsRoot);
            return false;
        }
        relCandidate = QDir(wsRoot).relativeFilePath(cleaned);
    } else {
        relCandidate = QDir::cleanPath(inputCopy);
    }

    if (relCandidate.isEmpty() || relCandidate == QStringLiteral(".") ||
        relCandidate.startsWith(QStringLiteral("../")) || relCandidate == QStringLiteral("..") ||
        QFileInfo(relCandidate).isAbsolute()) {
        outError = QString("Invalid relative workspace path: '%1'").arg(inputCopy);
        return false;
    }

    const QStringList segments = relCandidate.split('/', Qt::SkipEmptyParts);
    if (segments.isEmpty()) {
        outError = QStringLiteral("Empty workspace path segments.");
        return false;
    }

    if (isInternalDesk32EntryName(segments.first())) {
        outError = QString("Internal workspace path '%1' is reserved and cannot be accessed as user content.").arg(relCandidate);
        return false;
    }

    // Traverse segment-by-segment checking lstat to strictly forbid ANY symlink traversal
    QString currentPath = wsRoot;
    for (int i = 0; i < segments.size(); ++i) {
        const QString& seg = segments[i];
        if (seg == QStringLiteral(".") || seg == QStringLiteral("..")) {
            outError = QString("Invalid traversal segment in path: %1").arg(relCandidate);
            return false;
        }
        currentPath = currentPath + QStringLiteral("/") + seg;
        const QByteArray enc = QFile::encodeName(currentPath);
        struct stat st{};
        if (::lstat(enc.constData(), &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                outError = QString("Symbolic links are disallowed in workspace operations: %1").arg(currentPath);
                return false;
            }
            const QString canon = resolveRealPath(currentPath);
            if (canon.isEmpty() || !isSameOrSubpath(canon, wsRoot)) {
                outError = QString("Containment check failed: '%1' escapes workspace '%2'.").arg(currentPath, wsRoot);
                return false;
            }
        } else {
            if (!allowNonExistentLeaf && i == segments.size() - 1) {
                outError = QString("Workspace file does not exist: %1").arg(currentPath);
                return false;
            }
            if (i < segments.size() - 1 && !allowNonExistentLeaf) {
                outError = QString("Parent directory does not exist in workspace: %1").arg(currentPath);
                return false;
            }
        }
    }

    outAbsPath = currentPath;
    outRelPath = segments.join('/');
    return true;
}

bool WorkspaceManager::isPathInsideOpenWorkspace(const QString& targetPath, bool allowNonExistentLeaf, QString* outError) const {
    QString absP, relP, err;
    const bool ok = resolveInsideOpenWorkspace(targetPath, allowNonExistentLeaf, absP, relP, err);
    if (!ok && outError) *outError = err;
    return ok;
}

bool WorkspaceManager::selectWorkspaceAtStartup(const QString& dirPath) {
    QString err;
    return openWorkspace(dirPath, err);
}

bool WorkspaceManager::registerAndSelectWorkspaceAtStartup(const QString& dirPath) {
    QString err;
    return registerAndOpenWorkspace(dirPath, err);
}

void WorkspaceManager::continueInReadOnlyMode() {
    enterReadOnlyMode();
}

void WorkspaceManager::scanCurrentWorkspaceLocked(std::vector<WorkspaceFileEntry>& outEntries, bool computeTags) const {
    outEntries.clear();
    QString wsRoot;
    std::set<QString> logOnlySnapshot;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_readOnlyMode || m_openWorkspacePath.isEmpty() || m_lockFd < 0) return;
        wsRoot = m_openWorkspacePath;
        logOnlySnapshot = m_logOnlyProvenanceRelPaths;
    }

    std::function<void(const QString&, const QString&)> walk = [&](const QString& curDir, const QString& relPrefix) {
        QDir dir(curDir);
        const QFileInfoList items = dir.entryInfoList(
            QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
            QDir::Name);
        for (const QFileInfo& fi : items) {
            const QString name = fi.fileName();
            if (relPrefix.isEmpty() && isInternalDesk32EntryName(name)) {
                continue;
            }

            const QString fullPath = curDir + QStringLiteral("/") + name;
            const QString relPath = relPrefix.isEmpty() ? name : (relPrefix + QStringLiteral("/") + name);
            const QByteArray enc = QFile::encodeName(fullPath);

            struct stat st{};
            if (::lstat(enc.constData(), &st) != 0) continue;
            if (S_ISLNK(st.st_mode)) continue; // Strictly exclude symlinks (Spec §2.5)

            if (S_ISDIR(st.st_mode)) {
                walk(fullPath, relPath);
            } else if (S_ISREG(st.st_mode)) {
                WorkspaceFileEntry entry;
                entry.relativePath = relPath;
                entry.absolutePath = fullPath;
                entry.size = static_cast<qint64>(st.st_size);
                entry.mtimeNs = static_cast<qint64>(st.st_mtim.tv_sec) * 1000000000LL +
                                static_cast<qint64>(st.st_mtim.tv_nsec);
                if (computeFileSha256(fullPath, entry.sha256)) {
                    entry.logOnlyProvenance = (logOnlySnapshot.count(relPath) > 0);
                    if (computeTags) {
                        entry.metadata = metadata::MetadataReader::parseTags(fullPath.toStdString());
                        if (!entry.metadata.provenanceEmbedSupported) {
                            entry.logOnlyProvenance = true;
                        }
                    }
                    outEntries.push_back(std::move(entry));
                }
            }
        }
    };

    walk(wsRoot, QString());
}

void WorkspaceManager::refreshWatcherLocked() {
    if (!m_watcher) return;
    QString wsRoot;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_readOnlyMode || m_openWorkspacePath.isEmpty() || m_lockFd < 0) return;
        wsRoot = m_openWorkspacePath;
    }

    QStringList dirsToWatch;
    QStringList filesToWatch;
    dirsToWatch.append(wsRoot);

    std::function<void(const QString&, bool)> collectWatch = [&](const QString& curDir, bool isRoot) {
        QDir d(curDir);
        const QFileInfoList items = d.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden);
        for (const QFileInfo& fi : items) {
            if (isRoot && isInternalDesk32EntryName(fi.fileName())) continue;
            const QString fullPath = curDir + QStringLiteral("/") + fi.fileName();
            const QByteArray enc = QFile::encodeName(fullPath);
            struct stat st{};
            if (::lstat(enc.constData(), &st) != 0 || S_ISLNK(st.st_mode)) continue;
            if (S_ISDIR(st.st_mode)) {
                dirsToWatch.append(fullPath);
                collectWatch(fullPath, false);
            } else if (S_ISREG(st.st_mode)) {
                filesToWatch.append(fullPath);
            }
        }
    };
    collectWatch(wsRoot, true);

    auto applyWatch = [this, dirsToWatch, filesToWatch]() {
        if (!m_watcher) return;
        const QStringList oldDirs = m_watcher->directories();
        if (!oldDirs.isEmpty()) m_watcher->removePaths(oldDirs);
        const QStringList oldFiles = m_watcher->files();
        if (!oldFiles.isEmpty()) m_watcher->removePaths(oldFiles);
        if (!dirsToWatch.isEmpty()) m_watcher->addPaths(dirsToWatch);
        if (!filesToWatch.isEmpty()) m_watcher->addPaths(filesToWatch);
    };

    if (QThread::currentThread() == this->thread()) {
        applyWatch();
    } else {
        QMetaObject::invokeMethod(this, applyWatch, Qt::QueuedConnection);
    }
}

void WorkspaceManager::cleanTempDirectoryLocked() {
    QString wsRoot;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        wsRoot = m_openWorkspacePath;
    }
    if (wsRoot.isEmpty()) return;
    const QString tmpDir = wsRoot + QStringLiteral("/.desk32/tmp");
    QDir d(tmpDir);
    if (!d.exists()) return;
    const QFileInfoList items = d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo& fi : items) {
        if (fi.isDir() && !fi.isSymLink()) {
            QDir(fi.absoluteFilePath()).removeRecursively();
        } else {
            QFile::remove(fi.absoluteFilePath());
        }
    }
}

bool WorkspaceManager::loadStateIndexLocked(std::map<QString, StateIndexEntry>& outIndex) const {
    outIndex.clear();
    QString wsRoot;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        wsRoot = m_openWorkspacePath;
    }
    if (wsRoot.isEmpty()) return false;

    const QString idxPath = wsRoot + QStringLiteral("/.desk32/state_index.json");
    QFile f(idxPath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return false;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;

    const QJsonArray arr = doc.object().value(QStringLiteral("files")).toArray();
    for (const QJsonValue& val : arr) {
        if (!val.isObject()) continue;
        const QJsonObject o = val.toObject();
        const QString rp = o.value(QStringLiteral("relative_path")).toString();
        const QString sha = o.value(QStringLiteral("sha256")).toString();
        const qint64 sz = o.value(QStringLiteral("size")).toInteger();
        const bool logOnly = o.value(QStringLiteral("log_only_provenance")).toBool(false);
        if (!rp.isEmpty() && !sha.isEmpty()) {
            outIndex[rp] = StateIndexEntry{sha, sz, logOnly};
        }
    }
    return true;
}

bool WorkspaceManager::saveStateIndexLocked(const std::vector<WorkspaceFileEntry>& entries) const {
    QString wsRoot;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        wsRoot = m_openWorkspacePath;
    }
    if (wsRoot.isEmpty()) return false;

    QJsonArray arr;
    for (const auto& e : entries) {
        QJsonObject o;
        o.insert(QStringLiteral("relative_path"), e.relativePath);
        o.insert(QStringLiteral("sha256"), e.sha256);
        o.insert(QStringLiteral("size"), e.size);
        o.insert(QStringLiteral("log_only_provenance"), e.logOnlyProvenance);
        arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("format_version"), 1);
    root.insert(QStringLiteral("updated_utc"), utcIsoNow());
    root.insert(QStringLiteral("files"), arr);

    const QString idxPath = wsRoot + QStringLiteral("/.desk32/state_index.json");
    QSaveFile save(idxPath);
    if (!save.open(QIODevice::WriteOnly)) return false;
    save.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return save.commit();
}

void WorkspaceManager::detectExternalChangesAgainstIndexLocked(const std::vector<WorkspaceFileEntry>& currentEntries,
                                                               QStringList& outMessages)
{
    std::map<QString, StateIndexEntry> prevIndex;
    if (!loadStateIndexLocked(prevIndex)) {
        return;
    }

    std::map<QString, const WorkspaceFileEntry*> curMap;
    for (const auto& e : currentEntries) {
        curMap[e.relativePath] = &e;
    }

    QStringList modifiedAbsPaths;
    for (const auto& [relPath, expected] : prevIndex) {
        auto it = curMap.find(relPath);
        if (it == curMap.end()) {
            const QString msg = QString("External removal/rename detected for workspace file: %1").arg(relPath);
            outMessages.append(msg);
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                ++m_externalModificationCount;
                m_externalChangeDetails[relPath] = msg;
            }
            core::log::LogService::instance().log(
                "Workspace", "ExternalModification", "Warning", msg, "WorkspaceFile", relPath,
                QString("{\"type\":\"removed_or_renamed\",\"expected_sha256\":\"%1\"}").arg(expected.sha256),
                "", "", core::log::ErrorCode::WorkspaceExternalModification, 0.0);
            emit externalModificationDetected(relPath, msg);
        } else if (it->second->sha256 != expected.sha256) {
            const QString msg = QString("External modification detected (SHA-256 changed) for workspace file: %1").arg(relPath);
            outMessages.append(msg);
            modifiedAbsPaths.append(it->second->absolutePath);
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                ++m_externalModificationCount;
                m_externalChangeDetails[relPath] = msg;
            }
            core::log::LogService::instance().log(
                "Workspace", "ExternalModification", "Warning", msg, "WorkspaceFile", relPath,
                QString("{\"type\":\"content_or_tag_modified\",\"expected_sha256\":\"%1\",\"actual_sha256\":\"%2\"}")
                    .arg(expected.sha256, it->second->sha256),
                "", "", core::log::ErrorCode::WorkspaceExternalModification, 0.0);
            emit externalModificationDetected(relPath, msg);
        }
    }

    // Detect externally added regular files on disk that are not recorded in prevIndex
    for (const auto& e : currentEntries) {
        if (prevIndex.find(e.relativePath) == prevIndex.end()) {
            const QString msg = QString("Externally added file detected in workspace: %1").arg(e.relativePath);
            outMessages.append(msg);
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                ++m_externalModificationCount;
                m_externalChangeDetails[e.relativePath] = msg;
            }
            core::log::LogService::instance().log(
                "Workspace", "ExternalModification", "Warning", msg, "WorkspaceFile", e.relativePath,
                QString("{\"type\":\"externally_added\",\"actual_sha256\":\"%1\"}").arg(e.sha256),
                "", "", core::log::ErrorCode::WorkspaceExternalModification, 0.0);
            emit externalModificationDetected(e.relativePath, msg);
        }
    }

    if (!modifiedAbsPaths.isEmpty()) {
        MetadataReloadCallback reloadCb;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            reloadCb = m_metadataReloadCallback;
        }
        if (reloadCb) {
            reloadCb(modifiedAbsPaths);
        }
    }

    // Also verify integrity of object store items against their own filenames (content-addressed SHA-256 check)
    QString wsRoot;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        wsRoot = m_openWorkspacePath;
    }
    if (!wsRoot.isEmpty()) {
        std::vector<SaveManifest> saves;
        if (loadAllSavesLocked(saves)) {
            std::set<QString> checkedHashes;
            for (const auto& s : saves) {
                for (const auto& f : s.files) {
                    if (checkedHashes.count(f.sha256)) continue;
                    const QString objPath = wsRoot + QStringLiteral("/.desk32/objects/") + f.sha256;
                    if (QFileInfo::exists(objPath)) {
                        checkedHashes.insert(f.sha256);
                        QString actualObjSha;
                        if (!computeFileSha256(objPath, actualObjSha) || actualObjSha != f.sha256) {
                            const QString msg = QString("External corruption detected in saved object %1 (save '%2')").arg(f.sha256, s.name);
                            outMessages.append(msg);
                            {
                                std::lock_guard<std::mutex> lock(m_stateMutex);
                                ++m_externalModificationCount;
                            }
                            core::log::LogService::instance().log(
                                "Workspace", "ExternalModification", "Warning", msg, "SaveObject", objPath,
                                "{}", "", "", core::log::ErrorCode::WorkspaceExternalModification, 0.0);
                            emit externalModificationDetected(objPath, msg);
                        }
                    }
                }
            }
        }
    }
}

QStringList WorkspaceManager::verifyWorkspaceIntegrity() {
    if (!hasOpenWorkspace()) return {};
    return runOnWorkerThread([this]() -> QStringList {
        QStringList messages;
        std::vector<WorkspaceFileEntry> current;
        scanCurrentWorkspaceLocked(current, false);
        detectExternalChangesAgainstIndexLocked(current, messages);
        saveStateIndexLocked(current);
        refreshWatcherLocked();
        return messages;
    });
}

QStringList WorkspaceManager::checkExternalChangesFromUi() {
    return verifyWorkspaceIntegrity();
}

std::vector<WorkspaceFileEntry> WorkspaceManager::listWorkspaceFiles(const QString& relativeSubDir) const {
    if (!hasOpenWorkspace()) return {};
    return runOnWorkerThread([this, relativeSubDir]() -> std::vector<WorkspaceFileEntry> {
        std::vector<WorkspaceFileEntry> all;
        scanCurrentWorkspaceLocked(all, true);

        std::map<QString, StateIndexEntry> prevIndex;
        loadStateIndexLocked(prevIndex);

        std::map<QString, QString> persistedExtChanges;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            persistedExtChanges = m_externalChangeDetails;
        }

        std::vector<WorkspaceFileEntry> filtered;
        const QString cleanSub = QDir::cleanPath(relativeSubDir);
        const bool filterBySub = !cleanSub.isEmpty() && cleanSub != QStringLiteral(".");

        for (auto& e : all) {
            if (filterBySub && !isSameOrSubpath(e.relativePath, cleanSub)) {
                continue;
            }
            auto it = prevIndex.find(e.relativePath);
            if (it != prevIndex.end()) {
                e.logOnlyProvenance = e.logOnlyProvenance || it->second.logOnlyProvenance;
            }
            if (it != prevIndex.end() && it->second.sha256 != e.sha256) {
                e.externallyModified = true;
                e.externalChangeDetail = QStringLiteral("SHA-256 differs from last known Desk32 state");
            } else {
                auto extIt = persistedExtChanges.find(e.relativePath);
                if (extIt != persistedExtChanges.end()) {
                    e.externallyModified = true;
                    e.externalChangeDetail = extIt->second;
                }
            }
            filtered.push_back(std::move(e));
        }
        return filtered;
    });
}

QVariantList WorkspaceManager::getWorkspaceFiles(const QString& relativeSubDir) const {
    QVariantList list;
    for (const auto& e : listWorkspaceFiles(relativeSubDir)) {
        QVariantMap m;
        m["relativePath"] = e.relativePath;
        m["absolutePath"] = e.absolutePath;
        m["fileName"] = QFileInfo(e.relativePath).fileName();
        m["sha256"] = e.sha256;
        m["size"] = e.size;
        m["externallyModified"] = e.externallyModified;
        m["externalChangeDetail"] = e.externalChangeDetail;
        m["logOnlyProvenance"] = e.logOnlyProvenance || !e.metadata.provenanceEmbedSupported;
        m["metadata"] = metadataToVariantMap(e.metadata);
        list.append(m);
    }
    return list;
}

QVariantMap WorkspaceManager::getFileDetails(const QString& relativePath) const {
    QVariantMap out;
    if (!hasOpenWorkspace()) return out;

    QString absP, relP, err;
    if (!resolveInsideOpenWorkspace(relativePath, false, absP, relP, err)) {
        out["error"] = err;
        return out;
    }

    QString sha;
    qint64 sz = 0;
    computeFileSha256(absP, sha, &sz);
    const auto meta = metadata::MetadataReader::parseTags(absP.toStdString());

    std::map<QString, StateIndexEntry> idx;
    const bool hasIdx = loadStateIndexLocked(idx);
    const auto idxIt = idx.find(relP);
    const bool logOnly = !meta.provenanceEmbedSupported ||
                         (hasIdx && idxIt != idx.end() && idxIt->second.logOnlyProvenance);

    out["relativePath"] = relP;
    out["absolutePath"] = absP;
    out["sha256"] = sha;
    out["size"] = sz;
    out["logOnlyProvenance"] = logOnly;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        auto it = m_externalChangeDetails.find(relP);
        if (it != m_externalChangeDetails.end()) {
            out["externallyModified"] = true;
            out["externalChangeDetail"] = it->second;
        } else {
            out["externallyModified"] = false;
            out["externalChangeDetail"] = QString();
        }
    }
    out["metadata"] = metadataToVariantMap(meta);
    return out;
}

ImportResult WorkspaceManager::importFile(const QString& sourceFilePath, const QString& relativeDestDir) {
    return runOnWorkerThread([this, sourceFilePath, relativeDestDir]() -> ImportResult {
        ImportResult res;
        res.sourcePath = sourceFilePath;

        QString wsRoot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_readOnlyMode || m_openWorkspacePath.isEmpty() || m_lockFd < 0) {
                res.status = ImportResultStatus::Failed;
                res.message = QStringLiteral("Cannot import file: no workspace open (fail-closed read-only mode).");
                m_lastError = res.message;
                return res;
            }
            wsRoot = m_openWorkspacePath;
        }

        QString blockReason;
        if (isPathBlockedByLockedWorkspace(sourceFilePath, &blockReason)) {
            res.status = ImportResultStatus::Failed;
            res.message = blockReason;
            setLastError(res.message);
            core::log::LogService::instance().log(
                "Workspace", "ImportFile", "Failed", res.message, "AudioFile", sourceFilePath,
                "{}", "", "", core::log::ErrorCode::WorkspaceLockedAccessDenied, 0.0);
            return res;
        }

        QString linkErr;
        if (!checkRegularNoHardlink(sourceFilePath, linkErr)) {
            res.status = ImportResultStatus::Failed;
            res.message = linkErr;
            setLastError(res.message);
            core::log::LogService::instance().log(
                "Workspace", "ImportFile", "Failed", res.message, "AudioFile", sourceFilePath,
                "{}", "", "", core::log::ErrorCode::WorkspaceHardLinkRejected, 0.0);
            return res;
        }

        const QString sourceCanon = resolveRealPath(sourceFilePath);
        if (sourceCanon.isEmpty() || !QFileInfo(sourceCanon).isFile()) {
            res.status = ImportResultStatus::Failed;
            res.message = QString("Source file does not exist or cannot be resolved: %1").arg(sourceFilePath);
            setLastError(res.message);
            return res;
        }

        // Prevent self-copy when source file already resides inside the currently open workspace (§2.5)
        if (isSameOrSubpath(sourceCanon, wsRoot)) {
            res.status = ImportResultStatus::Failed;
            res.message = QString("Source file is already inside the open workspace (self-copy prevented): %1").arg(sourceCanon);
            setLastError(res.message);
            core::log::LogService::instance().log(
                "Workspace", "ImportFile", "Failed", res.message, "AudioFile", sourceCanon);
            return res;
        }

        // AUD-10 / §2.3: Strictly reject mono and multichannel audio upfront (2-channel stereo only)
        {
            decoder::FFmpegAudioDecoder probeDecoder;
            if (!probeDecoder.open(sourceCanon.toStdString())) {
                const std::string decErr = probeDecoder.lastError();
                if (decErr.find("Non-stereo") != std::string::npos ||
                    decErr.find("AUD-10") != std::string::npos) {
                    res.status = ImportResultStatus::Failed;
                    res.message = QString::fromStdString(decErr);
                    setLastError(res.message);
                    core::log::LogService::instance().log(
                        "Workspace", "ImportFile", "Failed", res.message, "AudioFile", sourceCanon,
                        "{}", "", "", core::log::ErrorCode::WorkspaceNonStereoRejected, 0.0);
                    return res;
                }
            } else if (probeDecoder.formatInfo().channels != 2) {
                res.status = ImportResultStatus::Failed;
                res.message = QString("Non-stereo audio format rejected: source has %1 channel(s). Stereo 2-channel audio only (AUD-10).")
                                  .arg(probeDecoder.formatInfo().channels);
                setLastError(res.message);
                core::log::LogService::instance().log(
                    "Workspace", "ImportFile", "Failed", res.message, "AudioFile", sourceCanon,
                    "{}", "", "", core::log::ErrorCode::WorkspaceNonStereoRejected, 0.0);
                return res;
            }
        }

        // (1) & (2): Open read-only and compute original SHA-256
        QString origSha;
        qint64 origSize = 0;
        if (!computeFileSha256(sourceCanon, origSha, &origSize)) {
            res.status = ImportResultStatus::Failed;
            res.message = QString("Failed to read source file (read-only) and compute SHA-256: %1").arg(sourceCanon);
            setLastError(res.message);
            return res;
        }
        res.originalSha256 = origSha;

        const QString fileName = QFileInfo(sourceFilePath).fileName();
        QString relTarget = relativeDestDir.trimmed();
        while (relTarget.startsWith('/')) relTarget.remove(0, 1);
        while (relTarget.endsWith('/')) relTarget.chop(1);
        const QString fullRelCandidate = relTarget.isEmpty() ? fileName : (relTarget + QStringLiteral("/") + fileName);

        QString destAbsPath, destRelPath, contErr;
        if (!resolveInsideOpenWorkspace(fullRelCandidate, /*allowNonExistentLeaf=*/true, destAbsPath, destRelPath, contErr)) {
            res.status = ImportResultStatus::Failed;
            res.message = contErr;
            setLastError(res.message);
            core::log::LogService::instance().log(
                "Workspace", "ImportFile", "Failed", contErr, "WorkspaceFile", fullRelCandidate,
                "{}", "", "", core::log::ErrorCode::WorkspaceContainmentViolation, 0.0);
            return res;
        }
        res.destPath = destAbsPath;

        // Ensure parent directory inside workspace is created safely if sub-folder path specified
        const QString destParentDir = QFileInfo(destAbsPath).absolutePath();
        if (!QFileInfo::exists(destParentDir) && !QDir().mkpath(destParentDir)) {
            res.status = ImportResultStatus::Failed;
            res.message = QString("Failed to create destination folder inside workspace: %1").arg(destParentDir);
            setLastError(res.message);
            return res;
        }

        // Same-name collision check (§2.5)
        const QByteArray destEnc = QFile::encodeName(destAbsPath);
        struct stat destSt{};
        if (::lstat(destEnc.constData(), &destSt) == 0) {
            if (S_ISLNK(destSt.st_mode)) {
                res.status = ImportResultStatus::Conflict;
                res.message = QString("Collision aborted: destination is a symbolic link: %1").arg(destRelPath);
                setLastError(res.message);
                return res;
            }

            const metadata::TrackMetadata existingMeta = metadata::MetadataReader::parseTags(destAbsPath.toStdString());
            if (existingMeta.hasProvenance) {
                if (!existingMeta.provenanceValid) {
                    res.status = ImportResultStatus::Conflict;
                    res.message = QString("Name conflict: '%1' has corrupt or incomplete provenance metadata.").arg(destRelPath);
                    setLastError(res.message);
                    core::log::LogService::instance().log(
                        "Workspace", "ImportFile", "Failed", res.message, "WorkspaceFile", destRelPath,
                        "{}", "", "", core::log::ErrorCode::WorkspaceImportNameConflict, 0.0);
                    return res;
                }
                if (existingMeta.provenanceOriginalSha256 == origSha.toStdString()) {
                    res.status = ImportResultStatus::SkippedIdentical;
                    res.message = QString("Skipped identical file (matched provenance original SHA-256): %1").arg(destRelPath);
                    core::log::LogService::instance().log(
                        "Workspace", "ImportFile", "Info", res.message, "WorkspaceFile", destRelPath);
                    return res;
                }
                res.status = ImportResultStatus::Conflict;
                res.message = QString("Name conflict: '%1' already exists with different provenance content.").arg(destRelPath);
                setLastError(res.message);
                core::log::LogService::instance().log(
                    "Workspace", "ImportFile", "Failed", res.message, "WorkspaceFile", destRelPath,
                    "{}", "", "", core::log::ErrorCode::WorkspaceImportNameConflict, 0.0);
                return res;
            } else {
                QString existingWholeSha;
                if (computeFileSha256(destAbsPath, existingWholeSha) && existingWholeSha == origSha) {
                    res.status = ImportResultStatus::SkippedIdentical;
                    res.message = QString("Skipped identical file (matched file SHA-256): %1").arg(destRelPath);
                    core::log::LogService::instance().log(
                        "Workspace", "ImportFile", "Info", res.message, "WorkspaceFile", destRelPath);
                    return res;
                }
                res.status = ImportResultStatus::Conflict;
                res.message = QString("Name conflict: '%1' already exists with different content (no overwrite, no auto-rename).").arg(destRelPath);
                setLastError(res.message);
                core::log::LogService::instance().log(
                    "Workspace", "ImportFile", "Failed", res.message, "WorkspaceFile", destRelPath,
                    "{}", "", "", core::log::ErrorCode::WorkspaceImportNameConflict, 0.0);
                return res;
            }
        }

        // (3) Copy to workspace temporary staging file
        const QString tmpName = QString("import_%1_%2")
                                    .arg(QUuid::createUuid().toString(QUuid::Id128).left(12), fileName);
        const QString tmpAbsPath = wsRoot + QStringLiteral("/.desk32/tmp/") + tmpName;

        QString actualTmpSha;
        QString copyErr;
        if (!copyFileReadOnlyVerified(sourceCanon, tmpAbsPath, origSha, actualTmpSha, copyErr)) {
            res.status = ImportResultStatus::Failed;
            res.message = copyErr;
            setLastError(copyErr);
            return res;
        }

        // (4) Re-check source SHA-256 to guarantee source was not modified externally mid-copy
        QString sourceShaAfter;
        if (!computeFileSha256(sourceCanon, sourceShaAfter) || sourceShaAfter != origSha || actualTmpSha != origSha) {
            QFile::remove(tmpAbsPath);
            res.status = ImportResultStatus::Failed;
            res.message = QString("Source file changed externally during import copy (SHA-256 mismatch): %1").arg(sourceCanon);
            setLastError(res.message);
            core::log::LogService::instance().log(
                "Workspace", "ImportFile", "Failed", res.message, "AudioFile", sourceCanon,
                "{}", "", "", core::log::ErrorCode::WorkspaceImportHashMismatch, 0.0);
            return res;
        }

        // (5) Embed provenance tags via TagLib
        const QString copyTimeIso = localIsoWithOffsetNow();
        const metadata::TagWriteStatus tagStatus = metadata::MetadataReader::writeProvenanceTags(
            tmpAbsPath.toStdString(),
            sourceCanon.toStdString(),
            copyTimeIso.toStdString(),
            origSha.toStdString());

        if (tagStatus == metadata::TagWriteStatus::Success) {
            res.provenanceEmbedded = true;
        } else if (tagStatus == metadata::TagWriteStatus::UnsupportedFormat) {
            // Spec §2.5: formats that cannot embed tags record log-only provenance
            res.provenanceEmbedded = false;
        } else {
            QFile::remove(tmpAbsPath);
            res.status = ImportResultStatus::Failed;
            res.message = QString("TagLib provenance embedding failed (code=%1); temp file removed.").arg(static_cast<int>(tagStatus));
            setLastError(res.message);
            return res;
        }

        if (triggerFaultIfSet(WorkspaceFaultPoint::DuringImportBeforeCommit, destRelPath)) {
            QFile::remove(tmpAbsPath);
            res.status = ImportResultStatus::Failed;
            res.message = QStringLiteral("Injected fault before import commit; temporary file removed cleanly.");
            setLastError(res.message);
            return res;
        }

        // Re-check containment and absence of destination right before atomic commit
        if (!resolveInsideOpenWorkspace(destRelPath, true, destAbsPath, destRelPath, contErr) ||
            QFileInfo::exists(destAbsPath)) {
            QFile::remove(tmpAbsPath);
            res.status = ImportResultStatus::Conflict;
            res.message = contErr.isEmpty()
                              ? QString("Destination appeared concurrently during import: %1").arg(destRelPath)
                              : contErr;
            setLastError(res.message);
            return res;
        }

        // (6) Atomic commit rename
        std::error_code ec;
        std::filesystem::rename(tmpAbsPath.toStdString(), destAbsPath.toStdString(), ec);
        if (ec) {
            QFile::remove(tmpAbsPath);
            res.status = ImportResultStatus::Failed;
            res.message = QString("Failed to atomically commit imported file '%1': %2")
                              .arg(destRelPath, QString::fromStdString(ec.message()));
            setLastError(res.message);
            return res;
        }

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (!res.provenanceEmbedded) {
                m_logOnlyProvenanceRelPaths.insert(destRelPath);
            } else {
                m_logOnlyProvenanceRelPaths.erase(destRelPath);
            }
        }

        // (7) Log provenance & update state index + watcher
        std::vector<WorkspaceFileEntry> current;
        scanCurrentWorkspaceLocked(current, false);
        saveStateIndexLocked(current);
        refreshWatcherLocked();

        QJsonObject det;
        det.insert(QStringLiteral("original_path"), sourceCanon);
        det.insert(QStringLiteral("copy_timestamp"), copyTimeIso);
        det.insert(QStringLiteral("original_sha256"), origSha);
        det.insert(QStringLiteral("dest_relative_path"), destRelPath);
        det.insert(QStringLiteral("provenance_embedded"), res.provenanceEmbedded);
        det.insert(QStringLiteral("provenance_mode"),
                   res.provenanceEmbedded ? QStringLiteral("embedded_and_logged") : QStringLiteral("log_only"));

        core::log::LogService::instance().log(
            "Workspace", "ImportFile", "Success",
            QString("Imported '%1' into workspace (%2)").arg(destRelPath, res.provenanceEmbedded ? "provenance embedded" : "Log only"),
            "WorkspaceFile", destRelPath,
            QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)));

        res.status = ImportResultStatus::Imported;
        res.message = QString("Imported '%1' cleanly.").arg(destRelPath);
        emit workspaceContentsChanged();
        return res;
    });
}

QVariantMap WorkspaceManager::importFileFromUi(const QString& sourceFilePath, const QString& relativeDestDir) {
    const ImportResult r = importFile(sourceFilePath, relativeDestDir);
    QVariantMap m;
    m["status"] = static_cast<int>(r.status);
    m["statusText"] = (r.status == ImportResultStatus::Imported) ? QStringLiteral("Imported")
                      : ((r.status == ImportResultStatus::SkippedIdentical) ? QStringLiteral("Skipped")
                         : ((r.status == ImportResultStatus::Conflict) ? QStringLiteral("Conflict")
                            : QStringLiteral("Failed")));
    m["sourcePath"] = r.sourcePath;
    m["destPath"] = r.destPath;
    m["originalSha256"] = r.originalSha256;
    m["provenanceEmbedded"] = r.provenanceEmbedded;
    m["message"] = r.message;
    emit importCompleted(m);
    return m;
}

std::vector<ImportResult> WorkspaceManager::importFolder(const QString& sourceFolderPath, const QString& relativeDestDir) {
    std::vector<ImportResult> results;
    QString blockReason;
    if (isPathBlockedByLockedWorkspace(sourceFolderPath, &blockReason)) {
        ImportResult fail;
        fail.status = ImportResultStatus::Failed;
        fail.sourcePath = sourceFolderPath;
        fail.message = blockReason;
        results.push_back(fail);
        return results;
    }

    QDir dir(sourceFolderPath);
    if (!dir.exists()) return results;

    const QString folderBaseName = QFileInfo(QDir::cleanPath(sourceFolderPath)).fileName();
    QString baseSubDir = relativeDestDir.trimmed();
    while (baseSubDir.startsWith('/')) baseSubDir.remove(0, 1);
    while (baseSubDir.endsWith('/')) baseSubDir.chop(1);
    const QString targetSubDir = baseSubDir.isEmpty() ? folderBaseName : (baseSubDir + QStringLiteral("/") + folderBaseName);

    std::function<void(const QString&, const QString&)> importRecursive = [&](const QString& srcDir, const QString& curRelDest) {
        QDir d(srcDir);
        const QFileInfoList items = d.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& fi : items) {
            if (fi.isSymLink()) continue;
            if (fi.isDir()) {
                importRecursive(fi.absoluteFilePath(), curRelDest + QStringLiteral("/") + fi.fileName());
            } else if (fi.isFile() && isSupportedAudioFileName(fi.fileName())) {
                results.push_back(importFile(fi.absoluteFilePath(), curRelDest));
            }
        }
    };

    importRecursive(dir.absolutePath(), targetSubDir);
    return results;
}

QVariantList WorkspaceManager::importFolderFromUi(const QString& sourceFolderPath, const QString& relativeDestDir) {
    QVariantList out;
    for (const auto& r : importFolder(sourceFolderPath, relativeDestDir)) {
        QVariantMap m;
        m["status"] = static_cast<int>(r.status);
        m["statusText"] = (r.status == ImportResultStatus::Imported) ? QStringLiteral("Imported")
                          : ((r.status == ImportResultStatus::SkippedIdentical) ? QStringLiteral("Skipped")
                             : ((r.status == ImportResultStatus::Conflict) ? QStringLiteral("Conflict")
                                : QStringLiteral("Failed")));
        m["sourcePath"] = r.sourcePath;
        m["destPath"] = r.destPath;
        m["provenanceEmbedded"] = r.provenanceEmbedded;
        m["message"] = r.message;
        out.append(m);
    }
    return out;
}

bool WorkspaceManager::isObjectReferencedLocked(const QString& sha256) const {
    std::vector<SaveManifest> saves;
    if (!loadAllSavesLocked(saves)) return false;
    for (const auto& s : saves) {
        for (const auto& f : s.files) {
            if (f.sha256 == sha256) return true;
        }
    }
    return false;
}

bool WorkspaceManager::evacuateCopyOnWriteLocked(const QString& absPath, const QString& relPath, QString& outError) {
    outError.clear();
    QString wsRoot;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        wsRoot = m_openWorkspacePath;
    }
    if (wsRoot.isEmpty()) {
        outError = QStringLiteral("No open workspace.");
        return false;
    }

    // Step 2: compute current SHA-256 of target file prior to modification/deletion
    QString currentSha;
    qint64 currentSize = 0;
    if (!computeFileSha256(absPath, currentSha, &currentSize)) {
        outError = QString("Failed to hash file prior to copy-on-write evacuation: %1").arg(relPath);
        return false;
    }

    // Check against saves: detect if a saved version at relPath differs externally prior to COW evacuation
    std::vector<SaveManifest> saves;
    loadAllSavesLocked(saves);

    bool referencedCurrentSha = false;
    for (const auto& s : saves) {
        for (const auto& f : s.files) {
            if (f.sha256 == currentSha) {
                referencedCurrentSha = true;
            }
            if (f.relativePath == relPath && f.sha256 != currentSha) {
                const QString objPath = wsRoot + QStringLiteral("/.desk32/objects/") + f.sha256;
                if (!QFileInfo::exists(objPath)) {
                    const QString warnMsg = QString(
                        "External modification before copy-on-write evacuation altered '%1' (saved hash %2 != current %3); previous version was not yet evacuated.")
                        .arg(relPath, f.sha256, currentSha);
                    {
                        std::lock_guard<std::mutex> lock(m_stateMutex);
                        ++m_externalModificationCount;
                    }
                    core::log::LogService::instance().log(
                        "Workspace", "ExternalModification", "Warning", warnMsg, "WorkspaceFile", relPath,
                        "{}", "", "", core::log::ErrorCode::WorkspaceExternalModification, 0.0);
                    emit externalModificationDetected(relPath, warnMsg);
                }
            }
        }
    }

    // Step 3: if currentSha is referenced by any save, materialize it into .desk32/objects/
    if (!referencedCurrentSha) {
        return true;
    }

    const QString objectsDir = wsRoot + QStringLiteral("/.desk32/objects");
    QDir().mkpath(objectsDir);
    const QString objectPath = objectsDir + QStringLiteral("/") + currentSha;

    if (QFileInfo::exists(objectPath)) {
        // Verify existing object matches expected SHA-256
        QString existingObjSha;
        if (computeFileSha256(objectPath, existingObjSha) && existingObjSha == currentSha) {
            return true;
        }
        outError = QString("Existing object in store failed SHA-256 verification: %1").arg(objectPath);
        return false;
    }

    // Step 4 & 5: Copy to temporary path in .desk32/tmp/ and re-verify SHA-256
    const QString tmpCowPath = wsRoot + QString("/.desk32/tmp/cow_%1_%2")
                                            .arg(QUuid::createUuid().toString(QUuid::Id128).left(10), currentSha);
    QString actualSha;
    QString copyErr;
    if (!copyFileReadOnlyVerified(absPath, tmpCowPath, currentSha, actualSha, copyErr)) {
        outError = QString("Copy-on-write evacuation failed (original edit/delete aborted): %1").arg(copyErr);
        return false;
    }

    if (triggerFaultIfSet(WorkspaceFaultPoint::BeforeCowCommit, relPath)) {
        QFile::remove(tmpCowPath);
        outError = QStringLiteral("Copy-on-write evacuation fault triggered prior to commit; original edit/delete aborted.");
        return false;
    }

    // Step 6: Commit to .desk32/objects/<sha256>
    std::error_code ec;
    std::filesystem::rename(tmpCowPath.toStdString(), objectPath.toStdString(), ec);
    if (ec) {
        QFile::remove(tmpCowPath);
        outError = QString("Copy-on-write atomic commit failed for %1: %2")
                       .arg(objectPath, QString::fromStdString(ec.message()));
        return false;
    }

    return true;
}

bool WorkspaceManager::editFileTags(const QString& relativePath, const metadata::TrackMetadata& updatedMeta, QString& outError) {
    return runOnWorkerThread([this, relativePath, updatedMeta, &outError]() -> bool {
        outError.clear();
        QString absPath, relPath;
        if (!resolveInsideOpenWorkspace(relativePath, false, absPath, relPath, outError)) {
            setLastError(outError);
            core::log::LogService::instance().log(
                "Workspace", "EditTags", "Failed", outError, "WorkspaceFile", relativePath,
                "{}", "", "", core::log::ErrorCode::WorkspaceContainmentViolation, 0.0);
            return false;
        }

        if (!checkRegularNoHardlink(absPath, outError)) {
            setLastError(outError);
            const int32_t code = outError.contains(QStringLiteral("Hard-linked"))
                                     ? core::log::ErrorCode::WorkspaceHardLinkRejected
                                     : core::log::ErrorCode::WorkspaceContainmentViolation;
            core::log::LogService::instance().log(
                "Workspace", "EditTags", "Failed", outError, "WorkspaceFile", relPath,
                "{}", "", "", code, 0.0);
            return false;
        }

        // Stage 2 hash check against state index & copy-on-write evacuation BEFORE modifying target!
        if (!evacuateCopyOnWriteLocked(absPath, relPath, outError)) {
            setLastError(outError);
            core::log::LogService::instance().log(
                "Workspace", "EditTags", "Failed", outError, "WorkspaceFile", relPath,
                "{}", "", "", core::log::ErrorCode::WorkspaceCowEvacuationFailed, 0.0);
            return false;
        }

        QString wsRoot;
        StopPlaybackCallback stopCb;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            wsRoot = m_openWorkspacePath;
            stopCb = m_stopPlaybackCallback;
        }

        if (stopCb) {
            stopCb(QStringList{absPath});
        }

        const QString fileName = QFileInfo(absPath).fileName();
        const QString tmpEditPath = wsRoot + QString("/.desk32/tmp/edittags_%1_%2")
                                                 .arg(QUuid::createUuid().toString(QUuid::Id128).left(10), fileName);

        QString copiedSha, copyErr;
        if (!copyFileReadOnlyVerified(absPath, tmpEditPath, QString(), copiedSha, copyErr)) {
            outError = QString("Failed to stage copy for tag edit: %1").arg(copyErr);
            setLastError(outError);
            return false;
        }

        std::string tagErr;
        const metadata::TagWriteStatus st = metadata::MetadataReader::writeUserTags(
            tmpEditPath.toStdString(), updatedMeta, tagErr);
        if (st != metadata::TagWriteStatus::Success) {
            QFile::remove(tmpEditPath);
            outError = QString("Tag editing failed: %1").arg(QString::fromStdString(tagErr));
            setLastError(outError);
            return false;
        }

        // Re-verify containment and st_nlink immediately prior to atomic replacement
        if (!resolveInsideOpenWorkspace(relPath, false, absPath, relPath, outError) ||
            !checkRegularNoHardlink(absPath, outError)) {
            QFile::remove(tmpEditPath);
            setLastError(outError);
            return false;
        }

        std::error_code ec;
        std::filesystem::rename(tmpEditPath.toStdString(), absPath.toStdString(), ec);
        if (ec) {
            QFile::remove(tmpEditPath);
            outError = QString("Failed to commit edited tags atomically to '%1': %2")
                           .arg(relPath, QString::fromStdString(ec.message()));
            setLastError(outError);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_externalChangeDetails.erase(relPath);
        }

        std::vector<WorkspaceFileEntry> current;
        scanCurrentWorkspaceLocked(current, false);
        saveStateIndexLocked(current);
        refreshWatcherLocked();

        core::log::LogService::instance().log(
            "Workspace", "EditTags", "Success",
            QString("Updated tags on '%1' (provenance preserved)").arg(relPath),
            "WorkspaceFile", relPath);
        emit workspaceContentsChanged();
        return true;
    });
}

bool WorkspaceManager::editFileTagsFromUi(const QString& relativePath, const QVariantMap& tagMap) {
    QString absP, relP, err;
    if (!resolveInsideOpenWorkspace(relativePath, false, absP, relP, err)) {
        setLastError(err);
        return false;
    }

    metadata::TrackMetadata meta = metadata::MetadataReader::parseTags(absP.toStdString());
    auto optStr = [&](const char* key, std::string& field) {
        if (tagMap.contains(key)) field = tagMap.value(key).toString().toStdString();
    };
    auto optInt = [&](const char* key, int32_t& field) {
        if (tagMap.contains(key)) field = tagMap.value(key).toInt();
    };

    optStr("title", meta.title);
    optStr("artist", meta.artist);
    optStr("album", meta.album);
    optStr("albumArtist", meta.albumArtist);
    optInt("trackNumber", meta.trackNumber);
    optInt("totalTracks", meta.totalTracks);
    optInt("discNumber", meta.discNumber);
    optInt("totalDiscs", meta.totalDiscs);
    optStr("date", meta.dateOrYear);
    optStr("dateOrYear", meta.dateOrYear);
    optStr("genre", meta.genre);
    optStr("composer", meta.composer);
    optStr("conductor", meta.conductor);
    optStr("performer", meta.performer);
    optStr("arranger", meta.arranger);
    optStr("label", meta.label);
    optStr("publisher", meta.publisher);
    optStr("catalogNumber", meta.catalogNumber);
    optStr("isrc", meta.isrc);
    optStr("comment", meta.comment);

    return editFileTags(relP, meta, err);
}

bool WorkspaceManager::renameWorkspaceFile(const QString& relativePath, const QString& newFileName, QString& outError) {
    return runOnWorkerThread([this, relativePath, newFileName, &outError]() -> bool {
        outError.clear();
        if (newFileName.isEmpty() || newFileName.contains('/') || newFileName.contains('\\') ||
            newFileName == QStringLiteral(".") || newFileName == QStringLiteral("..") ||
            isInternalDesk32EntryName(newFileName)) {
            outError = QString("Invalid destination file name: '%1'").arg(newFileName);
            setLastError(outError);
            return false;
        }

        QString srcAbs, srcRel;
        if (!resolveInsideOpenWorkspace(relativePath, false, srcAbs, srcRel, outError)) {
            setLastError(outError);
            return false;
        }

        if (!checkRegularNoHardlink(srcAbs, outError)) {
            setLastError(outError);
            const int32_t code = outError.contains(QStringLiteral("Hard-linked"))
                                     ? core::log::ErrorCode::WorkspaceHardLinkRejected
                                     : core::log::ErrorCode::WorkspaceContainmentViolation;
            core::log::LogService::instance().log(
                "Workspace", "RenameFile", "Failed", outError, "WorkspaceFile", srcRel,
                "{}", "", "", code, 0.0);
            return false;
        }

        const QFileInfo srcFi(srcRel);
        const QString parentRel = srcFi.path();
        const QString dstRelCandidate = (parentRel == QStringLiteral(".") || parentRel.isEmpty())
                                            ? newFileName
                                            : (parentRel + QStringLiteral("/") + newFileName);

        QString dstAbs, dstRel;
        if (!resolveInsideOpenWorkspace(dstRelCandidate, true, dstAbs, dstRel, outError)) {
            setLastError(outError);
            return false;
        }
        if (QFileInfo::exists(dstAbs)) {
            outError = QString("Cannot rename: destination already exists: %1").arg(dstRel);
            setLastError(outError);
            return false;
        }

        StopPlaybackCallback stopCb;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            stopCb = m_stopPlaybackCallback;
        }
        if (stopCb) {
            stopCb(QStringList{srcAbs});
        }

        std::error_code ec;
        std::filesystem::rename(srcAbs.toStdString(), dstAbs.toStdString(), ec);
        if (ec) {
            outError = QString("Rename failed: %1").arg(QString::fromStdString(ec.message()));
            setLastError(outError);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_externalChangeDetails.erase(srcRel);
            m_externalChangeDetails.erase(dstRel);
            if (m_logOnlyProvenanceRelPaths.erase(srcRel) > 0) {
                m_logOnlyProvenanceRelPaths.insert(dstRel);
            }
        }

        std::vector<WorkspaceFileEntry> current;
        scanCurrentWorkspaceLocked(current, false);
        saveStateIndexLocked(current);
        refreshWatcherLocked();

        core::log::LogService::instance().log(
            "Workspace", "RenameFile", "Success",
            QString("Renamed '%1' -> '%2'").arg(srcRel, dstRel),
            "WorkspaceFile", dstRel);
        emit workspaceContentsChanged();
        return true;
    });
}

bool WorkspaceManager::renameFileFromUi(const QString& relativePath, const QString& newFileName) {
    QString err;
    return renameWorkspaceFile(relativePath, newFileName, err);
}

bool WorkspaceManager::moveWorkspaceFile(const QString& relativePath, const QString& targetRelativeDir, QString& outError) {
    return runOnWorkerThread([this, relativePath, targetRelativeDir, &outError]() -> bool {
        outError.clear();
        QString srcAbs, srcRel;
        if (!resolveInsideOpenWorkspace(relativePath, false, srcAbs, srcRel, outError)) {
            setLastError(outError);
            return false;
        }

        if (!checkRegularNoHardlink(srcAbs, outError)) {
            setLastError(outError);
            const int32_t code = outError.contains(QStringLiteral("Hard-linked"))
                                     ? core::log::ErrorCode::WorkspaceHardLinkRejected
                                     : core::log::ErrorCode::WorkspaceContainmentViolation;
            core::log::LogService::instance().log(
                "Workspace", "MoveFile", "Failed", outError, "WorkspaceFile", srcRel,
                "{}", "", "", code, 0.0);
            return false;
        }

        QString cleanDir = targetRelativeDir.trimmed();
        while (cleanDir.startsWith('/')) cleanDir.remove(0, 1);
        while (cleanDir.endsWith('/')) cleanDir.chop(1);
        const QString baseName = QFileInfo(srcRel).fileName();
        const QString dstRelCandidate = cleanDir.isEmpty() ? baseName : (cleanDir + QStringLiteral("/") + baseName);

        QString dstAbs, dstRel;
        if (!resolveInsideOpenWorkspace(dstRelCandidate, true, dstAbs, dstRel, outError)) {
            setLastError(outError);
            return false;
        }
        if (QFileInfo::exists(dstAbs)) {
            outError = QString("Cannot move: destination already exists: %1").arg(dstRel);
            setLastError(outError);
            return false;
        }

        if (!QDir().mkpath(QFileInfo(dstAbs).absolutePath())) {
            outError = QString("Failed to create target directory for move: %1").arg(cleanDir);
            setLastError(outError);
            return false;
        }

        StopPlaybackCallback stopCb;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            stopCb = m_stopPlaybackCallback;
        }
        if (stopCb) {
            stopCb(QStringList{srcAbs});
        }

        std::error_code ec;
        std::filesystem::rename(srcAbs.toStdString(), dstAbs.toStdString(), ec);
        if (ec) {
            outError = QString("Move failed: %1").arg(QString::fromStdString(ec.message()));
            setLastError(outError);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_externalChangeDetails.erase(srcRel);
            m_externalChangeDetails.erase(dstRel);
            if (m_logOnlyProvenanceRelPaths.erase(srcRel) > 0) {
                m_logOnlyProvenanceRelPaths.insert(dstRel);
            }
        }

        std::vector<WorkspaceFileEntry> current;
        scanCurrentWorkspaceLocked(current, false);
        saveStateIndexLocked(current);
        refreshWatcherLocked();

        core::log::LogService::instance().log(
            "Workspace", "MoveFile", "Success",
            QString("Moved '%1' -> '%2'").arg(srcRel, dstRel),
            "WorkspaceFile", dstRel);
        emit workspaceContentsChanged();
        return true;
    });
}

bool WorkspaceManager::moveFileFromUi(const QString& relativePath, const QString& targetRelativeDir) {
    QString err;
    return moveWorkspaceFile(relativePath, targetRelativeDir, err);
}

bool WorkspaceManager::deleteWorkspaceFile(const QString& relativePath, QString& outError) {
    return runOnWorkerThread([this, relativePath, &outError]() -> bool {
        outError.clear();
        QString absPath, relPath;
        if (!resolveInsideOpenWorkspace(relativePath, false, absPath, relPath, outError)) {
            setLastError(outError);
            return false;
        }

        QString wsRoot;
        StopPlaybackCallback stopCb;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            wsRoot = m_openWorkspacePath;
            stopCb = m_stopPlaybackCallback;
        }

        if (!checkRegularNoHardlink(absPath, outError)) {
            setLastError(outError);
            const int32_t code = outError.contains(QStringLiteral("Hard-linked"))
                                     ? core::log::ErrorCode::WorkspaceHardLinkRejected
                                     : core::log::ErrorCode::WorkspaceContainmentViolation;
            core::log::LogService::instance().log(
                "Workspace", "DeleteFile", "Failed", outError, "WorkspaceFile", relPath,
                "{}", "", "", code, 0.0);
            return false;
        }

        if (stopCb) {
            stopCb(QStringList{absPath});
        }

        // Mandatory copy-on-write evacuation PRIOR to deletion
        if (!evacuateCopyOnWriteLocked(absPath, relPath, outError)) {
            setLastError(outError);
            core::log::LogService::instance().log(
                "Workspace", "DeleteFile", "Failed", outError, "WorkspaceFile", relPath,
                "{}", "", "", core::log::ErrorCode::WorkspaceCowEvacuationFailed, 0.0);
            return false;
        }

        if (!QFile::remove(absPath)) {
            outError = QString("Failed to delete workspace file: %1").arg(relPath);
            setLastError(outError);
            return false;
        }
        removeEmptyParentDirsUpTo(wsRoot, QFileInfo(absPath).absolutePath());

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_externalChangeDetails.erase(relPath);
            m_logOnlyProvenanceRelPaths.erase(relPath);
        }

        std::vector<WorkspaceFileEntry> current;
        scanCurrentWorkspaceLocked(current, false);
        saveStateIndexLocked(current);
        refreshWatcherLocked();

        core::log::LogService::instance().log(
            "Workspace", "DeleteFile", "Success",
            QString("Deleted workspace file '%1'").arg(relPath),
            "WorkspaceFile", relPath);
        emit workspaceContentsChanged();
        return true;
    });
}

bool WorkspaceManager::deleteFileFromUi(const QString& relativePath) {
    QString err;
    return deleteWorkspaceFile(relativePath, err);
}

bool WorkspaceManager::loadSaveByIdLocked(const QString& saveId, SaveManifest& outManifest, QString& outError) const {
    outError.clear();
    QString wsRoot;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        wsRoot = m_openWorkspacePath;
    }
    if (wsRoot.isEmpty()) {
        outError = QStringLiteral("No open workspace.");
        return false;
    }
    if (saveId.contains('/') || saveId.contains("..")) {
        outError = QStringLiteral("Invalid save identifier.");
        return false;
    }

    const QString path = wsRoot + QStringLiteral("/.desk32/saves/") + saveId + QStringLiteral(".json");
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        outError = QString("Save manifest not found: %1").arg(saveId);
        return false;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        outError = QString("Corrupt save manifest JSON for save '%1'").arg(saveId);
        return false;
    }

    const QJsonObject obj = doc.object();
    outManifest = SaveManifest{};
    outManifest.formatVersion = obj.value(QStringLiteral("format_version")).toInt(1);
    outManifest.saveId = obj.value(QStringLiteral("save_id")).toString(saveId);
    outManifest.name = obj.value(QStringLiteral("name")).toString();
    outManifest.createdUtc = obj.value(QStringLiteral("created_utc")).toString();
    outManifest.localUtcOffset = obj.value(QStringLiteral("local_utc_offset")).toString();
    outManifest.isAutoSave = obj.value(QStringLiteral("auto_save")).toBool(false);

    const QJsonArray filesArr = obj.value(QStringLiteral("files")).toArray();
    for (const QJsonValue& v : filesArr) {
        if (!v.isObject()) continue;
        const QJsonObject fo = v.toObject();
        SaveManifestFile mf;
        mf.relativePath = fo.value(QStringLiteral("relative_path")).toString();
        mf.sha256 = fo.value(QStringLiteral("sha256")).toString();
        mf.size = fo.value(QStringLiteral("size")).toInteger();
        mf.mtimeNs = fo.value(QStringLiteral("mtime_ns")).toInteger();
        const QJsonObject tagsObj = fo.value(QStringLiteral("tags")).toObject();
        for (auto it = tagsObj.begin(); it != tagsObj.end(); ++it) {
            mf.tags[it.key().toStdString()] = it.value().toString().toStdString();
        }
        outManifest.files.push_back(std::move(mf));
    }
    return true;
}

bool WorkspaceManager::loadAllSavesLocked(std::vector<SaveManifest>& outSaves) const {
    outSaves.clear();
    QString wsRoot;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        wsRoot = m_openWorkspacePath;
    }
    if (wsRoot.isEmpty()) return false;

    const QDir savesDir(wsRoot + QStringLiteral("/.desk32/saves"));
    if (!savesDir.exists()) return true;

    const QFileInfoList manifests = savesDir.entryInfoList(QStringList() << QStringLiteral("*.json"), QDir::Files, QDir::Name);
    for (const QFileInfo& fi : manifests) {
        SaveManifest m;
        QString err;
        if (loadSaveByIdLocked(fi.completeBaseName(), m, err)) {
            outSaves.push_back(std::move(m));
        }
    }

    std::sort(outSaves.begin(), outSaves.end(), [](const SaveManifest& a, const SaveManifest& b) {
        if (a.createdUtc != b.createdUtc) return a.createdUtc < b.createdUtc;
        return a.saveId < b.saveId;
    });

    // Calculate chronological incremental bytes (unique new entity sizes introduced by each save)
    std::set<QString> seenHashes;
    for (auto& s : outSaves) {
        qint64 inc = 0;
        std::set<QString> newInSave;
        for (const auto& f : s.files) {
            if (!seenHashes.count(f.sha256) && !newInSave.count(f.sha256)) {
                newInSave.insert(f.sha256);
                seenHashes.insert(f.sha256);
                inc += f.size;
            }
        }
        s.incrementalBytes = inc;
    }
    return true;
}

bool WorkspaceManager::writeSaveManifestLocked(const SaveManifest& manifest, QString& outError) const {
    QString wsRoot;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        wsRoot = m_openWorkspacePath;
    }
    if (wsRoot.isEmpty()) {
        outError = QStringLiteral("No open workspace.");
        return false;
    }

    QJsonArray filesArr;
    for (const auto& f : manifest.files) {
        QJsonObject fo;
        fo.insert(QStringLiteral("relative_path"), f.relativePath);
        fo.insert(QStringLiteral("sha256"), f.sha256);
        fo.insert(QStringLiteral("size"), f.size);
        fo.insert(QStringLiteral("mtime_ns"), f.mtimeNs);
        QJsonObject tagsObj;
        for (const auto& [k, v] : f.tags) {
            tagsObj.insert(QString::fromStdString(k), QString::fromStdString(v));
        }
        fo.insert(QStringLiteral("tags"), tagsObj);
        filesArr.append(fo);
    }

    QJsonObject root;
    root.insert(QStringLiteral("format_version"), manifest.formatVersion);
    root.insert(QStringLiteral("save_id"), manifest.saveId);
    root.insert(QStringLiteral("name"), manifest.name);
    root.insert(QStringLiteral("created_utc"), manifest.createdUtc);
    root.insert(QStringLiteral("local_utc_offset"), manifest.localUtcOffset);
    root.insert(QStringLiteral("auto_save"), manifest.isAutoSave);
    root.insert(QStringLiteral("files"), filesArr);

    const QString savesDir = wsRoot + QStringLiteral("/.desk32/saves");
    QDir().mkpath(savesDir);
    const QString tmpPath = wsRoot + QString("/.desk32/tmp/manifest_%1.json").arg(manifest.saveId);
    const QString finalPath = savesDir + QStringLiteral("/") + manifest.saveId + QStringLiteral(".json");

    QFile tf(tmpPath);
    if (!tf.open(QIODevice::WriteOnly)) {
        outError = QString("Failed to write save manifest temp file (check permissions/disk space): %1").arg(tf.errorString());
        return false;
    }
    const QByteArray jsonBytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (tf.write(jsonBytes) != jsonBytes.size() || !tf.flush()) {
        tf.close();
        QFile::remove(tmpPath);
        outError = QStringLiteral("Failed to write full save manifest bytes (possible disk space issue).");
        return false;
    }
    ::fsync(tf.handle());
    tf.close();

    std::error_code ec;
    std::filesystem::rename(tmpPath.toStdString(), finalPath.toStdString(), ec);
    if (ec) {
        QFile::remove(tmpPath);
        outError = QString("Failed to commit save manifest '%1': %2")
                       .arg(finalPath, QString::fromStdString(ec.message()));
        return false;
    }
    return true;
}

bool WorkspaceManager::createSave(const QString& saveName, QString& outSaveId, QString& outError, bool isAutoSave) {
    return runOnWorkerThread([this, saveName, &outSaveId, &outError, isAutoSave]() -> bool {
        outSaveId.clear();
        outError.clear();

        if (!hasOpenWorkspace()) {
            outError = QStringLiteral("Cannot create save: no workspace is open.");
            setLastError(outError);
            return false;
        }

        std::vector<WorkspaceFileEntry> pass1;
        scanCurrentWorkspaceLocked(pass1, true);

        if (triggerFaultIfSet(WorkspaceFaultPoint::DuringSaveScan, saveName)) {
            outError = QStringLiteral("File changed externally during save creation; save aborted without writing manifest.");
            setLastError(outError);
            core::log::LogService::instance().log(
                "Workspace", "CreateSave", "Failed", outError, "WorkspaceSave", saveName,
                "{}", "", "", core::log::ErrorCode::WorkspaceSaveAbortedOnChange, 0.0);
            return false;
        }

        // Second pass hash verification to detect any concurrent external modification while building the save
        std::vector<WorkspaceFileEntry> pass2;
        scanCurrentWorkspaceLocked(pass2, false);

        if (pass1.size() != pass2.size()) {
            outError = QStringLiteral("Workspace file count changed during save creation; save aborted.");
            setLastError(outError);
            core::log::LogService::instance().log(
                "Workspace", "CreateSave", "Failed", outError, "WorkspaceSave", saveName,
                "{}", "", "", core::log::ErrorCode::WorkspaceSaveAbortedOnChange, 0.0);
            return false;
        }
        for (size_t i = 0; i < pass1.size(); ++i) {
            if (pass1[i].relativePath != pass2[i].relativePath ||
                pass1[i].sha256 != pass2[i].sha256 ||
                pass1[i].size != pass2[i].size) {
                outError = QString("Concurrent modification detected on '%1' while creating save; save aborted.")
                               .arg(pass1[i].relativePath);
                setLastError(outError);
                core::log::LogService::instance().log(
                    "Workspace", "CreateSave", "Failed", outError, "WorkspaceFile", pass1[i].relativePath,
                    "{}", "", "", core::log::ErrorCode::WorkspaceSaveAbortedOnChange, 0.0);
                return false;
            }
        }

        SaveManifest m;
        m.formatVersion = 1;
        m.createdUtc = utcIsoNow();
        m.localUtcOffset = localOffsetStringNow();
        m.isAutoSave = isAutoSave;
        m.saveId = QString("save_%1_%2")
                       .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")),
                            QUuid::createUuid().toString(QUuid::Id128).left(8));
        m.name = saveName.trimmed().isEmpty()
                     ? QString("Checkpoint %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                     : saveName.trimmed();

        for (const auto& e : pass1) {
            SaveManifestFile mf;
            mf.relativePath = e.relativePath;
            mf.sha256 = e.sha256;
            mf.size = e.size;
            mf.mtimeNs = e.mtimeNs;
            mf.tags = e.metadata.canonicalTagMap();
            m.files.push_back(std::move(mf));
        }

        if (!writeSaveManifestLocked(m, outError)) {
            setLastError(outError);
            return false;
        }

        saveStateIndexLocked(pass1);
        outSaveId = m.saveId;

        core::log::LogService::instance().log(
            "Workspace", "CreateSave", "Success",
            QString("Created save '%1' (%2 files, id=%3)").arg(m.name).arg(m.files.size()).arg(m.saveId),
            "WorkspaceSave", m.saveId);
        emit workspaceContentsChanged();
        return true;
    });
}

QString WorkspaceManager::createSaveFromUi(const QString& saveName) {
    QString id, err;
    createSave(saveName, id, err, false);
    return id;
}

std::vector<SaveManifest> WorkspaceManager::listSaves() const {
    if (!hasOpenWorkspace()) return {};
    return runOnWorkerThread([this]() -> std::vector<SaveManifest> {
        std::vector<SaveManifest> out;
        loadAllSavesLocked(out);
        return out;
    });
}

qint64 WorkspaceManager::saveStoreTotalBytes() const {
    QString wsRoot;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_readOnlyMode || m_openWorkspacePath.isEmpty()) return 0;
        wsRoot = m_openWorkspacePath;
    }
    qint64 total = 0;
    const QDir objDir(wsRoot + QStringLiteral("/.desk32/objects"));
    if (objDir.exists()) {
        for (const QFileInfo& fi : objDir.entryInfoList(QDir::Files)) {
            total += fi.size();
        }
    }
    return total;
}

QVariantList WorkspaceManager::getSaves() const {
    QVariantList list;
    for (const auto& s : listSaves()) {
        QVariantMap m;
        m["saveId"] = s.saveId;
        m["name"] = s.name;
        m["createdUtc"] = s.createdUtc;
        m["localUtcOffset"] = s.localUtcOffset;
        m["isAutoSave"] = s.isAutoSave;
        m["fileCount"] = static_cast<int>(s.files.size());
        m["incrementalBytes"] = s.incrementalBytes;
        QStringList relPaths;
        for (const auto& f : s.files) relPaths.append(f.relativePath);
        m["relativePaths"] = relPaths;
        list.append(m);
    }
    return list;
}

SaveComparisonResult WorkspaceManager::compareSaves(const QString& fromSaveId, const QString& toSaveId) const {
    return runOnWorkerThread([this, fromSaveId, toSaveId]() -> SaveComparisonResult {
        SaveComparisonResult res;
        res.fromSaveId = fromSaveId;
        res.toSaveId = toSaveId;

        if (!hasOpenWorkspace()) {
            res.errorMessage = QStringLiteral("No open workspace.");
            return res;
        }

        SaveManifest a, b;
        if (!loadSaveByIdLocked(fromSaveId, a, res.errorMessage) ||
            !loadSaveByIdLocked(toSaveId, b, res.errorMessage)) {
            return res;
        }

        std::map<QString, const SaveManifestFile*> mapA, mapB;
        for (const auto& f : a.files) mapA[f.relativePath] = &f;
        for (const auto& f : b.files) mapB[f.relativePath] = &f;

        std::set<QString> allPaths;
        for (const auto& [p, _] : mapA) allPaths.insert(p);
        for (const auto& [p, _] : mapB) allPaths.insert(p);

        for (const QString& p : allPaths) {
            auto itA = mapA.find(p);
            auto itB = mapB.find(p);
            if (itA == mapA.end()) {
                SaveFileDiffEntry d;
                d.relativePath = p;
                d.changeType = QStringLiteral("Added");
                d.afterSha256 = itB->second->sha256;
                d.afterSize = itB->second->size;
                res.addedCount++;
                res.diffs.push_back(std::move(d));
            } else if (itB == mapB.end()) {
                SaveFileDiffEntry d;
                d.relativePath = p;
                d.changeType = QStringLiteral("Removed");
                d.beforeSha256 = itA->second->sha256;
                d.beforeSize = itA->second->size;
                res.removedCount++;
                res.diffs.push_back(std::move(d));
            } else if (itA->second->sha256 != itB->second->sha256) {
                SaveFileDiffEntry d;
                d.relativePath = p;
                d.changeType = QStringLiteral("Modified");
                d.beforeSha256 = itA->second->sha256;
                d.afterSha256 = itB->second->sha256;
                d.beforeSize = itA->second->size;
                d.afterSize = itB->second->size;

                std::set<std::string> tagKeys;
                for (const auto& [k, _] : itA->second->tags) tagKeys.insert(k);
                for (const auto& [k, _] : itB->second->tags) tagKeys.insert(k);
                for (const auto& k : tagKeys) {
                    const auto tA = itA->second->tags.find(k);
                    const auto tB = itB->second->tags.find(k);
                    const std::string valA = (tA != itA->second->tags.end()) ? tA->second : std::string();
                    const std::string valB = (tB != itB->second->tags.end()) ? tB->second : std::string();
                    if (valA != valB) {
                        SaveTagDiffEntry td;
                        td.key = QString::fromStdString(k);
                        td.beforeValue = QString::fromStdString(valA);
                        td.afterValue = QString::fromStdString(valB);
                        d.tagDiffs.push_back(std::move(td));
                    }
                }
                res.modifiedCount++;
                res.diffs.push_back(std::move(d));
            } else {
                res.unchangedCount++;
            }
        }

        res.valid = true;
        return res;
    });
}

QVariantMap WorkspaceManager::compareSavesFromUi(const QString& fromSaveId, const QString& toSaveId) const {
    const SaveComparisonResult r = compareSaves(fromSaveId, toSaveId);
    QVariantMap m;
    m["valid"] = r.valid;
    m["errorMessage"] = r.errorMessage;
    m["fromSaveId"] = r.fromSaveId;
    m["toSaveId"] = r.toSaveId;
    m["addedCount"] = r.addedCount;
    m["removedCount"] = r.removedCount;
    m["modifiedCount"] = r.modifiedCount;
    m["unchangedCount"] = r.unchangedCount;
    QVariantList diffs;
    for (const auto& d : r.diffs) {
        QVariantMap dm;
        dm["relativePath"] = d.relativePath;
        dm["changeType"] = d.changeType;
        dm["beforeSha256"] = d.beforeSha256;
        dm["afterSha256"] = d.afterSha256;
        dm["beforeSize"] = d.beforeSize;
        dm["afterSize"] = d.afterSize;
        QVariantList tdiffs;
        for (const auto& td : d.tagDiffs) {
            QVariantMap tdm;
            tdm["key"] = td.key;
            tdm["beforeValue"] = td.beforeValue;
            tdm["afterValue"] = td.afterValue;
            tdiffs.append(tdm);
        }
        dm["tagDiffs"] = tdiffs;
        diffs.append(dm);
    }
    m["diffs"] = diffs;
    return m;
}

bool WorkspaceManager::resolveSavedTrackForAudition(const QString& saveId, const QString& relativePath, QString& outFilePath, QString& outError) {
    return runOnWorkerThread([this, saveId, relativePath, &outFilePath, &outError]() -> bool {
        outFilePath.clear();
        outError.clear();

        QString wsRoot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_readOnlyMode || m_openWorkspacePath.isEmpty() || m_lockFd < 0) {
                outError = QStringLiteral("No open workspace.");
                return false;
            }
            wsRoot = m_openWorkspacePath;
        }

        SaveManifest m;
        if (!loadSaveByIdLocked(saveId, m, outError)) return false;

        const SaveManifestFile* targetEntry = nullptr;
        for (const auto& f : m.files) {
            if (f.relativePath == relativePath) {
                targetEntry = &f;
                break;
            }
        }
        if (!targetEntry) {
            outError = QString("File '%1' does not exist in save '%2'.").arg(relativePath, saveId);
            return false;
        }

        const QString objPath = wsRoot + QStringLiteral("/.desk32/objects/") + targetEntry->sha256;
        const QString livePath = wsRoot + QStringLiteral("/") + targetEntry->relativePath;

        QString sourceToUse;
        QString liveSha;
        if (QFileInfo::exists(objPath)) {
            QString objSha;
            if (!computeFileSha256(objPath, objSha) || objSha != targetEntry->sha256) {
                outError = QString("Saved object '%1' is corrupt (SHA-256 mismatch).").arg(targetEntry->sha256);
                return false;
            }
            sourceToUse = objPath;
        } else if (computeFileSha256(livePath, liveSha) && liveSha == targetEntry->sha256) {
            outFilePath = livePath;
            return true;
        } else {
            outError = QString("Saved content '%1' is unavailable (external change or missing object).").arg(targetEntry->sha256);
            return false;
        }

        // Prepare a temporary read-only preview with original filename extension for decoder/TagLib auditioning
        const QString previewPath = wsRoot + QString("/.desk32/tmp/audition_%1_%2")
                                                 .arg(targetEntry->sha256.left(12), QFileInfo(relativePath).fileName());
        QString existingPreviewSha;
        if (computeFileSha256(previewPath, existingPreviewSha) && existingPreviewSha == targetEntry->sha256) {
            outFilePath = previewPath;
            return true;
        }
        QFile::remove(previewPath);
        QString actualSha;
        if (!copyFileReadOnlyVerified(sourceToUse, previewPath, targetEntry->sha256, actualSha, outError)) {
            return false;
        }
        outFilePath = previewPath;
        return true;
    });
}

QString WorkspaceManager::prepareSavedTrackForAuditionFromUi(const QString& saveId, const QString& relativePath) {
    QString path, err;
    if (!resolveSavedTrackForAudition(saveId, relativePath, path, err)) {
        setLastError(err);
        return QString();
    }
    return path;
}

RollbackResult WorkspaceManager::adoptSave(const QString& saveId) {
    return runOnWorkerThread([this, saveId]() -> RollbackResult {
        RollbackResult res;
        QString wsRoot;
        StopPlaybackCallback stopCb;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_readOnlyMode || m_openWorkspacePath.isEmpty() || m_lockFd < 0) {
                res.errorMessage = QStringLiteral("Cannot adopt save: no workspace is open.");
                m_lastError = res.errorMessage;
                return res;
            }
            wsRoot = m_openWorkspacePath;
            stopCb = m_stopPlaybackCallback;
        }

        // Step 1: Load & pre-verify target save manifest and all required object payloads
        SaveManifest targetSave;
        if (!loadSaveByIdLocked(saveId, targetSave, res.errorMessage)) {
            setLastError(res.errorMessage);
            return res;
        }

        std::vector<WorkspaceFileEntry> currentEntries;
        scanCurrentWorkspaceLocked(currentEntries, false);
        std::map<QString, WorkspaceFileEntry> currentMap;
        std::map<QString, QString> currentBySha256;
        for (const auto& e : currentEntries) {
            currentMap[e.relativePath] = e;
            if (!e.sha256.isEmpty() && !currentBySha256.count(e.sha256)) {
                currentBySha256[e.sha256] = e.absolutePath;
            }
        }

        struct RestoreSource {
            SaveManifestFile entry;
            QString sourceFile; // either live file (even if renamed/moved) or .desk32/objects/<sha>
        };
        std::vector<RestoreSource> restorePlan;
        qint64 neededStagingBytes = 0;

        for (const auto& f : targetSave.files) {
            QString absP, relP, err;
            if (!resolveInsideOpenWorkspace(f.relativePath, true, absP, relP, err)) {
                res.errorMessage = err;
                setLastError(err);
                return res;
            }
            const QString objPath = wsRoot + QStringLiteral("/.desk32/objects/") + f.sha256;
            auto curIt = currentMap.find(f.relativePath);
            auto curShaIt = currentBySha256.find(f.sha256);
            if (QFileInfo::exists(objPath)) {
                QString objSha;
                qint64 objSize = 0;
                if (!computeFileSha256(objPath, objSha, &objSize) || objSha != f.sha256 || objSize != f.size) {
                    res.errorMessage = QString("External modification or corruption of saved object for '%1' (%2)").arg(f.relativePath, f.sha256);
                    setLastError(res.errorMessage);
                    core::log::LogService::instance().log(
                        "Workspace", "AdoptSave", "Failed", res.errorMessage, "WorkspaceSave", saveId,
                        "{}", "", "", core::log::ErrorCode::WorkspaceExternalModification, 0.0);
                    return res;
                }
                restorePlan.push_back({f, objPath});
                if (curIt == currentMap.end() || curIt->second.sha256 != f.sha256) {
                    neededStagingBytes += f.size;
                }
            } else if (curIt != currentMap.end() && curIt->second.sha256 == f.sha256) {
                restorePlan.push_back({f, curIt->second.absolutePath});
            } else if (curShaIt != currentBySha256.end()) {
                // File was moved or renamed in-place without content change
                restorePlan.push_back({f, curShaIt->second});
                neededStagingBytes += f.size;
            } else {
                res.errorMessage = QString("Cannot rollback: saved version of '%1' (SHA-256 %2) is missing or externally altered.")
                                       .arg(f.relativePath, f.sha256);
                setLastError(res.errorMessage);
                core::log::LogService::instance().log(
                    "Workspace", "AdoptSave", "Failed", res.errorMessage, "WorkspaceSave", saveId,
                    "{}", "", "", core::log::ErrorCode::WorkspaceExternalModification, 0.0);
                return res;
            }
        }

        // Check available disk space before making changes
        QStorageInfo storage(wsRoot);
        if (storage.isValid() && storage.bytesAvailable() >= 0 &&
            storage.bytesAvailable() < neededStagingBytes + 4096) {
            res.errorMessage = QString("Insufficient disk space for rollback (need %1 bytes, available %2 bytes).")
                                   .arg(neededStagingBytes)
                                   .arg(storage.bytesAvailable());
            setLastError(res.errorMessage);
            return res;
        }

        // Step 2: Stop playback/unload decks holding workspace files prior to file replacement
        if (stopCb) {
            QStringList affected;
            for (const auto& e : currentEntries) affected.append(e.absolutePath);
            for (const auto& f : targetSave.files) affected.append(wsRoot + "/" + f.relativePath);
            stopCb(affected);
        }

        // Step 3: Auto-save current state unless identical to an existing save, and materialize objects for overwritten files
        std::vector<SaveManifest> allSaves;
        loadAllSavesLocked(allSaves);
        auto matchesExistingSave = [&](const SaveManifest& existing) -> bool {
            if (existing.files.size() != currentEntries.size()) return false;
            std::map<QString, QString> existingMap;
            for (const auto& f : existing.files) existingMap[f.relativePath] = f.sha256;
            for (const auto& e : currentEntries) {
                auto it = existingMap.find(e.relativePath);
                if (it == existingMap.end() || it->second != e.sha256) return false;
            }
            return true;
        };

        const bool identicalSaveExists = std::any_of(allSaves.begin(), allSaves.end(), matchesExistingSave);
        if (!identicalSaveExists) {
            QString autoId, autoErr;
            if (!createSave(QString("Auto-save before rollback to %1").arg(targetSave.name), autoId, autoErr, true)) {
                res.errorMessage = QString("Auto-save prior to rollback failed: %1").arg(autoErr);
                setLastError(res.errorMessage);
                return res;
            }
            res.autoSaveId = autoId;
        }

        // Evacuate all current files via COW so every saved reference (including the auto-save) is preserved safely
        for (const auto& e : currentEntries) {
            QString cowErr;
            if (!evacuateCopyOnWriteLocked(e.absolutePath, e.relativePath, cowErr)) {
                res.errorMessage = QString("Pre-rollback copy-on-write evacuation failed for '%1': %2").arg(e.relativePath, cowErr);
                setLastError(res.errorMessage);
                return res;
            }
        }

        // Step 4: Prepare staging & undo directories in .desk32/tmp/
        const QString opTag = QUuid::createUuid().toString(QUuid::Id128).left(10);
        const QString targetStagingRoot = wsRoot + QString("/.desk32/tmp/rollback_tgt_%1").arg(opTag);
        const QString undoStagingRoot = wsRoot + QString("/.desk32/tmp/rollback_undo_%1").arg(opTag);
        QDir().mkpath(targetStagingRoot);
        QDir().mkpath(undoStagingRoot);

        auto cleanupStaging = [&]() {
            QDir(targetStagingRoot).removeRecursively();
            QDir(undoStagingRoot).removeRecursively();
        };

        // Back up every current workspace file to undoStagingRoot
        struct UndoFile {
            QString relativePath;
            QString backupAbsPath;
            QString sha256;
        };
        std::vector<UndoFile> undoPlan;
        for (const auto& e : currentEntries) {
            const QString bkpPath = undoStagingRoot + QStringLiteral("/") + e.relativePath;
            QDir().mkpath(QFileInfo(bkpPath).absolutePath());
            QString bkpSha, bkpErr;
            if (!copyFileReadOnlyVerified(e.absolutePath, bkpPath, e.sha256, bkpSha, bkpErr)) {
                cleanupStaging();
                res.errorMessage = QString("Failed to prepare rollback undo backup for '%1': %2").arg(e.relativePath, bkpErr);
                setLastError(res.errorMessage);
                return res;
            }
            undoPlan.push_back({e.relativePath, bkpPath, e.sha256});
        }

        // Stage target files to targetStagingRoot
        struct StagedTargetFile {
            QString relativePath;
            QString stagedAbsPath;
            QString destAbsPath;
            QString expectedSha256;
        };
        std::vector<StagedTargetFile> stagedTargets;
        for (const auto& rs : restorePlan) {
            const QString stgPath = targetStagingRoot + QStringLiteral("/") + rs.entry.relativePath;
            QDir().mkpath(QFileInfo(stgPath).absolutePath());
            QString stgSha, stgErr;
            if (!copyFileReadOnlyVerified(rs.sourceFile, stgPath, rs.entry.sha256, stgSha, stgErr)) {
                cleanupStaging();
                res.errorMessage = QString("Failed to prepare target file '%1' in rollback staging: %2").arg(rs.entry.relativePath, stgErr);
                setLastError(res.errorMessage);
                return res;
            }
            stagedTargets.push_back({
                rs.entry.relativePath,
                stgPath,
                wsRoot + QStringLiteral("/") + rs.entry.relativePath,
                rs.entry.sha256
            });
        }

        if (triggerFaultIfSet(WorkspaceFaultPoint::DuringRollbackPrepare, saveId)) {
            cleanupStaging();
            res.errorMessage = QStringLiteral("Rollback aborted during staging preparation (pre-apply; workspace untouched).");
            setLastError(res.errorMessage);
            return res;
        }

        // Step 5: Apply changes to workspace
        bool applyFailed = false;
        QString applyErr;

        std::set<QString> targetRelSet;
        for (const auto& st : stagedTargets) targetRelSet.insert(st.relativePath);

        // Remove files not in target save
        for (const auto& e : currentEntries) {
            if (!targetRelSet.count(e.relativePath)) {
                if (triggerFaultIfSet(WorkspaceFaultPoint::DuringRollbackApply, e.relativePath)) {
                    applyFailed = true;
                    applyErr = QString("Fault injected while removing '%1' during rollback apply.").arg(e.relativePath);
                    break;
                }
                if (!QFile::remove(e.absolutePath)) {
                    applyFailed = true;
                    applyErr = QString("Failed to remove '%1' during rollback apply.").arg(e.relativePath);
                    break;
                }
                removeEmptyParentDirsUpTo(wsRoot, QFileInfo(e.absolutePath).absolutePath());
            }
        }

        // Place target save files
        if (!applyFailed) {
            for (const auto& st : stagedTargets) {
                QString destAbs, destRel, contErr;
                if (!resolveInsideOpenWorkspace(st.relativePath, true, destAbs, destRel, contErr)) {
                    applyFailed = true;
                    applyErr = contErr;
                    break;
                }
                if (QFileInfo::exists(destAbs)) {
                    QString hlErr;
                    if (!checkRegularNoHardlink(destAbs, hlErr)) {
                        applyFailed = true;
                        applyErr = hlErr;
                        break;
                    }
                }
                if (!QDir().mkpath(QFileInfo(destAbs).absolutePath())) {
                    applyFailed = true;
                    applyErr = QString("Failed to create directory for '%1'").arg(st.relativePath);
                    break;
                }
                if (triggerFaultIfSet(WorkspaceFaultPoint::DuringRollbackApply, st.relativePath)) {
                    applyFailed = true;
                    applyErr = QString("Fault injected while writing '%1' during rollback apply.").arg(st.relativePath);
                    break;
                }
                std::error_code ec;
                std::filesystem::rename(st.stagedAbsPath.toStdString(), destAbs.toStdString(), ec);
                if (ec) {
                    applyFailed = true;
                    applyErr = QString("Failed to commit rolled-back file '%1': %2")
                                   .arg(st.relativePath, QString::fromStdString(ec.message()));
                    break;
                }
            }
        }

        if (!applyFailed) {
            cleanupStaging();
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_externalChangeDetails.clear();
                m_externalModificationCount = 0;
            }
            std::vector<WorkspaceFileEntry> finalEntries;
            scanCurrentWorkspaceLocked(finalEntries, false);
            saveStateIndexLocked(finalEntries);
            refreshWatcherLocked();

            res.success = true;
            core::log::LogService::instance().log(
                "Workspace", "AdoptSave", "Success",
                QString("Adopted save '%1' (%2) cleanly.").arg(targetSave.name, saveId),
                "WorkspaceSave", saveId);
            emit workspaceContentsChanged();
            return res;
        }

        // Step 6: Failure recovery -> restore workspace back to pre-rollback state (undoPlan)
        bool allRecovered = true;
        std::vector<WorkspaceFileEntry> postFailureEntries;
        scanCurrentWorkspaceLocked(postFailureEntries, false);
        std::set<QString> undoRelSet;
        for (const auto& u : undoPlan) undoRelSet.insert(u.relativePath);

        for (const auto& e : postFailureEntries) {
            if (!undoRelSet.count(e.relativePath)) {
                if (triggerFaultIfSet(WorkspaceFaultPoint::DuringRollbackRecover, e.relativePath) ||
                    !QFile::remove(e.absolutePath)) {
                    allRecovered = false;
                    res.failedPaths.append(e.relativePath + QStringLiteral(" (failed to remove extra post-failure file)"));
                } else {
                    removeEmptyParentDirsUpTo(wsRoot, QFileInfo(e.absolutePath).absolutePath());
                }
            }
        }

        for (const auto& u : undoPlan) {
            const QString destAbs = wsRoot + QStringLiteral("/") + u.relativePath;
            QString curSha;
            if (computeFileSha256(destAbs, curSha) && curSha == u.sha256) {
                continue;
            }
            if (triggerFaultIfSet(WorkspaceFaultPoint::DuringRollbackRecover, u.relativePath)) {
                allRecovered = false;
                res.failedPaths.append(u.relativePath + QStringLiteral(" (recovery write failed)"));
                continue;
            }
            QDir().mkpath(QFileInfo(destAbs).absolutePath());
            QString recSha, recErr;
            QFile::remove(destAbs);
            if (!copyFileReadOnlyVerified(u.backupAbsPath, destAbs, u.sha256, recSha, recErr)) {
                allRecovered = false;
                res.failedPaths.append(u.relativePath + QString(" (%1)").arg(recErr));
            }
        }

        cleanupStaging();
        std::vector<WorkspaceFileEntry> afterRecoverEntries;
        scanCurrentWorkspaceLocked(afterRecoverEntries, false);
        saveStateIndexLocked(afterRecoverEntries);
        refreshWatcherLocked();

        if (allRecovered) {
            res.success = false;
            res.partialRecovery = false;
            res.errorMessage = QString("Rollback failed (%1); workspace safely restored to pre-rollback state.").arg(applyErr);
            setLastError(res.errorMessage);
            core::log::LogService::instance().log(
                "Workspace", "AdoptSave", "Failed", res.errorMessage, "WorkspaceSave", saveId,
                "{}", "", "", core::log::ErrorCode::WorkspaceRollbackFailed, 0.0);
        } else {
            res.success = false;
            res.partialRecovery = true;
            res.errorMessage = QString("Partial recovery after rollback failure (%1); some files could not be restored: %2")
                                   .arg(applyErr, res.failedPaths.join(QStringLiteral(", ")));
            setLastError(res.errorMessage);
            QJsonObject det;
            QJsonArray fp;
            for (const QString& p : res.failedPaths) fp.append(p);
            det.insert(QStringLiteral("apply_error"), applyErr);
            det.insert(QStringLiteral("failed_paths"), fp);
            det.insert(QStringLiteral("partial_recovery"), true);
            core::log::LogService::instance().log(
                "Workspace", "AdoptSave", "Failed", res.errorMessage, "WorkspaceSave", saveId,
                QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
                "", "", core::log::ErrorCode::WorkspacePartialRecovery, 0.0);
        }
        emit workspaceContentsChanged();
        return res;
    });
}

QVariantMap WorkspaceManager::adoptSaveFromUi(const QString& saveId) {
    const RollbackResult r = adoptSave(saveId);
    QVariantMap m;
    m["success"] = r.success;
    m["partialRecovery"] = r.partialRecovery;
    m["autoSaveId"] = r.autoSaveId;
    m["errorMessage"] = r.errorMessage;
    m["failedPaths"] = r.failedPaths;
    return m;
}

bool WorkspaceManager::deleteSave(const QString& saveId, QString& outError) {
    return runOnWorkerThread([this, saveId, &outError]() -> bool {
        outError.clear();
        QString wsRoot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_readOnlyMode || m_openWorkspacePath.isEmpty() || m_lockFd < 0) {
                outError = QStringLiteral("No open workspace.");
                m_lastError = outError;
                return false;
            }
            wsRoot = m_openWorkspacePath;
        }

        if (saveId.contains('/') || saveId.contains("..")) {
            outError = QStringLiteral("Invalid save identifier.");
            setLastError(outError);
            return false;
        }

        const QString manifestPath = wsRoot + QStringLiteral("/.desk32/saves/") + saveId + QStringLiteral(".json");
        if (!QFile::exists(manifestPath)) {
            outError = QString("Save '%1' does not exist.").arg(saveId);
            setLastError(outError);
            return false;
        }

        if (!QFile::remove(manifestPath)) {
            outError = QString("Failed to delete save manifest '%1'.").arg(saveId);
            setLastError(outError);
            return false;
        }

        const int collected = runGarbageCollection();
        core::log::LogService::instance().log(
            "Workspace", "DeleteSave", "Success",
            QString("Deleted save '%1' and collected %2 unreferenced object(s).").arg(saveId).arg(collected),
            "WorkspaceSave", saveId);
        emit workspaceContentsChanged();
        return true;
    });
}

bool WorkspaceManager::deleteSaveFromUi(const QString& saveId) {
    QString err;
    return deleteSave(saveId, err);
}

int WorkspaceManager::runGarbageCollection() {
    if (!hasOpenWorkspace()) return 0;
    return runOnWorkerThread([this]() -> int {
        QString wsRoot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            wsRoot = m_openWorkspacePath;
        }
        if (wsRoot.isEmpty()) return 0;

        // Collect ALL SHA-256 hashes referenced by ANY valid save manifest
        const QDir savesDir(wsRoot + QStringLiteral("/.desk32/saves"));
        std::set<QString> referencedHashes;
        if (savesDir.exists()) {
            const QFileInfoList manifests = savesDir.entryInfoList(QStringList() << QStringLiteral("*.json"), QDir::Files);
            for (const QFileInfo& fi : manifests) {
                QFile f(fi.absoluteFilePath());
                if (!f.open(QIODevice::ReadOnly)) {
                    // Fail safe: if any manifest cannot be read, abort GC rather than risk deleting referenced objects!
                    return 0;
                }
                QJsonParseError err{};
                const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
                f.close();
                if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                    // Fail safe on unreadable/corrupted manifest
                    return 0;
                }
                const QJsonArray filesArr = doc.object().value(QStringLiteral("files")).toArray();
                for (const QJsonValue& v : filesArr) {
                    if (!v.isObject()) continue;
                    const QString sha = v.toObject().value(QStringLiteral("sha256")).toString();
                    if (!sha.isEmpty()) referencedHashes.insert(sha);
                }
            }
        }

        int deletedCount = 0;
        const QDir objectsDir(wsRoot + QStringLiteral("/.desk32/objects"));
        if (objectsDir.exists()) {
            const QFileInfoList objs = objectsDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
            for (const QFileInfo& fi : objs) {
                const QString objHash = fi.fileName();
                if (referencedHashes.count(objHash) == 0) {
                    if (triggerFaultIfSet(WorkspaceFaultPoint::DuringGcDelete, objHash)) {
                        break; // Simulate interrupted GC
                    }
                    if (QFile::remove(fi.absoluteFilePath())) {
                        ++deletedCount;
                    }
                }
            }
        }

        if (deletedCount > 0) {
            emit workspaceContentsChanged();
        }
        return deletedCount;
    });
}

int WorkspaceManager::runGarbageCollectionFromUi() {
    return runGarbageCollection();
}

} // namespace desk32::engine::workspace
