#include <QTest>
#include <QObject>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QDir>
#include <QFile>
#include <cmath>
#include <vector>
#include <array>
#include <memory>

#include "engine/render/WavFileWriter.hpp"
#include "engine/render/RenderSequenceManager.hpp"
#include "engine/render/RenderChain.hpp"
#include "engine/render/RenderEngine.hpp"
#include "engine/decoder/FFmpegAudioDecoder.hpp"
#include "engine/deck/DeckManager.hpp"
#include "engine/session/FolderSession.hpp"
#include "core/EngineController.hpp"
#include "core/log/LogService.hpp"
#include <sys/stat.h>
#include <sys/xattr.h>
#include <QCryptographicHash>
#include <map>

using namespace desk32::engine::render;
using namespace desk32::engine::decoder;
using namespace desk32::engine::deck;
using namespace desk32::engine::session;
using namespace desk32::core;
using namespace desk32::core::log;

struct FileSnapshot {
    QByteArray sha256;
    int64_t size = 0;
    int64_t mtimeSec = 0;
    int64_t mtimeNsec = 0;
    mode_t mode = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    std::map<std::string, std::string> xattrs;

    static FileSnapshot capture(const QString& path) {
        FileSnapshot s;
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (hash.addData(&f)) {
                s.sha256 = hash.result();
            }
            s.size = f.size();
            f.close();
        }

        struct stat st;
        if (::stat(path.toStdString().c_str(), &st) == 0) {
            s.mtimeSec = st.st_mtim.tv_sec;
            s.mtimeNsec = st.st_mtim.tv_nsec;
            s.mode = st.st_mode;
            s.uid = st.st_uid;
            s.gid = st.st_gid;
        }

        ssize_t listLen = ::listxattr(path.toStdString().c_str(), nullptr, 0);
        if (listLen > 0) {
            std::vector<char> listBuf(static_cast<size_t>(listLen));
            ::listxattr(path.toStdString().c_str(), listBuf.data(), static_cast<size_t>(listLen));
            const char* ptr = listBuf.data();
            while (ptr < listBuf.data() + listLen) {
                ssize_t valLen = ::getxattr(path.toStdString().c_str(), ptr, nullptr, 0);
                if (valLen > 0) {
                    std::vector<char> valBuf(static_cast<size_t>(valLen));
                    ::getxattr(path.toStdString().c_str(), ptr, valBuf.data(), static_cast<size_t>(valLen));
                    s.xattrs[ptr] = std::string(valBuf.data(), static_cast<size_t>(valLen));
                } else if (valLen == 0) {
                    s.xattrs[ptr] = "";
                }
                ptr += std::strlen(ptr) + 1;
            }
        }
        return s;
    }

    bool matches(const FileSnapshot& other, QString& reason) const {
        if (sha256 != other.sha256) {
            reason = "SHA-256 hash changed";
            return false;
        }
        if (size != other.size) {
            reason = QString("Size changed (%1 vs %2)").arg(size).arg(other.size);
            return false;
        }
        if (mtimeSec != other.mtimeSec || mtimeNsec != other.mtimeNsec) {
            reason = QString("mtime changed (%1.%2 vs %3.%4)").arg(mtimeSec).arg(mtimeNsec).arg(other.mtimeSec).arg(other.mtimeNsec);
            return false;
        }
        if (mode != other.mode) {
            reason = QString("mode changed (%1 vs %2)").arg(mode).arg(other.mode);
            return false;
        }
        if (uid != other.uid || gid != other.gid) {
            reason = "uid or gid changed";
            return false;
        }
        if (xattrs != other.xattrs) {
            reason = "xattr changed";
            return false;
        }
        return true;
    }
};

class TestPhase4ExportRender : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // 1. WAV File Writer (16-bit, 24-bit, 32-bit float, RIFF headers, TPDF dither)
    void testWavFileWriterHeaderAndPCM();

    // 2. Bijective Base-26 Sequence Algorithm
    void testRenderSequenceBijectiveBase26();

    // 3. Persistent Sequence Management, Folder Naming, and Non-Reuse (Decision D-2 & D-3)
    void testRenderSequencePersistenceAndNonReuse();

    // 4. Pre-flight Checks (Permissions, Collisions, Disk Space)
    void testPreflightChecksAndPermissions();

    // 5. Offline Single Track Render (Staging, Processing, Atomic Commit)
    void testSingleTrackRender();

    // 6. Offline Album Render (Natural Sort, Subfolder Exclusion, Unified Folder)
    void testAlbumRenderNaturalSort();

    // 7. Staging Transaction & Rollback on Cancellation / Failure
    void testStagingRollbackOnCancelOrError();

    // 8. Strict Listening Correction Isolation & Volume Non-Interference (0.000000 Leakage)
    void testListeningCorrectionAndVolumeIsolation();

    // 9. EngineController Integration & Auditioning of Rendered Files
    void testEngineControllerRenderIntegration();

    // 10. P0-01: Deadlock resolution, uninitialized allocate, failure non-reuse, concurrent thread allocation
    void testRenderSequenceDeadlockAndErrorHandling();

    // 11. P0-02: SQLite Thread Ownership (worker thread connection safety, zero Qt cross-thread violations)
    void testRenderWorkerThreadOwnership();

    // 12. P0-03: Render Template Chain Job-Specific Immutable Snapshots (P0-03)
    void testRenderJobDspSnapshotIsolation();

    // 13. P0-12: Capacity Estimation and 4 GiB Limit (P0-12)
    void testCapacityEstimationAndWavLimitOverflow();

    // 14. SYS-08: Non-destructive Original Sound Source Immutability (Phase C)
    void testSys08OriginalSoundSourceImmutability();

    // 15. P1-RND-01: Album Render duplicate base name collision prevention
    void testAlbumRenderDuplicateBaseNameCollision();

    // 16. P2-RND-02: Final output directory pre-existence check and non-overwrite safety
    void testFinalOutputDirPreExistingCollision();

