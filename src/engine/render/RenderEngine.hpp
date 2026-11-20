#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>

#include "WavFileWriter.hpp"
#include <functional>
#include "RenderSequenceManager.hpp"
#include "RenderChain.hpp"
#include "engine/session/FolderSession.hpp"
#include "core/log/LogService.hpp"

class TestPhase4ExportRender;

namespace desk32::engine::render {

enum class RenderBitDepth {
    Int16,
    Int24,
    Float32
};

enum class RenderSampleRateMode {
    Original,
    Custom
};

struct RenderSettings {
    RenderBitDepth bitDepth = RenderBitDepth::Int24;
    RenderSampleRateMode rateMode = RenderSampleRateMode::Original;
    double targetSampleRate = 48000.0;
    bool applyDither = true;
    bool enableMasterDsp = true;
    bool includePeq = true;
    bool includeGeq = true;
    bool includeFilter = true;
    bool includeCompressor = true;
    bool includeReverb = true;
    bool includeLimiter = true;

    [[nodiscard]] static QString resolveOutputWavFilename(const QString& inputFile) {
        const QFileInfo fi(inputFile);
        if (fi.suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0) {
            return fi.fileName();
        }
        return fi.completeBaseName() + QStringLiteral(".wav");
    }
};

enum class RenderStatus {
    Queued,
    Processing,
    Completed,
    Failed,
    Cancelled
};

enum class RenderWorkerState {
    Idle,
    Starting,
    Rendering,
    Stopping
};

enum class RenderType {
    SingleTrack,
    Album
};

struct RenderTaskInfo {
    QString taskId;
    RenderType type = RenderType::SingleTrack;
    QString sourcePath;
    QString sourceFolder;
    QString workspaceFolder;
    QStringList trackFiles;
    RenderSettings settings;
    RenderDspSnapshot dspSnapshot;
    RenderStatus status = RenderStatus::Queued;
    double progress = 0.0;
    int currentTrackIndex = 0;
    int totalTracks = 0;
    QString currentTrackName;
    QString finalOutputDir;
    QString stagingDir;
    QString serial;
    QString errorMessage;
    QString warningMessage;
    QVariantList trackResults;
    QStringList renderedFiles;
    bool rollbackPerformed = false;
    bool rollbackSucceeded = true;
};

/// Offline Export & Render Engine (Specification Section 5.11.3, 16.0 Phase 4 & 20.1).
/// Features:
/// - Background offline worker thread (NEVER runs on Audio Thread)
/// - Strictly serial execution queue (Decision D-2: 1 at a time FIFO)
/// - Non-reusable uppercase serial letters (Decision D-2: A..Z, AA..AZ..)
/// - Local datetime folder naming with UTC offset logging (Decision D-3)
/// - Pre-flight permission, collision, and disk space checks
/// - Staging folder transactions with complete rollback on error/cancel
/// - Strict isolation: 0.000000 Listening Correction leakage into rendered files
/// - Master Volume and Listening Volume non-interference
class RenderEngine : public QObject {
    Q_OBJECT

public:
    static constexpr qint64 MAX_CLASSIC_WAV_BYTES = 4294967200LL;

    static RenderEngine& instance();

    explicit RenderEngine(QObject* parent = nullptr);
    ~RenderEngine() override;

    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;

    /// Set SQLite database path for sequence management
    void setDatabasePath(const QString& dbPath);
    [[nodiscard]] QString databasePath() const;

    /// Set active dedicated workspace path for render outputs (Spec §16 Phase 4 / §19 D-2 revision).
    /// When set, rendered outputs, staging folders, and serial allocation occur under workspacePath.
    void setWorkspacePath(const QString& workspacePath);
    [[nodiscard]] QString workspacePath() const;

    /// Stop background worker thread safely
    void stop();

    /// Reset task info and clear status
    void resetTaskInfo();

