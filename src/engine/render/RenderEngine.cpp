#include "RenderEngine.hpp"
#include "engine/decoder/FFmpegAudioDecoder.hpp"
#include "engine/dsp/Resampler.hpp"
#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <QDateTime>
#include <QDebug>
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
    m_cancelRequested.store(true, std::memory_order_release);
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
}

void RenderEngine::workerLoop() {
    while (!m_stopWorker.load(std::memory_order_acquire)) {
        RenderTaskInfo task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this]() {
                return m_stopWorker.load(std::memory_order_acquire) || !m_taskQueue.empty();
            });

            if (m_stopWorker.load(std::memory_order_acquire)) {
                break;
            }

            task = m_taskQueue.front();
            m_taskQueue.pop();
            m_currentTask = task;
        }

        bool wasCancelRequested = m_cancelRequested.exchange(false, std::memory_order_acq_rel);
        m_isRendering.store(true, std::memory_order_release);

        emit renderTaskStarted(task.taskId, task.sourcePath, task.totalTracks);

        if (wasCancelRequested) {
            task.status = RenderStatus::Cancelled;
            task.errorMessage = "Render cancelled by user";
        } else {
            bool success = processTask(task);
            (void)success;
        }

        m_cancelRequested.store(false, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_currentTask = task;
        }

        m_isRendering.store(false, std::memory_order_release);

        emit renderTaskFinished(task.taskId, static_cast<int>(task.status),
                                task.finalOutputDir, task.errorMessage);
    }
}

bool RenderEngine::processTask(RenderTaskInfo& task) {
    const QString parentOpId = QString("RENDER_%1").arg(task.taskId);

    // 1. Pre-flight Check
    qint64 estimatedBytes = 0;
    for (const auto& f : task.trackFiles) {
        QFileInfo fi(task.sourceFolder + "/" + f);
        estimatedBytes += std::max<qint64>(fi.size() * 2, 10 * 1024 * 1024);
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
            QString("{\"error\":\"%1\"}").arg(preflightError)
        );
        return false;
    }

    // 2. Allocate non-reusable serial letter (Decision D-2)
    task.serial = RenderSequenceManager::instance().allocateNextSerial(task.sourceFolder);
    QDateTime now = QDateTime::currentDateTime();
    QString folderSuffix = RenderSequenceManager::formatFolderName(task.serial, now);

    task.stagingDir = task.sourceFolder + "/.desk32_staging_" + folderSuffix;
    task.finalOutputDir = task.sourceFolder + "/" + folderSuffix;

    // Create staging folder
    if (!QDir().mkpath(task.stagingDir)) {
        task.status = RenderStatus::Failed;
        task.errorMessage = "Failed to create temporary staging directory: " + task.stagingDir;
        core::log::LogService::instance().log(
            "Render", "CreateStagingDir", "Failed", task.errorMessage, "FileSystem", task.stagingDir
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
        task.finalOutputDir
    );

    // 3. Render tracks sequentially
    bool allTracksOk = true;

    for (int i = 0; i < task.trackFiles.size(); ++i) {
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

        double trackFraction = 0.0;
        QString trackError;

        emit renderProgressUpdated(task.taskId, static_cast<double>(i) / task.totalTracks,
                                   i + 1, task.totalTracks, trackFile);

        bool ok = renderSingleFile(inputFilePath, outputFilePath, task.settings,
                                   m_cancelRequested, trackFraction, trackError);

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
                "AudioFile", inputFilePath
            );
            break;
        }

        emit renderProgressUpdated(task.taskId, static_cast<double>(i + 1) / task.totalTracks,
                                   i + 1, task.totalTracks, trackFile);
    }

    // 4. Transaction Resolution: Rollback vs Commit
    if (!allTracksOk || task.status == RenderStatus::Cancelled || task.status == RenderStatus::Failed) {
        // Rollback: delete all staged files and staging directory completely
        bool removed = QDir(task.stagingDir).removeRecursively();
        if (!removed) {
            core::log::LogService::instance().log(
                "Render", "Rollback", "Warning",
                "Rollback incomplete: unable to delete temporary staging directory: " + task.stagingDir,
                "FileSystem", task.stagingDir
            );
        } else {
            core::log::LogService::instance().log(
                "Render", "Rollback", "Success",
                "Rollback completed: deleted staging directory without modifying existing files.",
                "FileSystem", task.stagingDir
            );
        }
        return false;
    }

    // Commit: Rename staging directory to final output directory
    bool committed = QDir().rename(task.stagingDir, task.finalOutputDir);
    if (!committed) {
        task.status = RenderStatus::Failed;
        task.errorMessage = "Failed to commit staging directory to final output directory: " + task.finalOutputDir;
        QDir(task.stagingDir).removeRecursively();
        core::log::LogService::instance().log(
            "Render", "Commit", "Failed", task.errorMessage, "FileSystem", task.finalOutputDir
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
        task.finalOutputDir
    );

    return true;
}

bool RenderEngine::renderSingleFile(const QString& inputFilePath,
                                    const QString& outputFilePath,
                                    const RenderSettings& settings,
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

    // RenderChain setup (cloned from live console template)
    RenderChain chain;
    chain.configure(targetRate);
    if (settings.enableMasterDsp) {
        chain.copyParametersFrom(
            m_templateChain.peq(),
            m_templateChain.geq(),
            m_templateChain.filter(),
            m_templateChain.compressor(),
            m_templateChain.reverb(),
            m_templateChain.limiter()
        );
    } else {
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
            break; // EOF
        }

        if (needsResample) {
            // Interleave
            for (size_t i = 0; i < framesRead; ++i) {
                interIn[i * 2]     = inL[i];
                interIn[i * 2 + 1] = inR[i];
            }

            size_t inDone = 0;
            size_t outDone = 0;
            size_t maxOut = interOut.size() / 2;
            resampler->process(interIn.data(), framesRead, inDone, interOut.data(), maxOut, outDone);

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
                return false;
            }
        } else {
            // Direct planar processing
            chain.process(inL.data(), inR.data(), framesRead);

            // Write to WAV
            if (!writer.writeBlock(inL.data(), inR.data(), framesRead, settings.applyDither)) {
                outError = "Disk write failure during rendered WAV output";
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