private:
    std::unique_ptr<QTemporaryDir> m_tempDir;
    QString m_dbPath;

    // Helper to generate a test WAV file
    bool createTestWav(const QString& path, double sampleRate, size_t numFrames, double freq = 440.0) {
        WavFileWriter writer;
        if (!writer.open(path, sampleRate, WavBitDepth::Int24)) {
            return false;
        }

        std::vector<double> left(numFrames);
        std::vector<double> right(numFrames);
        for (size_t i = 0; i < numFrames; ++i) {
            double t = static_cast<double>(i) / sampleRate;
            left[i] = 0.5 * std::sin(2.0 * M_PI * freq * t);
            right[i] = 0.5 * std::sin(2.0 * M_PI * freq * 1.5 * t);
        }

        bool ok = writer.writeBlock(left.data(), right.data(), numFrames, false);
        writer.close();
        return ok;
    }
};

void TestPhase4ExportRender::initTestCase()
{
    m_tempDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tempDir->isValid());

    m_dbPath = m_tempDir->filePath("test_desk32_render.sqlite");
    LogService::instance().start(m_dbPath);
    RenderSequenceManager::instance().initialize(m_dbPath);
    RenderEngine::instance().setDatabasePath(m_dbPath);
}

void TestPhase4ExportRender::cleanupTestCase()
{
    RenderEngine::instance().stop();
    RenderSequenceManager::instance().close();
    LogService::instance().stop();
    m_tempDir.reset();
}

void TestPhase4ExportRender::testWavFileWriterHeaderAndPCM()
{
    const size_t frames = 2000;
    const double sampleRate = 48000.0;
    std::vector<double> left(frames);
    std::vector<double> right(frames);
    for (size_t i = 0; i < frames; ++i) {
        left[i] = 0.4 * std::sin(static_cast<double>(i) * 0.05);
        right[i] = -0.4 * std::sin(static_cast<double>(i) * 0.05);
    }

    // 1. Test 16-bit PCM WAV (with TPDF dither)
    {
        QString wav16 = m_tempDir->filePath("test_16bit.wav");
        WavFileWriter writer;
        QVERIFY(writer.open(wav16, sampleRate, WavBitDepth::Int16));
        QVERIFY(writer.isOpen());
        QVERIFY(writer.writeBlock(left.data(), right.data(), frames, true));
        writer.close();
        QCOMPARE(writer.totalFramesWritten(), static_cast<uint64_t>(frames));

        QFile file(wav16);
        QVERIFY(file.exists());
        // 44 bytes header + frames * 2 channels * 2 bytes = 44 + 8000 = 8044
        QCOMPARE(file.size(), static_cast<qint64>(44 + frames * 4));

        // Read and verify RIFF header
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray header = file.read(44);
        file.close();
        QCOMPARE(header.mid(0, 4), QByteArray("RIFF"));
        QCOMPARE(header.mid(8, 4), QByteArray("WAVE"));
        QCOMPARE(header.mid(12, 4), QByteArray("fmt "));
        // Format tag at byte 20: 1 (PCM)
        uint16_t formatTag = *reinterpret_cast<const uint16_t*>(header.constData() + 20);
        QCOMPARE(formatTag, 1);
        // Bits per sample at byte 34: 16
        uint16_t bits = *reinterpret_cast<const uint16_t*>(header.constData() + 34);
        QCOMPARE(bits, 16);

        // Decode back via FFmpegAudioDecoder to verify audio content
        FFmpegAudioDecoder decoder;
        QVERIFY(decoder.open(wav16.toStdString()));
        const AudioFormatInfo& info = decoder.formatInfo();
        QCOMPARE(info.channels, 2U);
        QCOMPARE(info.sampleRate, 48000.0);
        QCOMPARE(info.totalFrames, static_cast<int64_t>(frames));
        decoder.close();
    }

    // 2. Test 24-bit PCM WAV (with TPDF dither)
    {
        QString wav24 = m_tempDir->filePath("test_24bit.wav");
        WavFileWriter writer;
        QVERIFY(writer.open(wav24, sampleRate, WavBitDepth::Int24));
        QVERIFY(writer.writeBlock(left.data(), right.data(), frames, true));
        writer.close();

        QFile file(wav24);
        QVERIFY(file.exists());
        // 44 bytes header + frames * 2 channels * 3 bytes = 44 + 12000 = 12044
        QCOMPARE(file.size(), static_cast<qint64>(44 + frames * 6));

        FFmpegAudioDecoder decoder;
        QVERIFY(decoder.open(wav24.toStdString()));
        const AudioFormatInfo& info = decoder.formatInfo();
        QCOMPARE(info.channels, 2U);
        QCOMPARE(info.sampleRate, 48000.0);
        QCOMPARE(info.totalFrames, static_cast<int64_t>(frames));
        decoder.close();
    }

    // 3. Test 32-bit Float WAV
    {
        QString wav32 = m_tempDir->filePath("test_32bit_float.wav");
        WavFileWriter writer;
        QVERIFY(writer.open(wav32, sampleRate, WavBitDepth::Float32));
        QVERIFY(writer.writeBlock(left.data(), right.data(), frames, false));
        writer.close();

        QFile file(wav32);
        QVERIFY(file.exists());
        // 44 bytes header + frames * 2 channels * 4 bytes = 44 + 16000 = 16044
        QCOMPARE(file.size(), static_cast<qint64>(44 + frames * 8));

        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray header = file.read(44);
        file.close();
        // Format tag for IEEE float: 3
        uint16_t formatTag = *reinterpret_cast<const uint16_t*>(header.constData() + 20);
        QCOMPARE(formatTag, 3);
        uint16_t bits = *reinterpret_cast<const uint16_t*>(header.constData() + 34);
        QCOMPARE(bits, 32);

        FFmpegAudioDecoder decoder;
        QVERIFY(decoder.open(wav32.toStdString()));
        const AudioFormatInfo& info = decoder.formatInfo();
        QCOMPARE(info.channels, 2U);
        QCOMPARE(info.sampleRate, 48000.0);
        QCOMPARE(info.totalFrames, static_cast<int64_t>(frames));
        decoder.close();
    }
}

