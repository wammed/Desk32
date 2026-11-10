#include "RenderEngine.hpp"
#include "engine/decoder/FFmpegAudioDecoder.hpp"
#include "engine/dsp/Resampler.hpp"
#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <QDateTime>
#include <QThread>
#include <QDebug>
#include <QSet>
#include <sys/stat.h>
#include <array>
#include <algorithm>

namespace desk32::engine::render {

RenderEngine& RenderEngine::instance() {
    static RenderEngine s_instance;
    return s_instance;
}

RenderEngine::RenderEngine(QObject* parent)
    : QObject(parent)
{
    // Start background render worker thread
    m_workerThread = std::thread(&RenderEngine::workerLoop, this);
}

RenderEngine::~RenderEngine() {
    stop();
}

void RenderEngine::stop() {
    cancelCurrentRender();
    m_stopWorker.store(true, std::memory_order_release);
    m_queueCv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

bool RenderEngine::isRendering() const noexcept {
    return m_isRendering.load(std::memory_order_acquire);
}

int RenderEngine::queuedTasksCount() const noexcept {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return static_cast<int>(m_taskQueue.size());
}

RenderTaskInfo RenderEngine::currentTaskInfo() const {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_currentTask.taskId.isEmpty() && !m_taskQueue.empty()) {
        return m_taskQueue.front();
    }
    return m_currentTask;
}

void RenderEngine::cancelCurrentRender() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_isRendering.load(std::memory_order_acquire) || !m_taskQueue.empty()) {
        m_cancelRequested.store(true, std::memory_order_release);
    }
}

void RenderEngine::setDatabasePath(const QString& dbPath) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_dbPath = dbPath;
}

QString RenderEngine::databasePath() const {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_dbPath;
}

RenderDspSnapshot RenderEngine::captureTemplateSnapshot() const {
    std::lock_guard<std::mutex> lock(m_templateMutex);
    return m_templateChain.createSnapshot();
}

bool RenderEngine::validatePreflight(const QString& sourceFolder, qint64 requiredBytes, QString& outError) {
    QFileInfo dirInfo(sourceFolder);
    if (!dirInfo.exists() || !dirInfo.isDir()) {
        outError = QString("Source folder does not exist: %1").arg(sourceFolder);
        return false;
    }

    if (!dirInfo.isWritable()) {
        outError = QString("Write permission denied for target directory: %1. Desk32 does not request or escalate privileges.")
                       .arg(sourceFolder);
        return false;
    }

    QStorageInfo storage(sourceFolder);
    if (storage.isValid()) {
        // Enforce required space plus a 20MB safety margin
        if (storage.bytesAvailable() < (requiredBytes + 20 * 1024 * 1024)) {
            outError = QString("Insufficient disk space in %1: %2 MB available, %3 MB required.")
                           .arg(sourceFolder)
                           .arg(storage.bytesAvailable() / (1024 * 1024))
                           .arg((requiredBytes + 20 * 1024 * 1024) / (1024 * 1024));
            return false;
        }
    }

    return true;
}

