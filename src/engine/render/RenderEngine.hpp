#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <memory>
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>

#include "WavFileWriter.hpp"
#include "RenderSequenceManager.hpp"
#include "RenderChain.hpp"
#include "engine/session/FolderSession.hpp"
#include "core/log/LogService.hpp"

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
};

enum class RenderStatus {
    Queued,
    Processing,
    Completed,
    Failed,
    Cancelled
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
    QStringList trackFiles;
    RenderSettings settings;
    RenderStatus status = RenderStatus::Queued;
    double progress = 0.0;
    int currentTrackIndex = 0;
    int totalTracks = 0;
    QString currentTrackName;
    QString finalOutputDir;
    QString stagingDir;
    QString serial;
    QString errorMessage;
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
    static RenderEngine& instance();

    explicit RenderEngine(QObject* parent = nullptr);
    ~RenderEngine() override;

    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;

    /// Stop background worker thread safely
    void stop();

    /// Reset task info and clear status
    void resetTaskInfo();

    /// Template render chain configured from live console
    [[nodiscard]] RenderChain& templateChain() noexcept { return m_templateChain; }
    [[nodiscard]] const RenderChain& templateChain() const noexcept { return m_templateChain; }

    /// Queue a single track render task
    QString enqueueTrackRender(const QString& trackPath, const RenderSettings& settings = RenderSettings());

    /// Queue an album render task (all audio files in folder in natural sort order)
    QString enqueueAlbumRender(const QString& folderPath, const RenderSettings& settings = RenderSettings());

    /// Request cancellation of currently active render task
    void cancelCurrentRender();

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
    void workerLoop();
    bool processTask(RenderTaskInfo& task);
    bool renderSingleFile(const QString& inputFilePath,
                          const QString& outputFilePath,
                          const RenderSettings& settings,
                          std::atomic<bool>& cancelFlag,
                          double& outProgressFraction,
                          QString& outError);

    RenderChain m_templateChain;

    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::queue<RenderTaskInfo> m_taskQueue;
    RenderTaskInfo m_currentTask;
    std::atomic<bool> m_isRendering{false};
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<bool> m_stopWorker{false};
    std::thread m_workerThread;
};

} // namespace desk32::engine::render