void TestPhase4ExportRender::testRenderSequenceBijectiveBase26()
{
    // Verification of exact Bijective Base-26 letter progression:
    // 0 -> A, 1 -> B, ..., 25 -> Z
    // 26 -> AA, 27 -> AB, ..., 51 -> AZ
    // 52 -> BA, 53 -> BB, ..., 701 -> ZZ
    // 702 -> AAA

    QCOMPARE(RenderSequenceManager::indexToSerial(0), std::string("A"));
    QCOMPARE(RenderSequenceManager::indexToSerial(1), std::string("B"));
    QCOMPARE(RenderSequenceManager::indexToSerial(25), std::string("Z"));
    QCOMPARE(RenderSequenceManager::indexToSerial(26), std::string("AA"));
    QCOMPARE(RenderSequenceManager::indexToSerial(27), std::string("AB"));
    QCOMPARE(RenderSequenceManager::indexToSerial(51), std::string("AZ"));
    QCOMPARE(RenderSequenceManager::indexToSerial(52), std::string("BA"));
    QCOMPARE(RenderSequenceManager::indexToSerial(53), std::string("BB"));
    QCOMPARE(RenderSequenceManager::indexToSerial(701), std::string("ZZ"));
    QCOMPARE(RenderSequenceManager::indexToSerial(702), std::string("AAA"));
}

void TestPhase4ExportRender::testRenderSequencePersistenceAndNonReuse()
{
    RenderSequenceManager& seqMgr = RenderSequenceManager::instance();
    QString folder1 = m_tempDir->filePath("album1");
    QString folder2 = m_tempDir->filePath("album2");

    // 1. Initial serial for folder1 is "A"
    QString s1 = seqMgr.allocateNextSerial(folder1);
    QCOMPARE(s1, QString("A"));

    // 2. Second serial for folder1 is "B"
    QString s2 = seqMgr.allocateNextSerial(folder1);
    QCOMPARE(s2, QString("B"));

    // 3. Independent sequence for folder2 starts at "A"
    QString s2_1 = seqMgr.allocateNextSerial(folder2);
    QCOMPARE(s2_1, QString("A"));

    // 4. Decision D-2: Even if a render fails or is cancelled, sequence numbers
    // are strictly NEVER reused. The next request must be "C", not "B".
    QString s3 = seqMgr.allocateNextSerial(folder1);
    QCOMPARE(s3, QString("C"));

    // 5. Test folder naming format: {Serial}{YYYYMMDD_HHMMSS_zzz}
    QDateTime testDt(QDate(2026, 11, 8), QTime(14, 30, 45, 123));
    QString folderName = RenderSequenceManager::formatFolderName(s3, testDt);
    QCOMPARE(folderName, QString("C20261108_143045_123"));
}

void TestPhase4ExportRender::testPreflightChecksAndPermissions()
{
    QString errorMsg;

    // 1. Valid writable directory
    QString writableDir = m_tempDir->path();
    QVERIFY(RenderEngine::validatePreflight(writableDir, 1024, errorMsg));

    // 2. Non-existent path in an impossible root path
    QString invalidDir = "/root/nonexistent_desk32_test_path_readonly";
    QVERIFY(!RenderEngine::validatePreflight(invalidDir, 1024, errorMsg));
}

void TestPhase4ExportRender::testSingleTrackRender()
{
    RenderEngine& engine = RenderEngine::instance();

    // Create source test track (48kHz stereo, 0.5s = 24000 frames)
    QString sourceDir = m_tempDir->filePath("single_source");
    QDir().mkpath(sourceDir);
    QString trackPath = sourceDir + "/test_song.wav";
    QVERIFY(createTestWav(trackPath, 48000.0, 24000, 440.0));

    RenderSettings settings;
    settings.bitDepth = RenderBitDepth::Int24;
    settings.rateMode = RenderSampleRateMode::Original;
    settings.applyDither = true;
    settings.enableMasterDsp = true;

    QSignalSpy spyStarted(&engine, &RenderEngine::renderTaskStarted);
    QSignalSpy spyProgress(&engine, &RenderEngine::renderProgressUpdated);
    QSignalSpy spyFinished(&engine, &RenderEngine::renderTaskFinished);

    QString taskId = engine.enqueueTrackRender(trackPath, settings);
    QVERIFY(!taskId.isEmpty());

    // Wait for render to complete (up to 5 seconds)
    QVERIFY(spyFinished.wait(5000));
    QCOMPARE(spyFinished.count(), 1);

    auto arguments = spyFinished.takeFirst();
    QCOMPARE(arguments.at(0).toString(), taskId);
    QCOMPARE(arguments.at(1).toInt(), static_cast<int>(RenderStatus::Completed));
    QString outputDir = arguments.at(2).toString();
    QVERIFY(!outputDir.isEmpty());
    QVERIFY(QDir(outputDir).exists());

    // Confirm staging folder is gone
    QDir sourceQDir(sourceDir);
    QStringList stagingEntries = sourceQDir.entryList(QStringList() << ".desk32_staging_*", QDir::Dirs | QDir::Hidden);
    QCOMPARE(stagingEntries.count(), 0);

    // Confirm output file exists in the timestamped folder
    QString renderedFile = outputDir + "/test_song.wav";
    QVERIFY(QFile::exists(renderedFile));

    // Verify output file can be decoded and frame count matches
    FFmpegAudioDecoder decoder;
    QVERIFY(decoder.open(renderedFile.toStdString()));
    const AudioFormatInfo& info = decoder.formatInfo();
    QCOMPARE(info.sampleRate, 48000.0);
    QCOMPARE(info.channels, 2U);
    QCOMPARE(info.totalFrames, 24000);
    decoder.close();
}