bool RenderEngine::validateOutputContainment(const RenderTaskInfo& task, QString& outError) {
    if (task.sourceFolder.isEmpty() || task.finalOutputDir.isEmpty()) {
        outError = "Source folder or output directory path is empty.";
        return false;
    }

    QFileInfo srcFolderInfo(task.sourceFolder);
    if (!srcFolderInfo.exists() || !srcFolderInfo.isDir()) {
        outError = QString("Source folder does not exist or is not a directory: %1").arg(task.sourceFolder);
        return false;
    }

    // 1. Resolve canonical source directory
    QString canonicalSource = srcFolderInfo.canonicalFilePath();
    if (canonicalSource.isEmpty()) {
        outError = QString("Failed to resolve canonical path for source folder: %1").arg(task.sourceFolder);
        return false;
    }

    // 2. Output directory parent must be strictly contained inside canonical source folder
    QFileInfo outDirInfo(task.finalOutputDir);
    QDir parentDir = outDirInfo.dir();
    QString canonicalParent = parentDir.canonicalPath();
    if (canonicalParent.isEmpty()) {
        outError = QString("Failed to resolve parent directory for output path: %1").arg(task.finalOutputDir);
        return false;
    }

    // Output must be an immediate child of the source folder (prevent directory traversal or external paths)
    if (QDir::cleanPath(canonicalParent) != QDir::cleanPath(canonicalSource)) {
        outError = QString("Render output directory '%1' escapes original sound source directory '%2'.")
                       .arg(task.finalOutputDir, task.sourceFolder);
        return false;
    }

    // 3. Output directory cannot point to source file itself or any track file
    if (!task.sourcePath.isEmpty()) {
        QFileInfo srcFileInfo(task.sourcePath);
        if (QDir::cleanPath(outDirInfo.absoluteFilePath()) == QDir::cleanPath(srcFileInfo.absoluteFilePath())) {
            outError = "Output directory conflicts with original sound source file.";
            return false;
        }
    }
    for (const auto& tf : task.trackFiles) {
        if (outDirInfo.fileName() == tf) {
            outError = QString("Output directory name matches original sound source file: %1").arg(tf);
            return false;
        }
    }

    // 4. Output directory must not be a symlink
    if (outDirInfo.isSymLink()) {
        outError = QString("Output directory cannot be a symbolic link: %1").arg(task.finalOutputDir);
        return false;
    }

    // 5. Staging directory must also be directly inside canonical source folder
    if (!task.stagingDir.isEmpty()) {
        QFileInfo stagingInfo(task.stagingDir);
        QDir stagingParent = stagingInfo.dir();
        QString canonicalStagingParent = stagingParent.canonicalPath();
        if (QDir::cleanPath(canonicalStagingParent) != QDir::cleanPath(canonicalSource)) {
            outError = QString("Staging directory '%1' escapes original sound source directory '%2'.")
                           .arg(task.stagingDir, task.sourceFolder);
            return false;
        }
        if (stagingInfo.isSymLink()) {
            outError = QString("Staging directory cannot be a symbolic link: %1").arg(task.stagingDir);
            return false;
        }
    }

    // 6. Verify same filesystem (stat st_dev match between source folder and output parent)
    struct stat stSource{}, stParent{};
    if (stat(canonicalSource.toLocal8Bit().constData(), &stSource) != 0) {
        outError = QString("Failed to stat source directory: %1").arg(canonicalSource);
        return false;
    }
    if (stat(canonicalParent.toLocal8Bit().constData(), &stParent) != 0) {
        outError = QString("Failed to stat output parent directory: %1").arg(canonicalParent);
        return false;
    }
    if (stSource.st_dev != stParent.st_dev) {
        outError = QString("Staging and output directories must reside on the same filesystem as source: "
                           "source dev=%1, parent dev=%2").arg(stSource.st_dev).arg(stParent.st_dev);
        return false;
    }

    return true;
}

QString RenderEngine::enqueueTrackRender(const QString& trackPath, const RenderSettings& settings) {
    QFileInfo fi(trackPath);
    if (!fi.exists() || !fi.isFile()) {
        qWarning() << "RenderEngine: track file does not exist:" << trackPath;
        return QString();
    }

    RenderTaskInfo task;
    task.taskId = QString("track_%1").arg(QDateTime::currentMSecsSinceEpoch());
    task.type = RenderType::SingleTrack;
    task.sourcePath = trackPath;
    task.sourceFolder = fi.absolutePath();
    task.trackFiles.append(fi.fileName());
    task.totalTracks = 1;
    task.settings = settings;
    task.dspSnapshot = captureTemplateSnapshot(); // P0-03: Freeze immutable DSP snapshot at enqueue time
    task.status = RenderStatus::Queued;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_currentTask.taskId.isEmpty() && !m_isRendering.load(std::memory_order_relaxed)) {
            m_currentTask = task;
        }
        m_taskQueue.push(task);
    }
    m_queueCv.notify_one();
    return task.taskId;
}

QString RenderEngine::enqueueTrackRenderForTesting(const QString& trackPath, const RenderSettings& settings, const QString& testOutputDir) {
    QFileInfo fi(trackPath);
    if (!fi.exists() || !fi.isFile()) {
        qWarning() << "RenderEngine: test track file does not exist:" << trackPath;
        return QString();
    }

    RenderTaskInfo task;
    task.taskId = QString("test_track_%1").arg(QDateTime::currentMSecsSinceEpoch());
    task.type = RenderType::SingleTrack;
    task.sourcePath = trackPath;
    task.sourceFolder = fi.absolutePath();
    task.trackFiles.append(fi.fileName());
    task.totalTracks = 1;
    task.settings = settings;
    task.dspSnapshot = captureTemplateSnapshot();
    task.status = RenderStatus::Queued;
    task.finalOutputDir = testOutputDir;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_currentTask.taskId.isEmpty() && !m_isRendering.load(std::memory_order_relaxed)) {
            m_currentTask = task;
        }
        m_taskQueue.push(task);
    }
    m_queueCv.notify_one();
    return task.taskId;
}

