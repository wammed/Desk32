#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QFileSystemWatcher>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "engine/metadata/TrackMetadata.hpp"

namespace desk32::engine::workspace {

enum class WorkspaceLockState {
    Locked = 0,     ///< Registered workspace not opened by any process (app-internal locked)
    Open = 1,       ///< Currently open and writable in this Desk32 session
    InUse = 2       ///< Locked exclusively (flock) by another running Desk32 process
};

enum class ImportResultStatus {
    Imported = 0,
    SkippedIdentical = 1,
    Conflict = 2,
    Failed = 3
};

enum class WorkspaceFaultPoint {
    None = 0,
    DuringImportBeforeCommit,
    BeforeCowCommit,
    DuringSaveScan,
    DuringRollbackPrepare,
    DuringRollbackApply,
    DuringRollbackRecover,
    DuringGcDelete
};

struct RegisteredWorkspaceInfo {
    QString path;
    QString lastUsedUtc;
    WorkspaceLockState lockState = WorkspaceLockState::Locked;
};

struct WorkspaceFileEntry {
    QString relativePath;
    QString absolutePath;
    QString sha256;
    qint64 size = 0;
    qint64 mtimeNs = 0;
    metadata::TrackMetadata metadata;
    bool externallyModified = false;
    QString externalChangeDetail;
};

struct SaveManifestFile {
    QString relativePath;
    QString sha256;
    qint64 size = 0;
    qint64 mtimeNs = 0;
    std::map<std::string, std::string> tags;
};

struct SaveManifest {
    int formatVersion = 1;
    QString saveId;
    QString name;
    QString createdUtc;
    QString localUtcOffset;
    bool isAutoSave = false;
    std::vector<SaveManifestFile> files;
    qint64 incrementalBytes = 0;
};

struct SaveTagDiffEntry {
    QString key;
    QString beforeValue;
    QString afterValue;
};

struct SaveFileDiffEntry {
    QString relativePath;
    QString changeType; // "Added", "Removed", "Modified"
    QString beforeSha256;
    QString afterSha256;
    qint64 beforeSize = 0;
    qint64 afterSize = 0;
    std::vector<SaveTagDiffEntry> tagDiffs;
};

struct SaveComparisonResult {
    bool valid = false;
    QString errorMessage;
    QString fromSaveId;
    QString toSaveId;
    int addedCount = 0;
    int removedCount = 0;
    int modifiedCount = 0;
    int unchangedCount = 0;
    std::vector<SaveFileDiffEntry> diffs;
};

struct ImportResult {
    ImportResultStatus status = ImportResultStatus::Failed;
    QString sourcePath;
    QString destPath;
    QString originalSha256;
    bool provenanceEmbedded = false;
    QString message;
};

struct RollbackResult {
    bool success = false;
    bool partialRecovery = false;
    QString autoSaveId;
    QString errorMessage;
    QStringList failedPaths;
};

/// Dedicated Workspace Manager implementing Desk32 v3.8.1 §2.5, §9.5, §10.6, §17.6, and Completion Criterion #35:
/// - Exclusive single-workspace model with startup selection & app-internal locking (no chmod/ACL mutation)
/// - Cross-process mutual exclusion via POSIX flock(2) on .desk32/workspace.lock
/// - Dedicated Workspace Worker Thread isolated from GUI Thread and RT Audio Thread
/// - Transactional file import with SHA-256 verification and TagLib provenance embedding
/// - Same-name collision detection comparing pre-embed original SHA-256 against stored provenance
/// - Containment (realpath), symlink exclusion, and hard-link (st_nlink >= 2) rejection
/// - Copy-on-write lazy evacuation before any tag edit, overwrite, or deletion
/// - Content-addressed deduplicated saves (.desk32/saves/, .desk32/objects/), diffs, rollback with recovery, and safe GC
/// - 2-stage external modification detection (inotify/QFileSystemWatcher + SHA-256 verification)
class WorkspaceManager : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool hasOpenWorkspace READ hasOpenWorkspace NOTIFY workspaceStateChanged)
    Q_PROPERTY(bool isReadOnlyMode READ isReadOnlyMode NOTIFY workspaceStateChanged)
    Q_PROPERTY(bool selectionFinalized READ isSelectionFinalized NOTIFY workspaceStateChanged)
    Q_PROPERTY(QString openWorkspacePath READ openWorkspacePath NOTIFY workspaceStateChanged)
    Q_PROPERTY(qint64 saveStoreTotalBytes READ saveStoreTotalBytes NOTIFY workspaceContentsChanged)
    Q_PROPERTY(int externalModificationCount READ externalModificationCount NOTIFY externalModificationDetected)