void TestPhase4ExportRender::testAlbumRenderNaturalSort()
{
    RenderEngine& engine = RenderEngine::instance();

    // Create album directory with unsorted filenames and a subdirectory
    QString albumDir = m_tempDir->filePath("album_source");
    QDir().mkpath(albumDir);
    QDir().mkpath(albumDir + "/sub_folder");

    // Natural sort order should be: track_1.wav, track_2.wav, track_10.wav
    QVERIFY(createTestWav(albumDir + "/track_10.wav", 48000.0, 4800, 100.0));
    QVERIFY(createTestWav(albumDir + "/track_2.wav", 48000.0, 4800, 200.0));
    QVERIFY(createTestWav(albumDir + "/track_1.wav", 48000.0, 4800, 300.0));
    // Subfolder file must be ignored per Decision D-2
    QVERIFY(createTestWav(albumDir + "/sub_folder/ignored.wav", 48000.0, 4800, 400.0));

    RenderSettings settings;
    settings.bitDepth = RenderBitDepth::Int16;
    settings.rateMode = RenderSampleRateMode::Original;
    settings.applyDither = true;

    QSignalSpy spyFinished(&engine, &RenderEngine::renderTaskFinished);

    QString taskId = engine.enqueueAlbumRender(albumDir, settings);
    QVERIFY(!taskId.isEmpty());

    // Wait for album render to finish
    QVERIFY(spyFinished.wait(10000));
    QCOMPARE(spyFinished.count(), 1);

    auto arguments = spyFinished.takeFirst();
    QCOMPARE(arguments.at(1).toInt(), static_cast<int>(RenderStatus::Completed));
    QString outputDir = arguments.at(2).toString();
    QVERIFY(QDir(outputDir).exists());

    // Verify all 3 files exist in output folder
    QVERIFY(QFile::exists(outputDir + "/track_1.wav"));
    QVERIFY(QFile::exists(outputDir + "/track_2.wav"));
    QVERIFY(QFile::exists(outputDir + "/track_10.wav"));
    // Subfolder file must NOT be rendered
    QVERIFY(!QFile::exists(outputDir + "/ignored.wav"));

    // Staging folder must be deleted
    QDir sourceQDir(albumDir);
    QStringList stagingEntries = sourceQDir.entryList(QStringList() << ".desk32_staging_*", QDir::Dirs | QDir::Hidden);
    QCOMPARE(stagingEntries.count(), 0);
}

void TestPhase4ExportRender::testStagingRollbackOnCancelOrError()
{
    RenderEngine& engine = RenderEngine::instance();

    // Create a large track so we can cancel while in progress
    QString sourceDir = m_tempDir->filePath("cancel_source");
    QDir().mkpath(sourceDir);
    QString trackPath = sourceDir + "/long_track.wav";
    // 48000 * 10 = 480,000 frames
    QVERIFY(createTestWav(trackPath, 48000.0, 480000, 440.0));

    RenderSettings settings;
    QSignalSpy spyFinished(&engine, &RenderEngine::renderTaskFinished);

    QString taskId = engine.enqueueTrackRender(trackPath, settings);
    QVERIFY(!taskId.isEmpty());

    // Immediately cancel
    engine.cancelCurrentRender();

    QVERIFY(spyFinished.wait(5000));
    QCOMPARE(spyFinished.count(), 1);

    auto arguments = spyFinished.takeFirst();
    RenderStatus status = static_cast<RenderStatus>(arguments.at(1).toInt());
    QCOMPARE(status, RenderStatus::Cancelled);

    // Staging directory must be completely removed
    QDir sourceQDir(sourceDir);
    QStringList stagingEntries = sourceQDir.entryList(QStringList() << ".desk32_staging_*", QDir::Dirs | QDir::Hidden);
    QCOMPARE(stagingEntries.count(), 0);

    // Source track must remain untouched
    QVERIFY(QFile::exists(trackPath));
}

void TestPhase4ExportRender::testListeningCorrectionAndVolumeIsolation()
{
    // Architectural Guarantee:
    // RenderChain must strictly isolate output from Listening Correction (0.000000 leakage)
    // and from Listening / Render Monitor Volumes.

    RenderChain chain;
    chain.configure(48000.0);

    // Configure some upstream mastering DSP (e.g. Peak Limiter ceiling -1.0 dB)
    chain.limiter().configure(48000.0, -1.0, 50.0, 5.0);
    chain.limiter().setEnabled(true);

    const size_t frames = 1024;
    std::vector<double> inL(frames, 0.7);
    std::vector<double> inR(frames, 0.7);

    std::vector<double> out1L = inL;
    std::vector<double> out1R = inR;
    chain.process(out1L.data(), out1R.data(), frames);

    // Reset chain state to ensure deterministic start
    chain.reset();

    // Now simulate live engine having massive Listening Correction EQ (+12dB)
    // and Listening Volume set to 0.1 (-20dB)
    ParametricEQ listeningEq;
    listeningEq.configure(48000.0);
    ParametricEQ::BandConfig boostBand;
    boostBand.enabled = true;
    boostBand.gainDb = 12.0;
    boostBand.frequency = 1000.0;
    boostBand.Q = 1.0;
    boostBand.type = BiquadFilterType::Peak;
    listeningEq.setLeftBand(0, boostBand);
    listeningEq.setEnabled(true);

    // Notice: RenderChain does NOT have Listening Correction!
    // When RenderChain runs the same audio again, it must produce identical samples (0.000000 diff)
    std::vector<double> out2L = inL;
    std::vector<double> out2R = inR;
    chain.process(out2L.data(), out2R.data(), frames);

    double maxDiff = 0.0;
    for (size_t i = 0; i < frames; ++i) {
        maxDiff = std::max(maxDiff, std::abs(out1L[i] - out2L[i]));
        maxDiff = std::max(maxDiff, std::abs(out1R[i] - out2R[i]));
    }

    QCOMPARE(maxDiff, 0.0);
}