QString RenderEngine::enqueueAlbumRender(const QString& folderPath, const RenderSettings& settings) {
    QDir dir(folderPath);
    if (!dir.exists()) {
        qWarning() << "RenderEngine: album folder does not exist:" << folderPath;
        return QString();
    }

    // Supported extensions: .flac, .wav, .mp3, .ogg, .opus, .m4a, .aac
    QStringList filters;
    filters << "*.flac" << "*.wav" << "*.mp3" << "*.ogg" << "*.opus" << "*.m4a" << "*.aac";
    QFileInfoList list = dir.entryInfoList(filters, QDir::Files, QDir::Unsorted);

    if (list.isEmpty()) {
        qWarning() << "RenderEngine: no supported audio files in album folder:" << folderPath;
        return QString();
    }

    std::vector<std::string> fileNames;
    fileNames.reserve(static_cast<size_t>(list.size()));
    for (const auto& fi : list) {
        fileNames.push_back(fi.fileName().toStdString());
    }

    // Sort deterministically using NaturalSortComparator (Section 2.5)
    std::sort(fileNames.begin(), fileNames.end(), [](const std::string& a, const std::string& b) {
        return session::NaturalSortComparator::naturalSortLess(a, b);
    });

    QStringList sortedList;
    for (const auto& name : fileNames) {
        sortedList.append(QString::fromStdString(name));
    }

    RenderTaskInfo task;
    task.taskId = QString("album_%1").arg(QDateTime::currentMSecsSinceEpoch());
    task.type = RenderType::Album;
    task.sourcePath = folderPath;
    task.sourceFolder = dir.absolutePath();
    task.trackFiles = sortedList;
    task.totalTracks = static_cast<int>(sortedList.size());
    task.settings = settings;
    task.dspSnapshot = captureTemplateSnapshot(); // P0-03: Freeze immutable DSP snapshot at enqueue time
    task.status = RenderStatus::Queued;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_currentTask.taskId.isEmpty() && !m_isRendering.load(std::memory_order_relaxed)) {
            m_currentTask = task;
        }
        m_taskQueue.push(task);
    }
    m_queueCv.notify_one();
    return task.taskId;
}

void RenderEngine::resetTaskInfo() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_currentTask = RenderTaskInfo();
    m_cancelRequested.store(false, std::memory_order_release);
}

void RenderEngine::workerLoop() {
    // P0-02: SQLite Connection is created, owned, and closed strictly within this worker thread
    const QString workerConnName = QString("Desk32RenderWorkerConn_%1")
                                       .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    RenderSequenceManager workerSeqMgr(workerConnName);

    while (!m_stopWorker.load(std::memory_order_acquire)) {
        RenderTaskInfo task;
        QString activeDbPath;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            if (m_taskQueue.empty() && !m_isRendering.load(std::memory_order_acquire)) {
                m_cancelRequested.store(false, std::memory_order_release);
            }
            m_queueCv.wait(lock, [this]() {
                return m_stopWorker.load(std::memory_order_acquire) || !m_taskQueue.empty();
            });

            if (m_stopWorker.load(std::memory_order_acquire)) {
                break;
            }

            task = m_taskQueue.front();
            m_taskQueue.pop();
            m_currentTask = task;
            activeDbPath = m_dbPath;
        }

        if (!workerSeqMgr.isInitialized()) {
            workerSeqMgr.initialize(activeDbPath);
        }

        bool wasCancelRequested = m_cancelRequested.exchange(false, std::memory_order_acq_rel);
        m_isRendering.store(true, std::memory_order_release);

        {
            const std::string startedId = task.taskId.toStdString();
            const std::string startedSource = task.sourcePath.toStdString();
            const int startedTotal = task.totalTracks;
            QMetaObject::invokeMethod(this, [this, startedId, startedSource, startedTotal]() {
                emit renderTaskStarted(QString::fromStdString(startedId),
                                       QString::fromStdString(startedSource),
                                       startedTotal);
            }, Qt::QueuedConnection);
        }

        if (wasCancelRequested) {
            task.status = RenderStatus::Cancelled;
            task.errorMessage = "Render cancelled by user";
        } else {
            bool success = processTask(task, workerSeqMgr);
            (void)success;
        }

        m_cancelRequested.store(false, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_currentTask = task;
        }

        m_isRendering.store(false, std::memory_order_release);

        {
            const std::string finishedId = task.taskId.toStdString();
            const int finishedStatus = static_cast<int>(task.status);
            const std::string finishedOutDir = task.finalOutputDir.toStdString();
            const std::string finishedErr = task.errorMessage.toStdString();
            QMetaObject::invokeMethod(this, [this, finishedId, finishedStatus, finishedOutDir, finishedErr]() {
                emit renderTaskFinished(QString::fromStdString(finishedId),
                                        finishedStatus,
                                        QString::fromStdString(finishedOutDir),
                                        QString::fromStdString(finishedErr));
            }, Qt::QueuedConnection);
        }
    }

    workerSeqMgr.close();
}