public:
    static WorkspaceManager& instance();

    explicit WorkspaceManager(QObject* parent = nullptr);
    ~WorkspaceManager() override;

    WorkspaceManager(const WorkspaceManager&) = delete;
    WorkspaceManager& operator=(const WorkspaceManager&) = delete;

    // -------------------------------------------------------------------------
    // Configuration & Test Helpers
    // -------------------------------------------------------------------------
    void setConfigFilePathForTesting(const QString& path);
    [[nodiscard]] QString configFilePath() const;
    void resetForTesting();
    void simulateAppRestartForTesting();

    using FaultCallback = std::function<bool(WorkspaceFaultPoint point, const QString& detailPath)>;
    void setTestFaultCallback(FaultCallback cb);

    /// Callback invoked prior to rollback/adoptSave or destructive file operations when workspace files are loaded/playing
    using StopPlaybackCallback = std::function<void(const QStringList& affectedPaths)>;
    void setStopPlaybackCallback(StopPlaybackCallback cb);

    /// Callback invoked after Stage 2 integrity verification detects modified workspace files on disk
    using MetadataReloadCallback = std::function<void(const QStringList& modifiedAbsPaths)>;
    void setMetadataReloadCallback(MetadataReloadCallback cb);

    /// Refresh .desk32/state_index.json and QFileSystemWatcher immediately after internal engine writes (e.g. RenderEngine output)
    void refreshIndexAfterInternalWrite();

    /// Acknowledge and clear persisted external modification badges for currently tracked files
    Q_INVOKABLE void acknowledgeExternalModifications();

    // -------------------------------------------------------------------------
    // Registration, Startup Selection, Locking & Process Exclusion
    // -------------------------------------------------------------------------
    [[nodiscard]] bool hasOpenWorkspace() const;
    [[nodiscard]] bool isReadOnlyMode() const;
    [[nodiscard]] bool isSelectionFinalized() const;
    [[nodiscard]] QString openWorkspacePath() const;
    [[nodiscard]] qint64 saveStoreTotalBytes() const;
    [[nodiscard]] int externalModificationCount() const;

    bool registerWorkspace(const QString& dirPath, QString& outError);
    bool openWorkspace(const QString& dirPath, QString& outError);
    bool registerAndOpenWorkspace(const QString& dirPath, QString& outError);
    void enterReadOnlyMode();
    void closeWorkspaceForShutdown();

    [[nodiscard]] std::vector<RegisteredWorkspaceInfo> listRegisteredWorkspaces() const;
    [[nodiscard]] bool isWorkspaceInUseByOtherProcess(const QString& workspacePath) const;

    /// Returns true if targetPath falls inside any REGISTERED workspace that is NOT the currently open workspace.
    /// Per v3.8.1 §2.5, all access (playback, Folder Session, import source, render) to locked workspaces is refused.
    [[nodiscard]] bool isPathBlockedByLockedWorkspace(const QString& targetPath, QString* outReason = nullptr) const;

    /// Returns true if targetPath is safely contained inside the currently open workspace (non-symlink, non-.desk32).
    [[nodiscard]] bool isPathInsideOpenWorkspace(const QString& targetPath, bool allowNonExistentLeaf = false, QString* outError = nullptr) const;

    // QML-invokable workspace lifecycle APIs
    Q_INVOKABLE QVariantList getRegisteredWorkspaces() const;
    Q_INVOKABLE bool selectWorkspaceAtStartup(const QString& dirPath);
    Q_INVOKABLE bool registerAndSelectWorkspaceAtStartup(const QString& dirPath);
    Q_INVOKABLE void continueInReadOnlyMode();
    Q_INVOKABLE QString lastError() const;

    // -------------------------------------------------------------------------
    // Workspace Directory Listing, Import, Edit, Delete, Rename/Move
    // -------------------------------------------------------------------------
    std::vector<WorkspaceFileEntry> listWorkspaceFiles(const QString& relativeSubDir = QString()) const;
    Q_INVOKABLE QVariantList getWorkspaceFiles(const QString& relativeSubDir = QString()) const;
    Q_INVOKABLE QVariantMap getFileDetails(const QString& relativePath) const;

    ImportResult importFile(const QString& sourceFilePath, const QString& relativeDestDir = QString());
    Q_INVOKABLE QVariantMap importFileFromUi(const QString& sourceFilePath, const QString& relativeDestDir = QString());
    std::vector<ImportResult> importFolder(const QString& sourceFolderPath, const QString& relativeDestDir = QString());
    Q_INVOKABLE QVariantList importFolderFromUi(const QString& sourceFolderPath, const QString& relativeDestDir = QString());

    bool editFileTags(const QString& relativePath, const metadata::TrackMetadata& updatedMeta, QString& outError);
    Q_INVOKABLE bool editFileTagsFromUi(const QString& relativePath, const QVariantMap& tagMap);

    bool renameWorkspaceFile(const QString& relativePath, const QString& newFileName, QString& outError);
    Q_INVOKABLE bool renameFileFromUi(const QString& relativePath, const QString& newFileName);

    bool moveWorkspaceFile(const QString& relativePath, const QString& targetRelativeDir, QString& outError);
    Q_INVOKABLE bool moveFileFromUi(const QString& relativePath, const QString& targetRelativeDir);

    bool deleteWorkspaceFile(const QString& relativePath, QString& outError);
    Q_INVOKABLE bool deleteFileFromUi(const QString& relativePath);

    // -------------------------------------------------------------------------
    // Checkpoints (.desk32/saves/), Copy-on-Write, Compare, Rollback, GC
    // -------------------------------------------------------------------------
    bool createSave(const QString& saveName, QString& outSaveId, QString& outError, bool isAutoSave = false);
    Q_INVOKABLE QString createSaveFromUi(const QString& saveName);

    std::vector<SaveManifest> listSaves() const;
    Q_INVOKABLE QVariantList getSaves() const;

    SaveComparisonResult compareSaves(const QString& fromSaveId, const QString& toSaveId) const;
    Q_INVOKABLE QVariantMap compareSavesFromUi(const QString& fromSaveId, const QString& toSaveId) const;

    /// Materialize a saved version of a track into a read-only preview path (or live path if unchanged) for Deck audition
    bool resolveSavedTrackForAudition(const QString& saveId, const QString& relativePath, QString& outFilePath, QString& outError);
    Q_INVOKABLE QString prepareSavedTrackForAuditionFromUi(const QString& saveId, const QString& relativePath);

    RollbackResult adoptSave(const QString& saveId);
    Q_INVOKABLE QVariantMap adoptSaveFromUi(const QString& saveId);

    bool deleteSave(const QString& saveId, QString& outError);
    Q_INVOKABLE bool deleteSaveFromUi(const QString& saveId);

    int runGarbageCollection();
    Q_INVOKABLE int runGarbageCollectionFromUi();

    /// Verify current workspace contents against state index and save references (Stage 2 hash check)
    QStringList verifyWorkspaceIntegrity();
    Q_INVOKABLE QStringList checkExternalChangesFromUi();