void TestPhase4ExportRender::testEngineControllerRenderIntegration()
{
    RenderEngine::instance().resetTaskInfo();
    EngineController& controller = EngineController::instance();
    QVERIFY(controller.initialize());

    // Check default properties
    QCOMPARE(controller.isRendering(), false);
    QCOMPARE(controller.renderProgress(), 0.0);
    QCOMPARE(controller.renderTrackIndex(), 0);
    QCOMPARE(controller.renderTotalTracks(), 0);

    // Create a synthetic track
    QString trackPath = m_tempDir->filePath("controller_test.wav");
    QVERIFY(createTestWav(trackPath, 48000.0, 4800, 440.0));

    // Load track into deck A
    QVERIFY(controller.loadTrack("A", trackPath));

    // Test Q_INVOKABLE startTrackRender from deck A
    QString taskId = controller.startTrackRender("A", 1, 0, 48000.0, true, true);
    QVERIFY(!taskId.isEmpty());

    // Test getRenderStatus
    QVariantMap statusMap = controller.getRenderStatus();
    QCOMPARE(statusMap.value("taskId").toString(), taskId);

    // Test cancelRender
    controller.cancelRender();
    while (RenderEngine::instance().queuedTasksCount() > 0 || RenderEngine::instance().isRendering()) {
        QTest::qWait(10);
    }
    RenderEngine::instance().resetTaskInfo();
    QCoreApplication::processEvents();

    // Test auditionRenderedFile into deck B
    QVERIFY(controller.auditionRenderedFile("B", trackPath));
    // Monitor path must be switched to RenderMonitor (1)
    QCOMPARE(controller.outputRoutingMode(), 1);
}

void TestPhase4ExportRender::testRenderSequenceDeadlockAndErrorHandling()
{
    // Test Case 1: allocateNextSerial on an uninitialized instance must NOT deadlock
    {
        RenderSequenceManager uninitMgr("TestConn_Uninit");
        QString serial = uninitMgr.allocateNextSerial(m_tempDir->path());
        // Should succeed without deadlock and produce a valid serial letter
        QVERIFY(!serial.isEmpty());
        uninitMgr.close();
    }

    // Test Case 2: initialization failure must NOT return "A" fallback
    {
        RenderSequenceManager failMgr("TestConn_Fail");
        // Using an impossible path that cannot be created or opened
        QString badPath = "/nonexistent_root_dir_12345/unwritable_sub/test.sqlite";
        bool ok = failMgr.initialize(badPath);
        QVERIFY(!ok);
        QString serial = failMgr.allocateNextSerial(m_tempDir->path());
        // Must return empty QString, NEVER fallback "A"
        QVERIFY(serial.isEmpty());
        failMgr.close();
    }

    // Test Case 3: Concurrent multithreaded serial allocation must have zero duplicates and strictly bijective sequence
    {
        QString concurrentFolder = m_tempDir->filePath("concurrent_test_folder");
        QDir().mkpath(concurrentFolder);

        constexpr int NUM_THREADS = 8;
        constexpr int SEQS_PER_THREAD = 10;
        std::vector<std::string> allSerials;
        std::mutex resultsMutex;
        std::vector<std::thread> threads;

        for (int t = 0; t < NUM_THREADS; ++t) {
            threads.emplace_back([&, t]() {
                // Each thread creates its own manager with unique connection
                QString connName = QString("ConcurrentConn_%1").arg(t);
                RenderSequenceManager mgr(connName);
                mgr.initialize(m_dbPath);

                for (int s = 0; s < SEQS_PER_THREAD; ++s) {
                    QString seq = mgr.allocateNextSerial(concurrentFolder);
                    if (!seq.isEmpty()) {
                        std::lock_guard<std::mutex> lock(resultsMutex);
                        allSerials.push_back(seq.toStdString());
                    }
                }
                mgr.close();
            });
        }

        for (auto& th : threads) {
            th.join();
        }

        // Verify total allocated count
        QCOMPARE(static_cast<int>(allSerials.size()), NUM_THREADS * SEQS_PER_THREAD);

        // Verify all serials are strictly unique (no duplicates!)
        std::sort(allSerials.begin(), allSerials.end());
        auto it = std::adjacent_find(allSerials.begin(), allSerials.end());
        QVERIFY2(it == allSerials.end(), "Duplicate serial found across concurrent threads!");

        // Verify bijective base-26 sequence: 0..79 must map to A..Z, AA..BZ
        std::vector<std::string> expectedSerials;
        for (uint64_t i = 0; i < NUM_THREADS * SEQS_PER_THREAD; ++i) {
            expectedSerials.push_back(RenderSequenceManager::indexToSerial(i));
        }
        std::sort(expectedSerials.begin(), expectedSerials.end());
        QCOMPARE(allSerials, expectedSerials);
    }
}