bool RenderEngine::processTask(RenderTaskInfo& task, RenderSequenceManager& seqMgr) {
    const QString parentOpId = QString("RENDER_%1").arg(task.taskId);

    // 1. Pre-flight Check: Accurate PCM disk space estimation (P0-12)
    qint64 estimatedBytes = 0;
    qint64 bytesPerSample = (task.settings.bitDepth == RenderBitDepth::Int16) ? 2 :
                            ((task.settings.bitDepth == RenderBitDepth::Float32) ? 4 : 3);

    for (const auto& f : task.trackFiles) {
        QString inputPath = task.sourceFolder + "/" + f;
        double durationSec = 0.0;
        double srcRate = 48000.0;

        decoder::FFmpegAudioDecoder probeDecoder;
        if (probeDecoder.open(inputPath.toStdString())) {
            const auto& fmt = probeDecoder.formatInfo();
            if (fmt.sampleRate > 0 && fmt.totalFrames > 0) {
                durationSec = static_cast<double>(fmt.totalFrames) / fmt.sampleRate;
            }
            srcRate = fmt.sampleRate > 0 ? fmt.sampleRate : 48000.0;
            probeDecoder.close();
        } else {
            // Fallback estimation if probe fails
            QFileInfo fi(inputPath);
            durationSec = std::max<double>(180.0, static_cast<double>(fi.size()) / (44100.0 * 2.0 * 2.0));
        }

        double outRate = (task.settings.rateMode == RenderSampleRateMode::Custom && task.settings.targetSampleRate > 0.0)
                             ? task.settings.targetSampleRate : srcRate;
        qint64 pcmBytes = static_cast<qint64>(std::ceil(durationSec * outRate * 2.0 * static_cast<double>(bytesPerSample)));

        // P0-12: Detect WAV 4 GiB size overflow upfront and reject before starting render
        if (pcmBytes + 44 > MAX_CLASSIC_WAV_BYTES) {
            task.status = RenderStatus::Failed;
            task.errorMessage = QString("Estimated WAV file size (%1 MB) exceeds classic 4 GiB RIFF limit for track %2.")
                                   .arg((pcmBytes + 44) / (1024 * 1024))
                                   .arg(f);
            core::log::LogService::instance().log(
                "Render", "Preflight", "Failed", task.errorMessage, "AudioFile", inputPath, "{}",
                parentOpId, ""
            );
            return false;
        }

        estimatedBytes += (pcmBytes + 44 + 1024 * 1024); // header + 1MB safety margin per file
    }

    QString preflightError;
    if (!validatePreflight(task.sourceFolder, estimatedBytes, preflightError)) {
        task.status = RenderStatus::Failed;
        task.errorMessage = preflightError;
        core::log::LogService::instance().log(
            "Render",
            task.type == RenderType::Album ? "AlbumRender" : "TrackRender",
            "Failed",
            QString("Pre-flight check failed: %1").arg(preflightError),
            "FileSystem",
            task.sourceFolder,
            QString("{\"error\":\"%1\"}").arg(preflightError),
            parentOpId, ""
        );
        return false;
    }

    // P1-RND-01: In Album Render, detect duplicate output base names upfront to prevent overwriting
    if (task.type == RenderType::Album && task.trackFiles.size() > 1) {
        QSet<QString> seenBaseNames;
        for (const auto& f : task.trackFiles) {
            QString baseName = QFileInfo(f).completeBaseName();
            if (seenBaseNames.contains(baseName)) {
                task.status = RenderStatus::Failed;
                task.errorMessage = QString("Duplicate output track base name detected in album: '%1.wav'. "
                                            "Render aborted to prevent data overwrite.")
                                        .arg(baseName);
                core::log::LogService::instance().log(
                    "Render", "AlbumRender", "Failed", task.errorMessage, "AudioFile", task.sourceFolder, "{}",
                    parentOpId, ""
                );
                return false;
            }
            seenBaseNames.insert(baseName);
        }
    }

    // 2. Allocate non-reusable serial letter via worker-owned manager (P0-01, P0-02)
    task.serial = seqMgr.allocateNextSerial(task.sourceFolder);
    if (task.serial.isEmpty()) {
        task.status = RenderStatus::Failed;
        task.errorMessage = "Failed to allocate render sequence number (database initialization or transaction error).";
        core::log::LogService::instance().log(
            "Render", "AllocateSerial", "Failed", task.errorMessage, "Database", task.sourceFolder, "{}",
            parentOpId, ""
        );
        return false;
    }

    QDateTime now = QDateTime::currentDateTime();
    QString folderSuffix = RenderSequenceManager::formatFolderName(task.serial, now);

    if (task.stagingDir.isEmpty()) {
        task.stagingDir = task.sourceFolder + "/.desk32_staging_" + folderSuffix;
    }
    if (task.finalOutputDir.isEmpty()) {
        task.finalOutputDir = task.sourceFolder + "/" + folderSuffix;
    }

    // A-3 / SYS-08: Validate output and staging containment inside source folder on same filesystem
    QString containmentErr;
    if (!validateOutputContainment(task, containmentErr)) {
        task.status = RenderStatus::Failed;
        task.errorMessage = containmentErr;
        core::log::LogService::instance().log(
            "Render", "ContainmentCheck", "Failed", task.errorMessage, "FileSystem", task.finalOutputDir, "{}",
            parentOpId, ""
        );
        return false;
    }

    // P2-RND-02: Pre-flight check for final output directory collision (never overwrite or touch existing)
    if (QFileInfo::exists(task.finalOutputDir)) {
        task.status = RenderStatus::Failed;
        task.errorMessage = QString("Final output directory already exists: %1. "
                                    "Render aborted to prevent collision with existing data.")
                                .arg(task.finalOutputDir);
        core::log::LogService::instance().log(
            "Render", "Preflight", "Failed", task.errorMessage, "FileSystem", task.finalOutputDir, "{}",
            parentOpId, ""
        );
        return false;
    }

    // P2-RND-02: Staging directory must also not pre-exist
    if (QFileInfo::exists(task.stagingDir)) {
        task.status = RenderStatus::Failed;
        task.errorMessage = QString("Staging directory already exists: %1. "
                                    "Render aborted to prevent collision.")
                                .arg(task.stagingDir);
        core::log::LogService::instance().log(
            "Render", "Preflight", "Failed", task.errorMessage, "FileSystem", task.stagingDir, "{}",
            parentOpId, ""
        );
        return false;
    }

    // Create staging folder
    if (!QDir().mkpath(task.stagingDir)) {
        task.status = RenderStatus::Failed;
        task.errorMessage = "Failed to create temporary staging directory: " + task.stagingDir;
        core::log::LogService::instance().log(
            "Render", "CreateStagingDir", "Failed", task.errorMessage, "FileSystem", task.stagingDir, "{}",
            parentOpId, ""
        );
        return false;
    }

    core::log::LogService::instance().log(
        "Render",
        task.type == RenderType::Album ? "AlbumRender" : "TrackRender",
        "Info",
        QString("Starting render: %1 files into folder %2 [Serial: %3, Staging: %4]")
            .arg(task.totalTracks).arg(folderSuffix, task.serial, task.stagingDir),
        "RenderEngine",
        task.finalOutputDir,
        "{}",
        parentOpId, ""
    );

    // 3. Render tracks sequentially
    bool allTracksOk = true;

    for (int i = 0; i < task.trackFiles.size(); ++i) {
        const QString trackOpId = QString("%1_TRACK_%2").arg(parentOpId).arg(i + 1);
        if (m_cancelRequested.load(std::memory_order_acquire)) {
            task.status = RenderStatus::Cancelled;
            task.errorMessage = "Render cancelled by user.";
            allTracksOk = false;
            break;
        }

        const QString& trackFile = task.trackFiles[i];
        task.currentTrackIndex = i;
        task.currentTrackName = trackFile;

        QString inputFilePath = task.sourceFolder + "/" + trackFile;
        QString baseName = QFileInfo(trackFile).completeBaseName();
        QString outputFilePath = task.stagingDir + "/" + baseName + ".wav";

        // P1-RND-01: Ensure destination file does not already exist in staging directory
        if (QFileInfo::exists(outputFilePath)) {
            allTracksOk = false;
            task.status = RenderStatus::Failed;
            task.errorMessage = QString("Output file already exists in staging directory: %1. "
                                        "Render aborted to prevent data overwrite.")
                                    .arg(outputFilePath);
            core::log::LogService::instance().log(
                "Render", "RenderTrack", "Failed",
                QString("Track failed: %1 - %2").arg(trackFile, task.errorMessage),
                "AudioFile", inputFilePath, "{}",
                trackOpId, parentOpId
            );
            break;
        }

        double trackFraction = 0.0;
        QString trackError;

        {
            const std::string progId = task.taskId.toStdString();
            const double progVal = static_cast<double>(i) / task.totalTracks;
            const int progIdx = i + 1;
            const int progTotal = task.totalTracks;
            const std::string progFile = trackFile.toStdString();
            QMetaObject::invokeMethod(this, [this, progId, progVal, progIdx, progTotal, progFile]() {
                emit renderProgressUpdated(QString::fromStdString(progId),
                                           progVal, progIdx, progTotal,
                                           QString::fromStdString(progFile));
            }, Qt::QueuedConnection);
        }

        bool ok = renderSingleFile(inputFilePath, outputFilePath, task.settings,
                                   task.dspSnapshot, m_cancelRequested, trackFraction, trackError);

        if (!ok) {
            allTracksOk = false;
            if (m_cancelRequested.load(std::memory_order_acquire)) {
                task.status = RenderStatus::Cancelled;
                task.errorMessage = "Render cancelled during processing: " + trackFile;
            } else {
                task.status = RenderStatus::Failed;
                task.errorMessage = QString("Render failed on track %1: %2").arg(trackFile, trackError);
            }
            core::log::LogService::instance().log(
                "Render", "RenderTrack", "Failed",
                QString("Track failed: %1 - %2").arg(trackFile, task.errorMessage),
                "AudioFile", inputFilePath, "{}",
                trackOpId, parentOpId
            );
            break;
        }

        {
            const std::string progId = task.taskId.toStdString();
            const double progVal = static_cast<double>(i + 1) / task.totalTracks;
            const int progIdx = i + 1;
            const int progTotal = task.totalTracks;
            const std::string progFile = trackFile.toStdString();
            QMetaObject::invokeMethod(this, [this, progId, progVal, progIdx, progTotal, progFile]() {
                emit renderProgressUpdated(QString::fromStdString(progId),
                                           progVal, progIdx, progTotal,
                                           QString::fromStdString(progFile));
            }, Qt::QueuedConnection);
        }
    }

    // 4. Transaction Resolution: Rollback vs Commit (P0-12)
    QFileInfo stagingFi(task.stagingDir);
    bool isValidStaging = task.stagingDir.startsWith(task.sourceFolder) &&
                          stagingFi.fileName().startsWith(".desk32_staging_");

    if (!allTracksOk || task.status == RenderStatus::Cancelled || task.status == RenderStatus::Failed) {
        // Rollback: delete all staged files and staging directory strictly within job scope
        if (isValidStaging && stagingFi.exists()) {
            bool removed = QDir(task.stagingDir).removeRecursively();
            if (!removed) {
                QString rollbackErr = QString("Rollback failed: unable to delete temporary staging directory: %1")
                                          .arg(task.stagingDir);
                task.errorMessage = task.errorMessage.isEmpty() ? rollbackErr : (task.errorMessage + " | " + rollbackErr);
                core::log::LogService::instance().log(
                    "Render", "Rollback", "Warning", rollbackErr, "FileSystem", task.stagingDir, "{}",
                    parentOpId, ""
                );
            } else {
                core::log::LogService::instance().log(
                    "Render", "Rollback", "Success",
                    "Rollback completed: deleted staging directory without modifying existing files.",
                    "FileSystem", task.stagingDir, "{}",
                    parentOpId, ""
                );
            }
        }
        return false;
    }

    // Commit: Rename staging directory to final output directory
    bool committed = false;
    if (isValidStaging && stagingFi.exists()) {
        committed = QDir().rename(task.stagingDir, task.finalOutputDir);
    }

    if (!committed) {
        task.status = RenderStatus::Failed;
        task.errorMessage = "Failed to commit staging directory to final output directory: " + task.finalOutputDir;
        if (isValidStaging && stagingFi.exists()) {
            bool removed = QDir(task.stagingDir).removeRecursively();
            if (!removed) {
                QString rollbackErr = QString("Rollback failed after commit failure: unable to delete %1").arg(task.stagingDir);
                task.errorMessage += " | " + rollbackErr;
                core::log::LogService::instance().log("Render", "Rollback", "Warning", rollbackErr, "FileSystem", task.stagingDir, "{}", parentOpId, "");
            }
        }
        core::log::LogService::instance().log(
            "Render", "Commit", "Failed", task.errorMessage, "FileSystem", task.finalOutputDir, "{}",
            parentOpId, ""
        );
        return false;
    }

    task.status = RenderStatus::Completed;
    task.progress = 1.0;

    core::log::LogService::instance().log(
        "Render",
        task.type == RenderType::Album ? "AlbumRender" : "TrackRender",
        "Success",
        QString("Render completed successfully: %1 tracks written to %2")
            .arg(task.totalTracks).arg(task.finalOutputDir),
        "RenderEngine",
        task.finalOutputDir,
        "{}",
        parentOpId, ""
    );

    return true;
}