signals:
    void workspaceStateChanged();
    void workspaceContentsChanged();
    void externalModificationDetected(const QString& path, const QString& detail);
    void workspaceErrorOccurred(const QString& operation, const QString& message);
    void importCompleted(const QVariantMap& summary);

private:
    void startWorkerThread();
    void stopWorkerThread();

    template <typename Fn>
    auto runOnWorkerThread(Fn&& fn) const -> decltype(fn()) {
        using Ret = decltype(fn());
        if (std::this_thread::get_id() == m_workerThreadId) {
            return fn();
        }
        auto task = std::make_shared<std::packaged_task<Ret()>>(std::forward<Fn>(fn));
        auto fut = task->get_future();
        {
            std::lock_guard<std::mutex> lock(m_workerQueueMutex);
            if (!m_workerRunning) {
                return (*task)(), fut.get();
            }
            m_workerQueue.push([task]() { (*task)(); });
        }
        m_workerCv.notify_one();
        return fut.get();
    }

    // Internal helpers (execute on WorkspaceWorkerThread)
    std::vector<RegisteredWorkspaceInfo> loadRegisteredWorkspacesLocked() const;
    bool saveRegisteredWorkspacesLocked(const std::vector<RegisteredWorkspaceInfo>& list) const;

    static QString resolveRealPath(const QString& path);
    static bool isSameOrSubpath(const QString& candidate, const QString& base);
    static bool computeFileSha256(const QString& absPath, QString& outHex, qint64* outSize = nullptr, qint64* outMtimeNs = nullptr);
    static bool checkRegularNoHardlink(const QString& absPath, QString& outError, qint64* outSize = nullptr, qint64* outMtimeNs = nullptr);
    static bool copyFileReadOnlyVerified(const QString& srcAbsPath,
                                         const QString& dstAbsPath,
                                         const QString& expectedSha256,
                                         QString& outActualSha256,
                                         QString& outError);

    bool resolveInsideOpenWorkspace(const QString& inputPath,
                                    bool allowNonExistentLeaf,
                                    QString& outAbsPath,
                                    QString& outRelPath,
                                    QString& outError) const;

    bool isObjectReferencedLocked(const QString& sha256) const;
    bool evacuateCopyOnWriteLocked(const QString& absPath, const QString& relPath, QString& outError);
    bool loadAllSavesLocked(std::vector<SaveManifest>& outSaves) const;
    bool loadSaveByIdLocked(const QString& saveId, SaveManifest& outManifest, QString& outError) const;
    bool writeSaveManifestLocked(const SaveManifest& manifest, QString& outError) const;
    void scanCurrentWorkspaceLocked(std::vector<WorkspaceFileEntry>& outEntries, bool computeTags = true) const;
    bool loadStateIndexLocked(std::map<QString, std::pair<QString, qint64>>& outIndex) const;
    bool saveStateIndexLocked(const std::vector<WorkspaceFileEntry>& entries) const;
    void detectExternalChangesAgainstIndexLocked(const std::vector<WorkspaceFileEntry>& currentEntries,
                                                 QStringList& outMessages);
    void refreshWatcherLocked();
    void cleanTempDirectoryLocked();
    bool triggerFaultIfSet(WorkspaceFaultPoint point, const QString& detailPath) const;
    void setLastError(const QString& err);

    mutable std::mutex m_stateMutex;
    QString m_customConfigPath;
    QString m_openWorkspacePath;
    int m_lockFd = -1;
    bool m_selectionFinalized = false;
    bool m_readOnlyMode = true;
    QString m_lastError;
    int m_externalModificationCount = 0;

    std::unique_ptr<QFileSystemWatcher> m_watcher;
    FaultCallback m_faultCallback;
    StopPlaybackCallback m_stopPlaybackCallback;
    MetadataReloadCallback m_metadataReloadCallback;
    std::map<QString, QString> m_externalChangeDetails;

    // Dedicated Workspace Worker Thread (Specification §9.5)
    mutable std::mutex m_workerQueueMutex;
    mutable std::condition_variable m_workerCv;
    mutable std::queue<std::function<void()>> m_workerQueue;
    std::thread m_workerThread;
    std::thread::id m_workerThreadId{};
    bool m_workerRunning = false;
};

} // namespace desk32::engine::workspace