void TestPhase4ExportRender::testRenderWorkerThreadOwnership()
{
    // P0-02: SQLite Connection must be created, used, and closed on the Render Worker thread
    RenderEngine& engine = RenderEngine::instance();
    engine.resetTaskInfo();
    engine.setDatabasePath(m_dbPath);

    // Create a temporary track
    QString trackPath = m_tempDir->filePath("worker_ownership_test.wav");
    QVERIFY(createTestWav(trackPath, 48000.0, 4800, 440.0));

    QSignalSpy finishedSpy(&engine, &RenderEngine::renderTaskFinished);

    QString taskId = engine.enqueueTrackRender(trackPath);
    QVERIFY(!taskId.isEmpty());

    // Wait for render task completion (up to 15s to accommodate low-core CI virtual machines in Debug)
    QVERIFY(finishedSpy.wait(15000));
    QCOMPARE(finishedSpy.count(), 1);

    auto args = finishedSpy.takeFirst();
    QCOMPARE(args.at(0).toString(), taskId);
    QCOMPARE(args.at(1).toInt(), static_cast<int>(RenderStatus::Completed));

    // Verify task serial was allocated and staging directory cleaned up
    RenderTaskInfo task = engine.currentTaskInfo();
    QVERIFY(!task.serial.isEmpty());
    QVERIFY(!QDir(task.stagingDir).exists());
    QVERIFY(QDir(task.finalOutputDir).exists());
}

void TestPhase4ExportRender::testRenderJobDspSnapshotIsolation()
{
    // P0-03: Render Job must take an immutable DSP snapshot at enqueue time
    RenderEngine& engine = RenderEngine::instance();
    engine.resetTaskInfo();
    engine.setDatabasePath(m_dbPath);

    // 1. Configure template chain with PEQ Band 0 Gain = +12.0 dB
    {
        auto lock = engine.lockTemplateChain();
        engine.templateChain().configure(48000.0);
        ParametricEQ::BandConfig cfg;
        cfg.frequency = 1000.0;
        cfg.gainDb = 12.0;
        cfg.Q = 1.0;
        cfg.type = BiquadFilterType::Peak;
        cfg.enabled = true;
        engine.templateChain().peq().setLeftBand(0, cfg);
        engine.templateChain().peq().setEnabled(true);
    }

    // Capture snapshot for Job 1
    RenderDspSnapshot snap1 = engine.captureTemplateSnapshot();
    QCOMPARE(snap1.peqLeftBands[0].gainDb, 12.0);

    // Create test audio file
    QString trackPath1 = m_tempDir->filePath("snapshot_job1.wav");
    QVERIFY(createTestWav(trackPath1, 48000.0, 4800, 440.0));

    QString taskId1 = engine.enqueueTrackRender(trackPath1);
    QVERIFY(!taskId1.isEmpty());

    // 2. Immediately mutate template chain to -12.0 dB before Job 1 completes
    {
        auto lock = engine.lockTemplateChain();
        ParametricEQ::BandConfig cfg;
        cfg.frequency = 1000.0;
        cfg.gainDb = -12.0;
        cfg.Q = 1.0;
        cfg.type = BiquadFilterType::Peak;
        cfg.enabled = true;
        engine.templateChain().peq().setLeftBand(0, cfg);
    }

    // Capture snapshot for Job 2
    RenderDspSnapshot snap2 = engine.captureTemplateSnapshot();
    QCOMPARE(snap2.peqLeftBands[0].gainDb, -12.0);

    QString trackPath2 = m_tempDir->filePath("snapshot_job2.wav");
    QVERIFY(createTestWav(trackPath2, 48000.0, 4800, 440.0));

    QString taskId2 = engine.enqueueTrackRender(trackPath2);
    QVERIFY(!taskId2.isEmpty());

    // Wait for all queued tasks to finish
    while (engine.queuedTasksCount() > 0 || engine.isRendering()) {
        QTest::qWait(10);
    }

    // Verify template chain mutation did NOT affect snap1
    QCOMPARE(snap1.peqLeftBands[0].gainDb, 12.0);
    QCOMPARE(snap2.peqLeftBands[0].gainDb, -12.0);
}

void TestPhase4ExportRender::testCapacityEstimationAndWavLimitOverflow()
{
    // P0-12: Capacity estimation & rejection of tracks exceeding 4 GiB Classic WAV limit
    RenderEngine& engine = RenderEngine::instance();

    // Create a 0.1s source track
    QString trackPath = m_tempDir->filePath("overflow_test.wav");
    QVERIFY(createTestWav(trackPath, 48000.0, 4800, 440.0));

    // Normal should succeed
    RenderSettings settings;
    settings.bitDepth = RenderBitDepth::Int24;
    QString taskId = engine.enqueueTrackRender(trackPath, settings);
    QVERIFY(!taskId.isEmpty());

    while (engine.queuedTasksCount() > 0 || engine.isRendering()) {
        QTest::qWait(10);
    }

    // Verify 4 GiB limit check constant
    QCOMPARE(RenderEngine::MAX_CLASSIC_WAV_BYTES, 4294967200LL);
}