bool RenderEngine::renderSingleFile(const QString& inputFilePath,
                                    const QString& outputFilePath,
                                    const RenderSettings& settings,
                                    const RenderDspSnapshot& dspSnapshot,
                                    std::atomic<bool>& cancelFlag,
                                    double& outProgressFraction,
                                    QString& outError)
{
    decoder::FFmpegAudioDecoder decoder;
    if (!decoder.open(inputFilePath.toStdString())) {
        outError = "Failed to open input audio file with FFmpegAudioDecoder: " + inputFilePath;
        return false;
    }

    const auto& info = decoder.formatInfo();
    double sourceRate = info.sampleRate > 0 ? info.sampleRate : 48000.0;
    double targetRate = (settings.rateMode == RenderSampleRateMode::Custom && settings.targetSampleRate > 0.0)
                            ? settings.targetSampleRate : sourceRate;

    // Resampler setup
    std::unique_ptr<dsp::Resampler> resampler;
    bool needsResample = std::abs(sourceRate - targetRate) > 0.01;
    if (needsResample) {
        resampler = std::make_unique<dsp::Resampler>();
        if (!resampler->configure(sourceRate, targetRate, 2, dsp::ResamplerQuality::VeryHigh)) {
            outError = "Failed to configure SoXR resampler for target rate " + QString::number(targetRate);
            return false;
        }
    }

    // RenderChain setup: configured strictly from job's immutable snapshot (P0-03)
    RenderChain chain;
    if (settings.enableMasterDsp) {
        chain.applySnapshot(dspSnapshot, targetRate);
    } else {
        chain.configure(targetRate);
        chain.setEnabled(false);
    }

    // WavFileWriter setup
    WavFileWriter writer;
    WavBitDepth depth = WavBitDepth::Int24;
    if (settings.bitDepth == RenderBitDepth::Int16) depth = WavBitDepth::Int16;
    else if (settings.bitDepth == RenderBitDepth::Float32) depth = WavBitDepth::Float32;

    if (!writer.open(outputFilePath, targetRate, depth)) {
        outError = "Failed to open WAV file for writing: " + outputFilePath;
        return false;
    }

    constexpr size_t BLOCK_FRAMES = 4096;
    alignas(64) std::array<double, BLOCK_FRAMES> inL{};
    alignas(64) std::array<double, BLOCK_FRAMES> inR{};

    std::vector<double> interIn;
    std::vector<double> interOut;
    std::vector<double> resampL;
    std::vector<double> resampR;

    if (needsResample) {
        size_t maxOutFrames = static_cast<size_t>(std::ceil(BLOCK_FRAMES * (targetRate / sourceRate) + 256));
        interIn.resize(BLOCK_FRAMES * 2);
        interOut.resize(maxOutFrames * 2);
        resampL.resize(maxOutFrames);
        resampR.resize(maxOutFrames);
    }

    uint64_t totalFrames = (info.totalFrames > 0) ? static_cast<uint64_t>(info.totalFrames) : 0ULL;
    uint64_t framesProcessed = 0;

    while (!cancelFlag.load(std::memory_order_relaxed)) {
        size_t framesRead = decoder.readFrames(inL.data(), inR.data(), BLOCK_FRAMES);
        if (framesRead == 0) {
            break; // EOF reached
        }

        if (needsResample) {
            // Interleave
            for (size_t i = 0; i < framesRead; ++i) {
                interIn[i * 2]     = inL[i];
                interIn[i * 2 + 1] = inR[i];
            }

            // P0-11: Guarantee full input consumption by processing in a loop until all inDone consumed
            size_t framesConsumedTotal = 0;
            size_t maxOut = interOut.size() / 2;

            while (framesConsumedTotal < framesRead && !cancelFlag.load(std::memory_order_relaxed)) {
                size_t inDone = 0;
                size_t outDone = 0;
                const double* inPtr = interIn.data() + framesConsumedTotal * 2;
                size_t inRemaining = framesRead - framesConsumedTotal;

                bool okResample = resampler->process(inPtr, inRemaining, inDone, interOut.data(), maxOut, outDone);
                if (!okResample) {
                    outError = "SoXR resampling failed during processing";
                    writer.close();
                    return false;
                }
                framesConsumedTotal += inDone;

                if (outDone > 0) {
                    // De-interleave
                    for (size_t i = 0; i < outDone; ++i) {
                        resampL[i] = interOut[i * 2];
                        resampR[i] = interOut[i * 2 + 1];
                    }

                    // Process through Render Chain
                    chain.process(resampL.data(), resampR.data(), outDone);

                    // Write to WAV
                    if (!writer.writeBlock(resampL.data(), resampR.data(), outDone, settings.applyDither)) {
                        outError = "Disk write failure during rendered WAV output";
                        writer.close();
                        return false;
                    }
                }

                if (inDone == 0 && outDone == 0) {
                    break;
                }
            }
        } else {
            // Direct planar processing
            chain.process(inL.data(), inR.data(), framesRead);

            // Write to WAV
            if (!writer.writeBlock(inL.data(), inR.data(), framesRead, settings.applyDither)) {
                outError = "Disk write failure during rendered WAV output";
                writer.close();
                return false;
            }
        }

        framesProcessed += framesRead;
        if (totalFrames > 0) {
            outProgressFraction = static_cast<double>(framesProcessed) / static_cast<double>(totalFrames);
        }
    }

    if (cancelFlag.load(std::memory_order_relaxed)) {
        writer.close();
        return false;
    }

    // P0-11: SoXR EOF Flush - Flush remaining delayed filter samples until empty
    if (needsResample && !cancelFlag.load(std::memory_order_relaxed)) {
        size_t outDone = 0;
        size_t maxOut = interOut.size() / 2;
        while (resampler->flush(interOut.data(), maxOut, outDone) && outDone > 0) {
            for (size_t i = 0; i < outDone; ++i) {
                resampL[i] = interOut[i * 2];
                resampR[i] = interOut[i * 2 + 1];
            }
            chain.process(resampL.data(), resampR.data(), outDone);
            if (!writer.writeBlock(resampL.data(), resampR.data(), outDone, settings.applyDither)) {
                outError = "Disk write failure during SoXR EOF flush";
                writer.close();
                return false;
            }
        }
    }

    writer.close();

    // Verify written file exists and has valid audio data
    QFileInfo outFi(outputFilePath);
    if (!outFi.exists() || outFi.size() <= 44) {
        outError = "Generated WAV file is invalid or zero-length: " + outputFilePath;
        return false;
    }

    return true;
}

} // namespace desk32::engine::render