    /// Template render chain configured from live console
    [[nodiscard]] RenderChain& templateChain() noexcept { return m_templateChain; }
    [[nodiscard]] const RenderChain& templateChain() const noexcept { return m_templateChain; }
    [[nodiscard]] std::unique_lock<std::mutex> lockTemplateChain() const {
        return std::unique_lock<std::mutex>(m_templateMutex);
    }

    /// Capture an immutable DSP snapshot from live template chain (P0-03)
    [[nodiscard]] RenderDspSnapshot captureTemplateSnapshot() const;

    /// Queue a single track render task (output folder is automatically created inside source folder)
    QString enqueueTrackRender(const QString& trackPath, const RenderSettings& settings = RenderSettings());

    /// Queue an album render task (all audio files in folder in natural sort order)
    QString enqueueAlbumRender(const QString& folderPath, const RenderSettings& settings = RenderSettings());

    /// Request cancellation of currently active render task
    void cancelCurrentRender();

    /// Request cancellation of currently active render task and all queued tasks
    void cancelAllRenders();

    [[nodiscard]] RenderWorkerState workerState() const noexcept;
    [[nodiscard]] bool isIdle() const noexcept;
    [[nodiscard]] bool isRendering() const noexcept;
    [[nodiscard]] int queuedTasksCount() const noexcept;
    [[nodiscard]] RenderTaskInfo currentTaskInfo() const;

    /// Pre-flight validation check
    static bool validatePreflight(const QString& sourceFolder, qint64 requiredBytes, QString& outError);

signals:
    void renderTaskStarted(const QString& taskId, const QString& name, int totalTracks);
    void renderProgressUpdated(const QString& taskId, double overallProgress, int trackIndex, int totalTracks, const QString& currentFile);
    void renderTaskFinished(const QString& taskId, int status, const QString& outputDir, const QString& errorMessage);

private:
    friend class ::TestPhase4ExportRender;

    /// Test-only hook to verify pre-existing collision and containment handling without exposing in public API
    QString enqueueTrackRenderForTesting(const QString& trackPath,
                                         const RenderSettings& settings,
                                         const QString& testOutputDir);

    using TestTaskHook = std::function<void(const QString& taskId,
                                            RenderWorkerState state,
                                            const std::atomic<bool>& cancelFlag)>;
    using TestHookCleanupCallback = std::function<void()>;

    void setTestTaskHook(TestTaskHook hook, TestHookCleanupCallback cleanupCb = nullptr);
    void clearTestTaskHook();
    [[nodiscard]] std::shared_ptr<TestTaskHook> getTestTaskHook() const;

    using StatDevResolver = std::function<bool(const QString& path, dev_t& outDev)>;

    /// Validates that output and staging directories are strictly contained in source folder on same filesystem
    static bool validateOutputContainment(const RenderTaskInfo& task,
                                         QString& outError,
                                         StatDevResolver statDevFn = nullptr);

    void workerLoop();
    bool processTask(RenderTaskInfo& task, RenderSequenceManager& seqMgr);
    bool renderSingleFile(const QString& inputFilePath,
                          const QString& outputFilePath,
                          const RenderSettings& settings,
                          const RenderDspSnapshot& dspSnapshot,
                          std::atomic<bool>& cancelFlag,
                          double& outProgressFraction,
                          QString& outError,
                          const std::function<void(double)>& onProgress = nullptr);

    mutable std::mutex m_templateMutex;
    RenderChain m_templateChain;
    QString m_dbPath;
    QString m_workspacePath;

    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::queue<RenderTaskInfo> m_taskQueue;
    RenderTaskInfo m_currentTask;
    std::atomic<RenderWorkerState> m_workerState{RenderWorkerState::Idle};
    std::atomic<bool> m_isRendering{false};
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<bool> m_cancelAllRequested{false};
    std::atomic<bool> m_stopWorker{false};
    std::atomic<uint64_t> m_nextTaskSeq{1};
    std::thread m_workerThread;

    mutable std::mutex m_testHookMutex;
    std::shared_ptr<TestTaskHook> m_testTaskHook;
    TestHookCleanupCallback m_testHookCleanupCb;
};

} // namespace desk32::engine::render