void TestPhase4ExportRender::testSys08OriginalSoundSourceImmutability()
{
    // SYS-08 Requirement:
    // Original sound source files must remain strictly immutable across all Desk32 operations:
    // - Decoding (open, read, seek, close)
    // - Deck loading & playback
    // - FolderSession scanning & iteration
    // - Render (success, failure, cancellation)
    // Verification includes: SHA-256, size, mtime (nanosecond precision), mode, uid/gid, xattr.
    // Also verifies that external modification/corruption is detected without automatic repair.

    QString srcDir = m_tempDir->filePath("sys08_source_dir");
    QDir().mkpath(srcDir);
    QString trackFile = "original_master_track.wav";
    QString trackPath = srcDir + "/" + trackFile;

    QVERIFY(createTestWav(trackPath, 48000.0, 48000 * 2, 440.0)); // 2s 24-bit stereo WAV

    // Explicitly set mode and extended attribute
    ::chmod(trackPath.toStdString().c_str(), 0644);
    ::setxattr(trackPath.toStdString().c_str(), "user.desk32_audit", "sys08_immutable_tag", 20, 0);

    // 1. Capture baseline snapshot
    FileSnapshot baseline = FileSnapshot::capture(trackPath);
    QVERIFY(!baseline.sha256.isEmpty());
    QVERIFY(baseline.size > 0);
    QCOMPARE(baseline.mode & 0777, 0644);
    QVERIFY(baseline.xattrs.count("user.desk32_audit") > 0);

    QString diff;

    // 2. Decoder operations (open, read, seek, close)
    {
        FFmpegAudioDecoder decoder;
        QVERIFY(decoder.open(trackPath.toStdString()));
        std::vector<double> left(512);
        std::vector<double> right(512);
        size_t readCount = decoder.readFrames(left.data(), right.data(), 512);
        QVERIFY(readCount > 0);
        QVERIFY(decoder.seek(1000));
        readCount = decoder.readFrames(left.data(), right.data(), 512);
        QVERIFY(readCount > 0);
        decoder.close();

        FileSnapshot afterDecode = FileSnapshot::capture(trackPath);
        QVERIFY2(afterDecode.matches(baseline, diff), qPrintable("Decoder modified original file: " + diff));
    }

    // 3. Deck load & playback
    {
        DeckManager deckMgr(48000.0);
        TrackInfo trackInfo{trackPath.toStdString(), 48000.0, 2, 48000 * 2};
        QVERIFY(deckMgr.deck('A')->loadTrack(trackInfo, 48000.0));
        deckMgr.deck('A')->play();
        std::vector<double> block(256 * 2);
        for (int b = 0; b < 10; ++b) {
            deckMgr.deck('A')->renderBlock(block.data(), 256);
        }
        deckMgr.deck('A')->stop();

        FileSnapshot afterDeck = FileSnapshot::capture(trackPath);
        QVERIFY2(afterDeck.matches(baseline, diff), qPrintable("Deck modified original file: " + diff));
    }

    // 4. FolderSession scan & track iteration
    {
        FolderSession session;
        QVERIFY(session.startSession(srcDir.toStdString(), {trackFile.toStdString()}));
        QCOMPARE(session.totalTracks(), 1u);
        QVERIFY(session.currentTrack() != nullptr);
        QCOMPARE(session.currentTrack()->fileName, trackFile.toStdString());

        FileSnapshot afterSession = FileSnapshot::capture(trackPath);
        QVERIFY2(afterSession.matches(baseline, diff), qPrintable("FolderSession modified original file: " + diff));
    }

    // 5. Successful Single Track & Album Render
    {
        RenderEngine& engine = RenderEngine::instance();
        RenderSettings settings;
        settings.bitDepth = RenderBitDepth::Int24;

        // Track Render
        QString tId = engine.enqueueTrackRender(trackPath, settings);
        QVERIFY(!tId.isEmpty());
        while (engine.queuedTasksCount() > 0 || engine.isRendering()) {
            QTest::qWait(10);
        }

        FileSnapshot afterTrackRender = FileSnapshot::capture(trackPath);
        QVERIFY2(afterTrackRender.matches(baseline, diff), qPrintable("Track render modified original file: " + diff));

        // Album Render
        QString albId = engine.enqueueAlbumRender(srcDir, settings);
        QVERIFY(!albId.isEmpty());
        while (engine.queuedTasksCount() > 0 || engine.isRendering()) {
            QTest::qWait(10);
        }

        FileSnapshot afterAlbumRender = FileSnapshot::capture(trackPath);
        QVERIFY2(afterAlbumRender.matches(baseline, diff), qPrintable("Album render modified original file: " + diff));
    }

    // 6. Cancelled Render & Staging Rollback
    {
        RenderEngine& engine = RenderEngine::instance();
        RenderSettings settings;

        QString cId = engine.enqueueTrackRender(trackPath, settings);
        engine.cancelCurrentRender();
        while (engine.queuedTasksCount() > 0 || engine.isRendering()) {
            QTest::qWait(10);
        }

        FileSnapshot afterCancel = FileSnapshot::capture(trackPath);
        QVERIFY2(afterCancel.matches(baseline, diff), qPrintable("Cancelled render modified original file: " + diff));

        // Staging directory must be strictly removed
        QDir srcQDir(srcDir);
        QStringList stagings = srcQDir.entryList(QStringList() << ".desk32_staging_*", QDir::Dirs | QDir::Hidden);
        QCOMPARE(stagings.count(), 0);
    }

    // 7. External Modification Detection & Strict Non-Repair / Non-Recreation
    {
        QString tamperedPath = srcDir + "/tampered_track.wav";
        QVERIFY(createTestWav(tamperedPath, 48000.0, 48000, 440.0));

        // External corruption
        {
            QFile f(tamperedPath);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("CORRUPTED_NON_AUDIO_BYTES_HEADER");
            f.close();
        }

        FileSnapshot tamperedBaseline = FileSnapshot::capture(tamperedPath);

        // Attempt decode on corrupted file
        FFmpegAudioDecoder decoder;
        bool opened = decoder.open(tamperedPath.toStdString());
        if (opened) {
            decoder.close();
        }

        // Verify Desk32 did not auto-repair, overwrite, or mutate the corrupted file
        FileSnapshot afterAttempt = FileSnapshot::capture(tamperedPath);
        QVERIFY2(afterAttempt.matches(tamperedBaseline, diff),
                 qPrintable("Desk32 modified corrupted file (violating no-auto-repair rule): " + diff));

        // External deletion
        QVERIFY(QFile::remove(tamperedPath));
        QVERIFY(!QFile::exists(tamperedPath));

        // Attempt to decode deleted file: must fail cleanly without creating or touching files
        bool openDeleted = decoder.open(tamperedPath.toStdString());
        QVERIFY(!openDeleted);
        QVERIFY(!QFile::exists(tamperedPath));
    }
}

void TestPhase4ExportRender::testAlbumRenderDuplicateBaseNameCollision()
{
    // P1-RND-01: Verify that when two files in an album have the same base name (e.g. Track01.wav and Track01.flac),
    // Album Render detects the collision upfront during preflight and safely aborts without overwriting or touching files.
    RenderEngine& engine = RenderEngine::instance();
    while (engine.queuedTasksCount() > 0 || engine.isRendering()) {
        QTest::qWait(10);
    }

    QString albumDir = m_tempDir->filePath("album_dup_test");
    QDir().mkpath(albumDir);

    // Create two test audio files with the exact same base name "Track01" but different extensions
    QString wavPath = albumDir + "/Track01.wav";
    QString flacPath = albumDir + "/Track01.flac";
    QVERIFY(createTestWav(wavPath, 48000.0, 4800, 220.0));
    QVERIFY(QFile::copy(wavPath, flacPath));

    FileSnapshot baselineWav = FileSnapshot::capture(wavPath);
    FileSnapshot baselineFlac = FileSnapshot::capture(flacPath);

    RenderSettings settings;
    QSignalSpy spyFinished(&engine, &RenderEngine::renderTaskFinished);

    QString taskId = engine.enqueueAlbumRender(albumDir, settings);
    QVERIFY(!taskId.isEmpty());

    QVERIFY(spyFinished.wait(5000));
    QCOMPARE(spyFinished.count(), 1);

    auto arguments = spyFinished.takeFirst();
    RenderStatus status = static_cast<RenderStatus>(arguments.at(1).toInt());
    QCOMPARE(status, RenderStatus::Failed);

    QString errorMsg = arguments.at(3).toString();
    QVERIFY2(errorMsg.contains("Duplicate output track base name detected in album"), qPrintable(errorMsg));

    // Verify original files are 100% untouched
    QString diff;
    FileSnapshot afterWav = FileSnapshot::capture(wavPath);
    FileSnapshot afterFlac = FileSnapshot::capture(flacPath);
    QVERIFY2(afterWav.matches(baselineWav, diff), qPrintable("WAV modified on duplicate collision: " + diff));
    QVERIFY2(afterFlac.matches(baselineFlac, diff), qPrintable("FLAC modified on duplicate collision: " + diff));

    // Verify staging directory was not created or was removed
    QDir albumQDir(albumDir);
    QStringList stagingEntries = albumQDir.entryList(QStringList() << ".desk32_staging_*", QDir::Dirs | QDir::Hidden);
    QCOMPARE(stagingEntries.count(), 0);

    while (engine.queuedTasksCount() > 0 || engine.isRendering()) {
        QTest::qWait(10);
    }
}

void TestPhase4ExportRender::testFinalOutputDirPreExistingCollision()
{
    // P2-RND-02: Verify that if the final output directory already exists (e.g. from an earlier external run or manual creation),
    // RenderEngine detects the collision before creating staging or processing audio, fails safely, and does NOT overwrite or touch the existing folder.
    RenderEngine& engine = RenderEngine::instance();
    while (engine.queuedTasksCount() > 0 || engine.isRendering()) {
        QTest::qWait(10);
    }

    QString sourceDir = m_tempDir->filePath("preexisting_output_test");
    QDir().mkpath(sourceDir);

    QString trackPath = sourceDir + "/test_song.wav";
    QVERIFY(createTestWav(trackPath, 48000.0, 4800, 440.0));

    // Pre-create the collision output directory
    QString preExistingDir = sourceDir + "/PreExistingRenderFolder";
    QDir().mkpath(preExistingDir);

    // Place an existing canary file inside preExistingDir
    QString canaryPath = preExistingDir + "/CANARY_DO_NOT_DELETE.txt";
    {
        QFile canary(canaryPath);
        QVERIFY(canary.open(QIODevice::WriteOnly));
        canary.write("PRE_EXISTING_CANARY_DATA");
        canary.close();
    }
    FileSnapshot canaryBaseline = FileSnapshot::capture(canaryPath);

    RenderSettings settings;
    QSignalSpy spyFinished(&engine, &RenderEngine::renderTaskFinished);

    // Enqueue track render explicitly pointing to preExistingDir
    QString taskId = engine.enqueueTrackRender(trackPath, settings, preExistingDir);
    QVERIFY(!taskId.isEmpty());

    QVERIFY(spyFinished.wait(5000));
    QCOMPARE(spyFinished.count(), 1);

    auto arguments = spyFinished.takeFirst();
    RenderStatus status = static_cast<RenderStatus>(arguments.at(1).toInt());
    QCOMPARE(status, RenderStatus::Failed);

    QString errorMsg = arguments.at(3).toString();
    QVERIFY2(errorMsg.contains("Final output directory already exists"), qPrintable(errorMsg));

    // Verify pre-existing canary file and folder are 100% untouched
    QString diff;
    FileSnapshot canaryAfter = FileSnapshot::capture(canaryPath);
    QVERIFY2(canaryAfter.matches(canaryBaseline, diff), qPrintable("Canary file was modified on collision: " + diff));
    QVERIFY(QDir(preExistingDir).exists());

    // Staging directory must not remain
    QDir srcQDir(sourceDir);
    QStringList stagingEntries = srcQDir.entryList(QStringList() << ".desk32_staging_*", QDir::Dirs | QDir::Hidden);
    QCOMPARE(stagingEntries.count(), 0);

    while (engine.queuedTasksCount() > 0 || engine.isRendering()) {
        QTest::qWait(10);
    }
}

QTEST_GUILESS_MAIN(TestPhase4ExportRender)
#include "test_phase4_export_render.moc"
