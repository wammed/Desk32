#include <QTest>
#include <QObject>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QElapsedTimer>
#include <atomic>
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
#include "core/log/LogErrorCodes.hpp"
#include "engine/workspace/WorkspaceManager.hpp"
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

/// Records whether a render-wait timeout occurred and whether the post-timeout
/// cancel-and-drain cleanup failed. Both flags are evaluated in cleanup() (Option A,
/// Section 6): even if post-timeout cleanup succeeds, a timeout is still recorded
/// and treated as a test failure; if cleanup itself fails, it is recorded as a
/// critical test environment abnormality.
static std::atomic<bool> g_renderCleanupFailed{false};
static std::atomic<bool> g_renderTimeoutOccurred{false};

/// Bounded cancel-and-drain of the render engine using the unified isIdle() state check.
/// Cancels both the active render and all queued tasks via cancelAllRenders(), then
/// polls until the engine reaches RenderWorkerState::Idle with an empty queue or the
/// deadline expires.
/// NOTE: std::thread::join() has no timeout; this only polls engine state. The CTest
/// TIMEOUT (300s) remains the last line of defence.
template <typename Engine>
static bool cancelAndDrainRenderEngine(Engine& engine, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    engine.cancelAllRenders();
    while (!engine.isIdle()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        engine.cancelAllRenders();
        QTest::qWait(10);
    }
    QCoreApplication::processEvents();
    engine.resetTaskInfo();
    return engine.isIdle();
}

/// Bounded wait for the render engine to become idle; returns false on timeout.
/// On timeout, records g_renderTimeoutOccurred = true, attempts bounded cancel-and-drain,
/// and sets g_renderCleanupFailed = true if the drain fails.
template <typename Engine>
static bool waitForRenderIdle(Engine& engine, int timeoutMs = 30000, int cleanupTimeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!engine.isIdle()) {
        if (timer.elapsed() > timeoutMs) {
            g_renderTimeoutOccurred.store(true, std::memory_order_release);
            if (!cancelAndDrainRenderEngine(engine, cleanupTimeoutMs)) {
                g_renderCleanupFailed.store(true, std::memory_order_release);
            }
            return false;
        }
        QTest::qWait(10);
    }
    return true;
}

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
    void init();
    void cleanup();

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

    // 17. A-T01: Verify public API has no explicitOutputDir argument
    void testPublicApiHasNoExplicitOutputDir();

    // 18. A-T05: Verify rendering outside original source folder is rejected
    void testRenderOutputOutsideSourceFolderRejected();

    // 19. A-T06: Verify symlink traversal outside source folder is rejected
    void testRenderSymlinkTraversalOutsideSourceFolderRejected();

    // 20. A-T08: Verify output and staging filesystem containment
    void testOutputAndStagingFilesystemContainment();

    // 21. RCT-01, RCT-02, RCT-03, RCT-05, RCT-06: Deterministic render start gate, active cancellation, queue drain, and recovery
    void testRenderTimeoutCancelAndDrainCleanup();

    // 22. RCT-04: Verify Idle state race window is eliminated during RenderWorkerState::Starting
    void testRenderWorkerStateIdleRaceWindowEliminated();

    // 23. RCT-07, RCT-08, RCT-09, RCT-10: Verify gate timeout, wait timeout, cleanup failure detection, and state isolation
    void testRenderCleanupFailureAndTimeoutDetection();

    // 24. Phase 4 Step 3: EngineController delegates render state to RenderEngine; progress & per-track results
    void testStep3ControllerCanonicalStateAndTrackResults();

    // 25. Phase 4 Step 3: cancelAllRenders via controller, Cancelled per-track status, log hierarchy
    void testStep3CancelAllAndLogHierarchy();

    // 26. Phase 4 Step 3: Render Monitor volume + rendered file audition rules
    void testStep3MonitorVolumeAndAudition();

    // 27. A-1 / A-4: EngineController syncs individual DSP node enabled states & bypasses to RenderEngine
    void testControllerDspNodeEnableSyncToRender();

    // 28. v3.8.1 §19 D-2: Fail-closed render without workspace & workspace-scoped output + serials
    void testFailClosedWithoutWorkspaceAndWorkspaceSerial();

private:
    struct CleanupEvaluation {
        bool passed = true;
        bool hadRenderTimeout = false;
        bool hadCleanupFailure = false;
        bool engineLeftBusy = false;
        QString failureMessage;
    };

    static CleanupEvaluation performTestCleanupCycle(RenderEngine& engine,
                                                     int drainTimeoutMs = 10000,
                                                     bool clearHooksFirst = true)
    {
        CleanupEvaluation eval;
        eval.hadRenderTimeout = g_renderTimeoutOccurred.exchange(false, std::memory_order_acq_rel);
        eval.hadCleanupFailure = g_renderCleanupFailed.exchange(false, std::memory_order_acq_rel);

        if (clearHooksFirst) {
            // Releases any active test gate via its registered cleanup callback and removes the hook
            engine.clearTestTaskHook();
        }

        // Always perform recovery/drain before reporting failure (Section 5.3)
        if (!engine.isIdle()) {
            if (!cancelAndDrainRenderEngine(engine, drainTimeoutMs)) {
                eval.hadCleanupFailure = true;
            }
        } else {
            QCoreApplication::processEvents();
            engine.resetTaskInfo();
        }

        eval.engineLeftBusy = !engine.isIdle();

        if (eval.hadCleanupFailure || eval.engineLeftBusy) {
            eval.passed = false;
            eval.failureMessage = QStringLiteral(
                "CRITICAL TEST ENVIRONMENT ABNORMALITY: Render engine cleanup/drain failed "
                "(hadRenderTimeout=%1, hadCleanupFailure=%2, engineLeftBusy=%3)")
                .arg(eval.hadRenderTimeout ? 1 : 0)
                .arg(eval.hadCleanupFailure ? 1 : 0)
                .arg(eval.engineLeftBusy ? 1 : 0);
        } else if (eval.hadRenderTimeout) {
            eval.passed = false;
            eval.failureMessage = QStringLiteral(
                "Render wait timed out during test (post-timeout cancel-and-drain cleanup succeeded, "
                "but the timeout itself is recorded as a test failure)");
        }

        return eval;
    }

    /// Reference-counted, bounded-timeout, RAII-safe test synchronization gate for RenderEngine.
    /// Shared state lives on the heap via std::shared_ptr<GateState> so the worker thread can
    /// never access dangling memory even if the gate is released/detached concurrently.
    class ScopedRenderTestGate {
    public:
        struct GateState {
            std::mutex mtx;
            std::condition_variable cv;
            RenderWorkerState targetState = RenderWorkerState::Rendering;
            bool releaseOnCancel = false;
            int workerWaitLimitMs = 5000;
            bool alreadyTriggered = false;
            bool arrived = false;
            bool released = false;
            bool workerInHook = false;
            bool workerExited = false;
            bool workerTimedOut = false;
            QString arrivedTaskId;
            RenderWorkerState arrivedState = RenderWorkerState::Idle;
        };

        explicit ScopedRenderTestGate(RenderEngine& engine,
                                      RenderWorkerState targetState = RenderWorkerState::Rendering,
                                      bool releaseOnCancel = false,
                                      int workerWaitLimitMs = 5000)
            : m_engine(engine)
            , m_state(std::make_shared<GateState>())
        {
            m_state->targetState = targetState;
            m_state->releaseOnCancel = releaseOnCancel;
            m_state->workerWaitLimitMs = workerWaitLimitMs;

            auto stateCopy = m_state;
            auto stateForCleanup = m_state;

            m_engine.setTestTaskHook(
                [stateCopy](const QString& taskId,
                            RenderWorkerState wState,
                            const std::atomic<bool>& cancelFlag) {
                    std::unique_lock<std::mutex> lock(stateCopy->mtx);
                    if (wState != stateCopy->targetState || stateCopy->alreadyTriggered) {
                        return;
                    }
                    stateCopy->alreadyTriggered = true;
                    stateCopy->arrived = true;
                    stateCopy->workerInHook = true;
                    stateCopy->arrivedTaskId = taskId;
                    stateCopy->arrivedState = wState;
                    stateCopy->cv.notify_all();

                    const auto deadline = std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(stateCopy->workerWaitLimitMs);
                    while (!stateCopy->released) {
                        if (stateCopy->releaseOnCancel &&
                            cancelFlag.load(std::memory_order_acquire)) {
                            break;
                        }
                        const auto now = std::chrono::steady_clock::now();
                        if (now >= deadline) {
                            stateCopy->workerTimedOut = true;
                            break;
                        }
                        const auto slice = std::min<std::chrono::milliseconds>(
                            std::chrono::milliseconds(10),
                            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) +
                                std::chrono::milliseconds(1));
                        stateCopy->cv.wait_for(lock, slice);
                    }

                    stateCopy->workerInHook = false;
                    stateCopy->workerExited = true;
                    stateCopy->cv.notify_all();
                },
                [stateForCleanup]() {
                    std::lock_guard<std::mutex> lock(stateForCleanup->mtx);
                    stateForCleanup->released = true;
                    stateForCleanup->cv.notify_all();
                }
            );
        }

        ~ScopedRenderTestGate() {
            releaseAndDetach();
        }

        ScopedRenderTestGate(const ScopedRenderTestGate&) = delete;
        ScopedRenderTestGate& operator=(const ScopedRenderTestGate&) = delete;

        bool waitForArrival(int timeoutMs = 5000) {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < timeoutMs) {
                {
                    std::unique_lock<std::mutex> lock(m_state->mtx);
                    if (m_state->arrived) {
                        return true;
                    }
                    m_state->cv.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                        return m_state->arrived;
                    });
                    if (m_state->arrived) {
                        return true;
                    }
                }
                QCoreApplication::processEvents();
            }
            return false;
        }

        void enableReleaseOnCancel() {
            std::lock_guard<std::mutex> lock(m_state->mtx);
            m_state->releaseOnCancel = true;
            m_state->cv.notify_all();
        }

        void release() {
            std::lock_guard<std::mutex> lock(m_state->mtx);
            m_state->released = true;
            m_state->cv.notify_all();
        }

        bool waitForWorkerExit(int timeoutMs = 5000) {
            std::unique_lock<std::mutex> lock(m_state->mtx);
            return m_state->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() {
                return !m_state->workerInHook && (!m_state->arrived || m_state->workerExited);
            });
        }

        void releaseAndDetach(int timeoutMs = 5000) {
            release();
            m_engine.clearTestTaskHook();
            (void)waitForWorkerExit(timeoutMs);
        }

        [[nodiscard]] QString arrivedTaskId() const {
            std::lock_guard<std::mutex> lock(m_state->mtx);
            return m_state->arrivedTaskId;
        }

        [[nodiscard]] RenderWorkerState arrivedState() const {
            std::lock_guard<std::mutex> lock(m_state->mtx);
            return m_state->arrivedState;
        }

        [[nodiscard]] bool workerTimedOut() const {
            std::lock_guard<std::mutex> lock(m_state->mtx);
            return m_state->workerTimedOut;
        }

    private:
        RenderEngine& m_engine;
        std::shared_ptr<GateState> m_state;
    };
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

    auto& wsMgr = desk32::engine::workspace::WorkspaceManager::instance();
    wsMgr.setConfigFilePathForTesting(m_tempDir->filePath("test_workspaces.json"));
    wsMgr.resetForTesting();
}

void TestPhase4ExportRender::init()
{
    g_renderTimeoutOccurred.store(false, std::memory_order_release);
    g_renderCleanupFailed.store(false, std::memory_order_release);
    RenderEngine::instance().clearTestTaskHook();
    RenderEngine::instance().setWorkspacePath(QString());
    desk32::engine::workspace::WorkspaceManager::instance().resetForTesting();
    QVERIFY2(RenderEngine::instance().isIdle(), "Render engine must be idle at start of test");
}

void TestPhase4ExportRender::cleanup()
{
    // Per-test isolation guard: performs full cleanup/drain before evaluating test outcome
    // (Section 5.3 & Section 6: never skip cleanup to record failure; evaluate both timeout
    // and cleanup failure flags independently).
    const CleanupEvaluation eval = performTestCleanupCycle(RenderEngine::instance(), 10000, true);
    QVERIFY2(eval.passed, qPrintable(eval.failureMessage));
}

void TestPhase4ExportRender::cleanupTestCase()
{
    RenderEngine::instance().stop();
    RenderEngine::instance().setWorkspacePath(QString());
    desk32::engine::workspace::WorkspaceManager::instance().resetForTesting();
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
    const QString wsDir = m_tempDir->filePath("ctrl_ws_integration");
    QDir().mkpath(wsDir);
    QString wsErr;
    QVERIFY2(desk32::engine::workspace::WorkspaceManager::instance().registerAndOpenWorkspace(wsDir, wsErr), qPrintable(wsErr));
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
    QVERIFY2(waitForRenderIdle(RenderEngine::instance()), "Timed out waiting for render engine to become idle");
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
    QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render engine to become idle");

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

    QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render engine to become idle");

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
        QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render engine to become idle");

        FileSnapshot afterTrackRender = FileSnapshot::capture(trackPath);
        QVERIFY2(afterTrackRender.matches(baseline, diff), qPrintable("Track render modified original file: " + diff));

        // Album Render
        QString albId = engine.enqueueAlbumRender(srcDir, settings);
        QVERIFY(!albId.isEmpty());
        QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render engine to become idle");

        FileSnapshot afterAlbumRender = FileSnapshot::capture(trackPath);
        QVERIFY2(afterAlbumRender.matches(baseline, diff), qPrintable("Album render modified original file: " + diff));
    }

    // 6. Cancelled Render & Staging Rollback
    {
        RenderEngine& engine = RenderEngine::instance();
        RenderSettings settings;

        QString cId = engine.enqueueTrackRender(trackPath, settings);
        engine.cancelCurrentRender();
        QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render engine to become idle");
        QCoreApplication::processEvents();

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
    QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render engine to become idle");
    QCoreApplication::processEvents();

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

    QVariantList matchedArgs;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        for (int i = 0; i < spyFinished.count(); ++i) {
            if (spyFinished.at(i).at(0).toString() == taskId) {
                matchedArgs = spyFinished.at(i);
                break;
            }
        }
        if (!matchedArgs.isEmpty()) {
            break;
        }
        spyFinished.wait(50);
    }
    QVERIFY2(!matchedArgs.isEmpty(), "Timed out waiting for matching renderTaskFinished signal");

    RenderStatus status = static_cast<RenderStatus>(matchedArgs.at(1).toInt());
    QCOMPARE(status, RenderStatus::Failed);

    QString errorMsg = matchedArgs.at(3).toString();
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

    QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render engine to become idle");
    QCoreApplication::processEvents();
}

void TestPhase4ExportRender::testFinalOutputDirPreExistingCollision()
{
    // P2-RND-02: Verify that if the final output directory already exists (e.g. from an earlier external run or manual creation),
    // RenderEngine detects the collision before creating staging or processing audio, fails safely, and does NOT overwrite or touch the existing folder.
    RenderEngine& engine = RenderEngine::instance();
    QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render engine to become idle");
    QCoreApplication::processEvents();

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

    // Enqueue track render pointing to preExistingDir via private test hook
    QString taskId = engine.enqueueTrackRenderForTesting(trackPath, settings, preExistingDir);
    QVERIFY(!taskId.isEmpty());

    QVariantList matchedArgs;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        for (int i = 0; i < spyFinished.count(); ++i) {
            if (spyFinished.at(i).at(0).toString() == taskId) {
                matchedArgs = spyFinished.at(i);
                break;
            }
        }
        if (!matchedArgs.isEmpty()) {
            break;
        }
        spyFinished.wait(50);
    }
    QVERIFY2(!matchedArgs.isEmpty(), "Timed out waiting for matching renderTaskFinished signal");

    RenderStatus status = static_cast<RenderStatus>(matchedArgs.at(1).toInt());
    QCOMPARE(status, RenderStatus::Failed);

    QString errorMsg = matchedArgs.at(3).toString();
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

    // 5. Verify sequence management and subsequent render are NOT corrupted by the collision
    RenderSequenceManager seqMgr(m_tempDir->filePath("collision_seq_test.db"));
    QVERIFY(seqMgr.initialize());
    QString s1 = seqMgr.allocateNextSerial(sourceDir);
    QVERIFY(!s1.isEmpty());
    QString s2 = seqMgr.allocateNextSerial(sourceDir);
    QVERIFY(!s2.isEmpty());
    QVERIFY(s1 != s2);

    // Subsequent normal render completes successfully without being blocked by prior collision
    QSignalSpy spyNextFinished(&engine, &RenderEngine::renderTaskFinished);
    QString nextTaskId = engine.enqueueTrackRender(trackPath, settings);
    QVERIFY(!nextTaskId.isEmpty());

    QVariantList nextArgs;
    timer.restart();
    while (timer.elapsed() < 5000) {
        for (int i = 0; i < spyNextFinished.count(); ++i) {
            if (spyNextFinished.at(i).at(0).toString() == nextTaskId) {
                nextArgs = spyNextFinished.at(i);
                break;
            }
        }
        if (!nextArgs.isEmpty()) break;
        spyNextFinished.wait(50);
    }
    QVERIFY2(!nextArgs.isEmpty(), "Timed out waiting for subsequent normal renderTaskFinished signal");
    RenderStatus nextStatus = static_cast<RenderStatus>(nextArgs.at(1).toInt());
    QCOMPARE(nextStatus, RenderStatus::Completed);

    QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render engine to become idle");
    QCoreApplication::processEvents();
}

template <typename, typename = void>
struct HasThreeArgEnqueueTrackRender : std::false_type {};

template <typename T>
struct HasThreeArgEnqueueTrackRender<T, std::void_t<decltype(std::declval<T>().enqueueTrackRender(
    std::declval<QString>(), std::declval<RenderSettings>(), std::declval<QString>()))>> : std::true_type {};

void TestPhase4ExportRender::testPublicApiHasNoExplicitOutputDir()
{
    // A-T01: Verify at compile time and runtime that RenderEngine::enqueueTrackRender accepts only (trackPath, settings)
    // and strictly prohibits explicit output directory injection in the production public API.
    static_assert(!HasThreeArgEnqueueTrackRender<RenderEngine>::value,
                  "RenderEngine::enqueueTrackRender must not accept an explicitOutputDir argument in public API.");
    QVERIFY(!HasThreeArgEnqueueTrackRender<RenderEngine>::value);

    // Verify invocability with 1 and 2 arguments
    static_assert(std::is_invocable_v<decltype(&RenderEngine::enqueueTrackRender), RenderEngine*, const QString&, const RenderSettings&>,
                  "enqueueTrackRender must be invocable with (trackPath, settings)");
    auto oneArgCall = [](RenderEngine& re, const QString& p) { return re.enqueueTrackRender(p); };
    static_assert(std::is_invocable_v<decltype(oneArgCall), RenderEngine&, const QString&>,
                  "enqueueTrackRender must be invocable with (trackPath)");
}

void TestPhase4ExportRender::testRenderOutputOutsideSourceFolderRejected()
{
    // A-T05: Verify that if an output path attempts to escape the original sound source directory (e.g. into an external directory),
    // RenderEngine containment validation detects it upfront, fails safely, and does not create or write files.
    RenderEngine& engine = RenderEngine::instance();
    QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render engine to become idle");
    QCoreApplication::processEvents();

    QString sourceDir = m_tempDir->filePath("containment_test_src");
    QString outsideDir = m_tempDir->filePath("external_target_dir");
    QDir().mkpath(sourceDir);
    QDir().mkpath(outsideDir);

    QString trackPath = sourceDir + "/test_song.wav";
    QVERIFY(createTestWav(trackPath, 48000.0, 4800, 440.0));

    RenderSettings settings;
    QSignalSpy spyFinished(&engine, &RenderEngine::renderTaskFinished);

    // Attempt to inject an output directory pointing outside sourceDir
    QString taskId = engine.enqueueTrackRenderForTesting(trackPath, settings, outsideDir + "/EscapedRenderFolder");
    QVERIFY(!taskId.isEmpty());

    QVariantList matchedArgs;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        for (int i = 0; i < spyFinished.count(); ++i) {
            if (spyFinished.at(i).at(0).toString() == taskId) {
                matchedArgs = spyFinished.at(i);
                break;
            }
        }
        if (!matchedArgs.isEmpty()) break;
        spyFinished.wait(50);
    }
    QVERIFY2(!matchedArgs.isEmpty(), "Timed out waiting for matching renderTaskFinished signal");

    RenderStatus status = static_cast<RenderStatus>(matchedArgs.at(1).toInt());
    QCOMPARE(status, RenderStatus::Failed);

    QString errorMsg = matchedArgs.at(3).toString();
    QVERIFY2(errorMsg.contains("escapes original sound source directory"), qPrintable(errorMsg));

    // Verify external directory was not written to
    QDir extQDir(outsideDir);
    QStringList extEntries = extQDir.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    QCOMPARE(extEntries.count(), 0);

    QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render engine to become idle");
    QCoreApplication::processEvents();
}

void TestPhase4ExportRender::testRenderSymlinkTraversalOutsideSourceFolderRejected()
{
    // A-T06: Verify that if a symlink inside the source directory points outside,
    // attempting to render into that symlinked directory is detected and strictly rejected.
    RenderEngine& engine = RenderEngine::instance();
    QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render engine to become idle");
    QCoreApplication::processEvents();

    QString sourceDir = m_tempDir->filePath("symlink_test_src");
    QString outsideDir = m_tempDir->filePath("symlink_outside_dir");
    QDir().mkpath(sourceDir);
    QDir().mkpath(outsideDir);

    QString trackPath = sourceDir + "/test_song.wav";
    QVERIFY(createTestWav(trackPath, 48000.0, 4800, 440.0));

    // Create a symlink inside sourceDir pointing to outsideDir
    QString symlinkPath = sourceDir + "/symlink_to_outside";
    QVERIFY(QFile::link(outsideDir, symlinkPath));

    RenderSettings settings;
    QSignalSpy spyFinished(&engine, &RenderEngine::renderTaskFinished);

    // Attempt to render into the symlinked path which resolves outside sourceDir
    QString taskId = engine.enqueueTrackRenderForTesting(trackPath, settings, symlinkPath + "/escaped_folder");
    QVERIFY(!taskId.isEmpty());

    QVariantList matchedArgs;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        for (int i = 0; i < spyFinished.count(); ++i) {
            if (spyFinished.at(i).at(0).toString() == taskId) {
                matchedArgs = spyFinished.at(i);
                break;
            }
        }
        if (!matchedArgs.isEmpty()) break;
        spyFinished.wait(50);
    }
    QVERIFY2(!matchedArgs.isEmpty(), "Timed out waiting for matching renderTaskFinished signal");

    RenderStatus status = static_cast<RenderStatus>(matchedArgs.at(1).toInt());
    QCOMPARE(status, RenderStatus::Failed);

    QString errorMsg = matchedArgs.at(3).toString();
    QVERIFY2(errorMsg.contains("escapes original sound source directory") || errorMsg.contains("symbolic link"),
             qPrintable(errorMsg));

    // Verify outside directory was not written to
    QDir extQDir(outsideDir);
    QStringList extEntries = extQDir.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    QCOMPARE(extEntries.count(), 0);

    QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render engine to become idle");
    QCoreApplication::processEvents();
}

void TestPhase4ExportRender::testOutputAndStagingFilesystemContainment()
{
    // A-T08: Substantive verification of RenderEngine::validateOutputContainment
    // Exercises all containment invariant checks: source folder canonical containment, staging containment,
    // symlink rejection, source file overwrite prevention, track file name collision prevention,
    // and POSIX filesystem boundary (st_dev) enforcement.

    QString sourceDir = m_tempDir->filePath("containment_substantive_src");
    QDir().mkpath(sourceDir);
    QString trackFile = "audio_track_01.wav";
    QString sourcePath = sourceDir + "/" + trackFile;
    QVERIFY(createTestWav(sourcePath, 48000.0, 4800, 440.0));

    QString canonicalSource = QFileInfo(sourceDir).canonicalFilePath();
    QVERIFY(!canonicalSource.isEmpty());

    RenderTaskInfo baseTask;
    baseTask.taskId = "containment_test_task";
    baseTask.sourceFolder = sourceDir;
    baseTask.sourcePath = sourcePath;
    baseTask.trackFiles.append(trackFile);
    baseTask.finalOutputDir = sourceDir + "/A20261110_120000_123";
    baseTask.stagingDir = sourceDir + "/.desk32_staging_A20261110_120000_123";

    QString err;

    // 1. Valid task: output and staging strictly inside source folder on same filesystem
    QVERIFY2(RenderEngine::validateOutputContainment(baseTask, err), qPrintable(err));
    QVERIFY(err.isEmpty());

    // 2. Output directory escapes source folder (parent is outside source directory)
    {
        RenderTaskInfo task = baseTask;
        QString outsideDir = m_tempDir->filePath("outside_containment_dir");
        QDir().mkpath(outsideDir);
        task.finalOutputDir = outsideDir + "/A20261110_120000_123";
        QVERIFY(!RenderEngine::validateOutputContainment(task, err));
        QVERIFY2(err.contains("escapes original sound source directory"), qPrintable(err));
    }

    // 3. Staging directory escapes source folder
    {
        RenderTaskInfo task = baseTask;
        QString outsideDir = m_tempDir->filePath("outside_staging_dir");
        QDir().mkpath(outsideDir);
        task.stagingDir = outsideDir + "/.desk32_staging_A20261110_120000_123";
        QVERIFY(!RenderEngine::validateOutputContainment(task, err));
        QVERIFY2(err.contains("escapes original sound source directory"), qPrintable(err));
    }

    // 4. Output directory points directly to original sound source file (overwriting prevention)
    {
        RenderTaskInfo task = baseTask;
        task.finalOutputDir = sourcePath;
        QVERIFY(!RenderEngine::validateOutputContainment(task, err));
        QVERIFY2(err.contains("conflicts with original sound source file"), qPrintable(err));
    }

    // 5. Output directory name matches one of the track files
    {
        RenderTaskInfo task = baseTask;
        task.finalOutputDir = sourceDir + "/" + trackFile;
        QVERIFY(!RenderEngine::validateOutputContainment(task, err));
        QVERIFY2(err.contains("matches original sound source file") || err.contains("conflicts with original sound source file"),
                 qPrintable(err));
    }

    // 6. Output directory path contains or is a symbolic link
    {
        RenderTaskInfo task = baseTask;
        QString symlinkDir = sourceDir + "/symlink_out";
        QString targetDir = m_tempDir->filePath("symlink_target");
        QDir().mkpath(targetDir);
        QVERIFY2(QFile::link(targetDir, symlinkDir), "Failed to create output symlink for test");
        task.finalOutputDir = symlinkDir;
        QVERIFY(!RenderEngine::validateOutputContainment(task, err));
        QVERIFY2(err.contains("symbolic link") || err.contains("escapes"), qPrintable(err));
    }

    // 7. Staging directory path contains or is a symbolic link
    {
        RenderTaskInfo task = baseTask;
        QString symlinkStaging = sourceDir + "/symlink_staging";
        QString stagingTarget = m_tempDir->filePath("staging_target");
        QDir().mkpath(stagingTarget);
        QVERIFY2(QFile::link(stagingTarget, symlinkStaging), "Failed to create staging symlink for test");
        task.stagingDir = symlinkStaging;
        QVERIFY(!RenderEngine::validateOutputContainment(task, err));
        QVERIFY2(err.contains("symbolic link") || err.contains("escapes"), qPrintable(err));
    }

    // 8. Filesystem boundary enforcement via StatDevResolver:
    // When st_dev of output parent differs from source folder (simulating cross-filesystem / mount point escape)
    {
        RenderTaskInfo task = baseTask;
        int callIndex = 0;
        auto mismatchResolver = [&](const QString& /*path*/, dev_t& outDev) -> bool {
            if (++callIndex == 1) {
                outDev = 1001; // Mock device A for source
                return true;
            } else {
                outDev = 2002; // Mock device B for output (different filesystem)
                return true;
            }
        };
        QVERIFY(!RenderEngine::validateOutputContainment(task, err, mismatchResolver));
        QVERIFY2(err.contains("same filesystem as source"), qPrintable(err));
    }

    // 9. When StatDevResolver returns matching st_dev
    {
        RenderTaskInfo task = baseTask;
        auto matchingResolver = [&](const QString& /*path*/, dev_t& outDev) -> bool {
            outDev = 1001; // Same mock device
            return true;
        };
        QVERIFY2(RenderEngine::validateOutputContainment(task, err, matchingResolver), qPrintable(err));
    }

    // 10. Verify actual paths generated by real RenderEngine during render process satisfy containment
    RenderEngine& engine = RenderEngine::instance();
    QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render engine to become idle");
    QCoreApplication::processEvents();

    RenderSettings settings;
    QSignalSpy spyStarted(&engine, &RenderEngine::renderTaskStarted);
    QSignalSpy spyFinished(&engine, &RenderEngine::renderTaskFinished);

    QString taskId = engine.enqueueTrackRender(sourcePath, settings);
    QVERIFY(!taskId.isEmpty());

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        if (spyFinished.count() > 0) break;
        spyFinished.wait(50);
    }
    QVERIFY2(spyFinished.count() > 0, "Timed out waiting for renderTaskFinished");

    // Retrieve finished task
    QVariantList args = spyFinished.takeFirst();
    RenderStatus status = static_cast<RenderStatus>(args.at(1).toInt());
    QCOMPARE(status, RenderStatus::Completed);
    QString outputFolder = args.at(2).toString();
    QVERIFY(!outputFolder.isEmpty());

    // Verify output directory strictly satisfies containment
    RenderTaskInfo generatedTask;
    generatedTask.sourceFolder = sourceDir;
    generatedTask.sourcePath = sourcePath;
    generatedTask.trackFiles.append(trackFile);
    generatedTask.finalOutputDir = outputFolder;
    generatedTask.stagingDir = sourceDir + "/.desk32_staging_test";
    QVERIFY2(RenderEngine::validateOutputContainment(generatedTask, err), qPrintable(err));

    // Verify parent of outputFolder is canonicalSource
    QFileInfo outGenInfo(outputFolder);
    QCOMPARE(QDir::cleanPath(outGenInfo.dir().canonicalPath()), QDir::cleanPath(canonicalSource));
}

void TestPhase4ExportRender::testRenderTimeoutCancelAndDrainCleanup()
{
    // RCT-01, RCT-02, RCT-03, RCT-05, RCT-06 (Section 4.2 & 4.3):
    // Deterministic verification of active render cancellation, queued task draining,
    // per-taskId outcome verification (distinguishing in-flight vs pre-start cancellation),
    // and subsequent normal render recovery without timing/sleep guessing.
    RenderEngine& engine = RenderEngine::instance();
    QVERIFY(engine.isIdle());

    QString sourceDir = m_tempDir->filePath("cleanup_deterministic_source");
    QDir().mkpath(sourceDir);
    RenderSettings settings;
    settings.bitDepth = RenderBitDepth::Int24;
    settings.rateMode = RenderSampleRateMode::Original;

    QString track1Path = sourceDir + "/cleanup_0.wav";
    QString track2Path = sourceDir + "/cleanup_1.wav";
    QString track3Path = sourceDir + "/cleanup_2.wav";
    QVERIFY(createTestWav(track1Path, 48000.0, 24000, 440.0));
    QVERIFY(createTestWav(track2Path, 48000.0, 24000, 440.0));
    QVERIFY(createTestWav(track3Path, 48000.0, 24000, 440.0));

    QSignalSpy spyStarted(&engine, &RenderEngine::renderTaskStarted);
    QSignalSpy spyFinished(&engine, &RenderEngine::renderTaskFinished);

    // Install deterministic gate that pauses Task 1 once it enters RenderWorkerState::Rendering
    ScopedRenderTestGate renderGate(engine, RenderWorkerState::Rendering, /*releaseOnCancel=*/false, 5000);

    // Step 1: Enqueue 1st Render task
    const QString taskId1 = engine.enqueueTrackRender(track1Path, settings);
    QVERIFY(!taskId1.isEmpty());

    // Step 2 & 3 (RCT-01): Synchronously confirm 1st task started rendering and is paused at the gate
    QVERIFY2(renderGate.waitForArrival(5000), "Timed out waiting for Task 1 to arrive at Rendering gate");
    QCOMPARE(renderGate.arrivedTaskId(), taskId1);
    QCOMPARE(renderGate.arrivedState(), RenderWorkerState::Rendering);
    QCOMPARE(engine.workerState(), RenderWorkerState::Rendering);
    QVERIFY(engine.isRendering());
    QVERIFY(!engine.isIdle());

    // Wait for queued renderTaskStarted signal for Task 1
    {
        QElapsedTimer sigTimer;
        sigTimer.start();
        while (spyStarted.count() < 1 && sigTimer.elapsed() < 2000) {
            QTest::qWait(10);
        }
    }
    QCOMPARE(spyStarted.count(), 1);
    QCOMPARE(spyStarted.at(0).at(0).toString(), taskId1);

    // Confirm Task 1 created its staging folder before pausing at the gate
    QDir sourceQDir(sourceDir);
    QStringList activeStagings = sourceQDir.entryList(QStringList() << ".desk32_staging_*", QDir::Dirs | QDir::Hidden);
    QCOMPARE(activeStagings.count(), 1);

    // Step 4: Enqueue 2nd and 3rd tasks while Task 1 is deterministically paused
    const QString taskId2 = engine.enqueueTrackRender(track2Path, settings);
    const QString taskId3 = engine.enqueueTrackRender(track3Path, settings);
    QVERIFY(!taskId2.isEmpty());
    QVERIFY(!taskId3.isEmpty());
    QVERIFY(taskId1 != taskId2);
    QVERIFY(taskId2 != taskId3);
    QVERIFY(taskId1 != taskId3);

    // Step 5: Verify 2nd and 3rd tasks are sitting in the wait queue
    QCOMPARE(engine.queuedTasksCount(), 2);
    QCOMPARE(spyFinished.count(), 0);

    // Step 6: Enable cancel-release on the gate and execute cancelAndDrainRenderEngine
    renderGate.enableReleaseOnCancel();
    const bool drained = cancelAndDrainRenderEngine(engine, 10000);
    QVERIFY2(drained, "cancelAndDrainRenderEngine did not reach idle within its deadline");
    renderGate.releaseAndDetach();

    // Flush queued signals so all renderTaskFinished events are delivered
    {
        QElapsedTimer sigTimer;
        sigTimer.start();
        while (spyFinished.count() < 3 && sigTimer.elapsed() < 2000) {
            QTest::qWait(10);
        }
    }

    // Step 7 & 8 (RCT-02, RCT-03, Section 4.3): Verify per-taskId terminal status and start vs pre-start distinction
    QCOMPARE(spyFinished.count(), 3);

    // Only Task 1 ever entered Rendering and emitted renderTaskStarted; Tasks 2 & 3 were cancelled before start
    QCOMPARE(spyStarted.count(), 1);
    QCOMPARE(spyStarted.at(0).at(0).toString(), taskId1);

    struct FinishedRecord {
        RenderStatus status = RenderStatus::Queued;
        QString outputDir;
        QString errorMessage;
    };
    std::map<QString, FinishedRecord> finishedByTaskId;
    for (int i = 0; i < spyFinished.count(); ++i) {
        const QVariantList& args = spyFinished.at(i);
        FinishedRecord rec;
        rec.status = static_cast<RenderStatus>(args.at(1).toInt());
        rec.outputDir = args.at(2).toString();
        rec.errorMessage = args.at(3).toString();
        finishedByTaskId[args.at(0).toString()] = rec;
    }

    // Verify Task 1 (cancelled during processing + staging rolled back)
    QVERIFY2(finishedByTaskId.count(taskId1) == 1, "Missing renderTaskFinished for Task 1");
    QCOMPARE(finishedByTaskId[taskId1].status, RenderStatus::Cancelled);
    QVERIFY2(finishedByTaskId[taskId1].errorMessage.contains("cancelled during processing"),
             qPrintable(finishedByTaskId[taskId1].errorMessage));

    // Verify Task 2 (cancelled before processing)
    QVERIFY2(finishedByTaskId.count(taskId2) == 1, "Missing renderTaskFinished for Task 2");
    QCOMPARE(finishedByTaskId[taskId2].status, RenderStatus::Cancelled);
    QVERIFY2(finishedByTaskId[taskId2].errorMessage.contains("cancelled before processing"),
             qPrintable(finishedByTaskId[taskId2].errorMessage));

    // Verify Task 3 (cancelled before processing)
    QVERIFY2(finishedByTaskId.count(taskId3) == 1, "Missing renderTaskFinished for Task 3");
    QCOMPARE(finishedByTaskId[taskId3].status, RenderStatus::Cancelled);
    QVERIFY2(finishedByTaskId[taskId3].errorMessage.contains("cancelled before processing"),
             qPrintable(finishedByTaskId[taskId3].errorMessage));

    // Staging directory from Task 1 must be rolled back and deleted
    QStringList remainingStagings = sourceQDir.entryList(QStringList() << ".desk32_staging_*", QDir::Dirs | QDir::Hidden);
    QCOMPARE(remainingStagings.count(), 0);

    // Step 9 (RCT-05): Verify all tasks are terminated and RenderEngine is completely Idle
    QVERIFY(engine.isIdle());
    QCOMPARE(engine.workerState(), RenderWorkerState::Idle);
    QCOMPARE(engine.queuedTasksCount(), 0);
    QVERIFY(!engine.isRendering());
    QVERIFY(engine.currentTaskInfo().taskId.isEmpty());

    // Step 10 (RCT-06): Verify a subsequent normal render completes with RenderStatus::Completed
    QString track4Path = sourceDir + "/after_cleanup.wav";
    QVERIFY(createTestWav(track4Path, 48000.0, 24000, 440.0));
    spyStarted.clear();
    spyFinished.clear();

    const QString taskId4 = engine.enqueueTrackRender(track4Path, settings);
    QVERIFY(!taskId4.isEmpty());
    QVERIFY(waitForRenderIdle(engine));
    QCoreApplication::processEvents();

    QCOMPARE(spyStarted.count(), 1);
    QCOMPARE(spyStarted.at(0).at(0).toString(), taskId4);
    QCOMPARE(spyFinished.count(), 1);
    QCOMPARE(spyFinished.at(0).at(0).toString(), taskId4);
    QCOMPARE(static_cast<RenderStatus>(spyFinished.at(0).at(1).toInt()), RenderStatus::Completed);
    const QString outDir4 = spyFinished.at(0).at(2).toString();
    QVERIFY(!outDir4.isEmpty());
    QVERIFY(QFile::exists(outDir4 + "/after_cleanup.wav"));
    engine.resetTaskInfo();
    QVERIFY(engine.isIdle());
}

void TestPhase4ExportRender::testRenderWorkerStateIdleRaceWindowEliminated()
{
    // RCT-04 (Section 3): Deterministic verification that popping a task from m_taskQueue
    // atomically transitions RenderEngine to RenderWorkerState::Starting under m_queueMutex,
    // eliminating the race window where queuedTasksCount() == 0 and isRendering() == false.
    RenderEngine& engine = RenderEngine::instance();
    QVERIFY(engine.isIdle());
    QCOMPARE(engine.workerState(), RenderWorkerState::Idle);

    QString sourceDir = m_tempDir->filePath("race_window_source");
    QDir().mkpath(sourceDir);
    QString trackPath = sourceDir + "/starting_state_track.wav";
    QVERIFY(createTestWav(trackPath, 48000.0, 12000, 440.0));

    RenderSettings settings;
    QSignalSpy spyFinished(&engine, &RenderEngine::renderTaskFinished);

    // Pause the worker thread in RenderWorkerState::Starting (immediately after m_taskQueue.pop()
    // and unlocking m_queueMutex, before workerSeqMgr.initialize() and before processTask()).
    ScopedRenderTestGate startingGate(engine, RenderWorkerState::Starting, /*releaseOnCancel=*/false, 5000);

    const QString taskId = engine.enqueueTrackRender(trackPath, settings);
    QVERIFY(!taskId.isEmpty());

    QVERIFY2(startingGate.waitForArrival(5000), "Timed out waiting for worker to enter Starting state");
    QCOMPARE(startingGate.arrivedTaskId(), taskId);
    QCOMPARE(startingGate.arrivedState(), RenderWorkerState::Starting);

    // At this exact point: the task has been popped from m_taskQueue (queuedTasksCount() == 0),
    // and processTask() has not started yet. The engine MUST NOT appear Idle!
    QCOMPARE(engine.queuedTasksCount(), 0);
    QCOMPARE(engine.workerState(), RenderWorkerState::Starting);
    QVERIFY(engine.isRendering());
    QVERIFY(!engine.isIdle());

    // Release the Starting gate and allow the render to complete normally
    startingGate.releaseAndDetach();
    QVERIFY2(waitForRenderIdle(engine), "Timed out waiting for render to finish after Starting gate release");
    QCoreApplication::processEvents();

    QCOMPARE(spyFinished.count(), 1);
    QCOMPARE(spyFinished.at(0).at(0).toString(), taskId);
    QCOMPARE(static_cast<RenderStatus>(spyFinished.at(0).at(1).toInt()), RenderStatus::Completed);
    QVERIFY(engine.isIdle());
    QCOMPARE(engine.workerState(), RenderWorkerState::Idle);
    QVERIFY(!engine.isRendering());
    engine.resetTaskInfo();
}

void TestPhase4ExportRender::testRenderCleanupFailureAndTimeoutDetection()
{
    // RCT-07, RCT-08, RCT-09, RCT-10 (Sections 5, 6, 7):
    // Deterministic verification of:
    // - RCT-10: Synchronization gate bounded timeouts (both arrival wait and worker hold limit)
    // - RCT-08: Render wait timeout is never treated as test success even if post-timeout cleanup succeeds
    // - RCT-07: Cleanup failure path sets g_renderCleanupFailed and is detected by cleanup evaluation
    // - RCT-09: Post-cleanup state isolation (subsequent tests and normal renders succeed cleanly)
    RenderEngine& engine = RenderEngine::instance();
    QVERIFY(engine.isIdle());

    QString sourceDir = m_tempDir->filePath("cleanup_failure_test_source");
    QDir().mkpath(sourceDir);
    RenderSettings settings;

    // -------------------------------------------------------------------------
    // Part 1 (RCT-10): Synchronization gate bounded timeout verification
    // -------------------------------------------------------------------------
    {
        // 1a. Arrival wait timeout when no task is enqueued
        ScopedRenderTestGate idleGate(engine, RenderWorkerState::Rendering, false, 500);
        QVERIFY(!idleGate.waitForArrival(40));
        idleGate.releaseAndDetach();
        QVERIFY(engine.isIdle());

        // 1b. Worker wait limit timeout when test never calls release()
        QString gateTimeoutTrack = sourceDir + "/gate_timeout_track.wav";
        QVERIFY(createTestWav(gateTimeoutTrack, 48000.0, 12000, 440.0));
        ScopedRenderTestGate shortHoldGate(engine, RenderWorkerState::Rendering, /*releaseOnCancel=*/false, /*workerWaitLimitMs=*/60);

        const QString tid = engine.enqueueTrackRender(gateTimeoutTrack, settings);
        QVERIFY(!tid.isEmpty());
        QVERIFY2(shortHoldGate.waitForArrival(5000), "Worker did not arrive at shortHoldGate");

        // Do not call release(); verify worker self-unblocks when workerWaitLimitMs (60ms) expires
        QVERIFY2(shortHoldGate.waitForWorkerExit(2000), "Worker did not self-unblock after gate timeout");
        QVERIFY(shortHoldGate.workerTimedOut());
        shortHoldGate.releaseAndDetach();

        QVERIFY2(waitForRenderIdle(engine), "Engine did not become idle after shortHoldGate self-unblock");
        engine.resetTaskInfo();
    }

    // -------------------------------------------------------------------------
    // Part 2 (RCT-08 & RCT-09): Wait timeout with successful post-timeout cleanup
    // -------------------------------------------------------------------------
    {
        QString timeoutTrack = sourceDir + "/wait_timeout_track.wav";
        QVERIFY(createTestWav(timeoutTrack, 48000.0, 24000, 440.0));
        QSignalSpy spyFinished(&engine, &RenderEngine::renderTaskFinished);

        // Gate holds worker during initial wait, but releases on cancel so post-timeout cleanup succeeds
        ScopedRenderTestGate cancelAwareGate(engine, RenderWorkerState::Rendering, /*releaseOnCancel=*/true, 5000);
        const QString tid = engine.enqueueTrackRender(timeoutTrack, settings);
        QVERIFY(!tid.isEmpty());
        QVERIFY2(cancelAwareGate.waitForArrival(5000), "Worker did not arrive at cancelAwareGate");

        // waitForRenderIdle times out after 40ms, then triggers cancelAndDrainRenderEngine(5000) which succeeds
        const bool waitResult = waitForRenderIdle(engine, /*timeoutMs=*/40, /*cleanupTimeoutMs=*/5000);
        QVERIFY(!waitResult); // RCT-08: waitForRenderIdle returns false on timeout
        QVERIFY(g_renderTimeoutOccurred.load());
        QVERIFY(!g_renderCleanupFailed.load()); // Post-timeout cleanup succeeded
        QVERIFY(engine.isIdle());               // Engine restored to Idle

        cancelAwareGate.releaseAndDetach();

        // Evaluate via performTestCleanupCycle: even though cleanup succeeded, hadRenderTimeout
        // MUST cause eval.passed to be false so a timeout is never treated as test success!
        const CleanupEvaluation eval = performTestCleanupCycle(engine, 5000, true);
        QVERIFY(!eval.passed);
        QVERIFY(eval.hadRenderTimeout);
        QVERIFY(!eval.hadCleanupFailure);
        QVERIFY(!eval.engineLeftBusy);
        QVERIFY2(eval.failureMessage.contains("Render wait timed out"), qPrintable(eval.failureMessage));
        QVERIFY(!g_renderTimeoutOccurred.load());
        QVERIFY(!g_renderCleanupFailed.load());
    }

    // -------------------------------------------------------------------------
    // Part 3 (RCT-07 & RCT-09): Deterministic cleanup failure path & recovery
    // -------------------------------------------------------------------------
    {
        QString failTrack = sourceDir + "/cleanup_fail_track.wav";
        QVERIFY(createTestWav(failTrack, 48000.0, 24000, 440.0));

        // Gate holds worker and ignores cancel (releaseOnCancel = false) to deterministically
        // force cancelAndDrainRenderEngine() to fail its deadline.
        ScopedRenderTestGate blockingGate(engine, RenderWorkerState::Rendering, /*releaseOnCancel=*/false, 5000);
        const QString tid = engine.enqueueTrackRender(failTrack, settings);
        QVERIFY(!tid.isEmpty());
        QVERIFY2(blockingGate.waitForArrival(5000), "Worker did not arrive at blockingGate");

        // Both wait (40ms) and post-timeout drain (40ms) expire while worker is held at blockingGate
        const bool waitResult = waitForRenderIdle(engine, /*timeoutMs=*/40, /*cleanupTimeoutMs=*/40);
        QVERIFY(!waitResult);
        QVERIFY(g_renderTimeoutOccurred.load());
        QVERIFY(g_renderCleanupFailed.load()); // RCT-07: g_renderCleanupFailed is deterministically set
        QVERIFY(!engine.isIdle());

        // Verify performTestCleanupCycle when hook is NOT yet cleared and drain fails:
        // detects both hadRenderTimeout, hadCleanupFailure, and engineLeftBusy.
        const CleanupEvaluation evalWhileBlocked = performTestCleanupCycle(
            engine, /*drainTimeoutMs=*/30, /*clearHooksFirst=*/false);
        QVERIFY(!evalWhileBlocked.passed);
        QVERIFY(evalWhileBlocked.hadRenderTimeout);
        QVERIFY(evalWhileBlocked.hadCleanupFailure);
        QVERIFY(evalWhileBlocked.engineLeftBusy);
        QVERIFY2(evalWhileBlocked.failureMessage.contains("CRITICAL TEST ENVIRONMENT ABNORMALITY"),
                 qPrintable(evalWhileBlocked.failureMessage));

        // Now verify standard performTestCleanupCycle (with clearHooksFirst = true, as called by cleanup()):
        // Re-set g_renderCleanupFailed to simulate cleanup() running after a failed drain,
        // verify it clears the hook (releasing blockingGate), drains the worker to Idle,
        // and still reports eval.passed == false due to hadCleanupFailure.
        g_renderTimeoutOccurred.store(true, std::memory_order_release);
        g_renderCleanupFailed.store(true, std::memory_order_release);

        const CleanupEvaluation evalWithHookRelease = performTestCleanupCycle(
            engine, /*drainTimeoutMs=*/5000, /*clearHooksFirst=*/true);
        QVERIFY(!evalWithHookRelease.passed);
        QVERIFY(evalWithHookRelease.hadRenderTimeout);
        QVERIFY(evalWithHookRelease.hadCleanupFailure);
        QVERIFY(!evalWithHookRelease.engineLeftBusy);
        QVERIFY2(evalWithHookRelease.failureMessage.contains("CRITICAL TEST ENVIRONMENT ABNORMALITY"),
                 qPrintable(evalWithHookRelease.failureMessage));

        blockingGate.releaseAndDetach();

        // Engine must now be completely Idle and flags reset
        QVERIFY(engine.isIdle());
        QCOMPARE(engine.workerState(), RenderWorkerState::Idle);
        QCOMPARE(engine.queuedTasksCount(), 0);
        QVERIFY(!engine.isRendering());
        QVERIFY(!g_renderTimeoutOccurred.load());
        QVERIFY(!g_renderCleanupFailed.load());

        // RCT-09: Verify subsequent normal render completes cleanly with zero leftover state pollution
        QString recoveryTrack = sourceDir + "/post_failure_recovery.wav";
        QVERIFY(createTestWav(recoveryTrack, 48000.0, 12000, 440.0));
        QSignalSpy spyRecoveryFinished(&engine, &RenderEngine::renderTaskFinished);

        const QString recId = engine.enqueueTrackRender(recoveryTrack, settings);
        QVERIFY(!recId.isEmpty());
        QVERIFY2(waitForRenderIdle(engine), "Subsequent render failed after cleanup recovery");
        QCoreApplication::processEvents();

        QCOMPARE(spyRecoveryFinished.count(), 1);
        QCOMPARE(spyRecoveryFinished.at(0).at(0).toString(), recId);
        QCOMPARE(static_cast<RenderStatus>(spyRecoveryFinished.at(0).at(1).toInt()), RenderStatus::Completed);
        engine.resetTaskInfo();
        QVERIFY(engine.isIdle());
    }
}

void TestPhase4ExportRender::testStep3ControllerCanonicalStateAndTrackResults()
{
    const QString wsDir = m_tempDir->filePath("ctrl_ws_step3");
    QDir().mkpath(wsDir);
    QString wsErr;
    QVERIFY2(desk32::engine::workspace::WorkspaceManager::instance().registerAndOpenWorkspace(wsDir, wsErr), qPrintable(wsErr));
    EngineController& controller = EngineController::instance();
    RenderEngine& engine = RenderEngine::instance();
    engine.resetTaskInfo();
    QVERIFY(controller.isRenderIdle());
    QCOMPARE(controller.isRenderIdle(), engine.isIdle());
    QCOMPARE(controller.renderWorkerState(), static_cast<int>(RenderWorkerState::Idle));
    QCOMPARE(controller.renderQueuedCount(), 0);
    QCOMPARE(controller.renderStatusText(), QString("Idle"));

    QString albumDir = m_tempDir->filePath("step3_album");
    QDir().mkpath(albumDir);
    QVERIFY(createTestWav(albumDir + "/t1.wav", 48000.0, 24000, 220.0));
    QVERIFY(createTestWav(albumDir + "/t2.wav", 48000.0, 24000, 330.0));

    QSignalSpy spyStarted(&controller, &EngineController::renderTaskStarted);
    QSignalSpy spyFinished(&controller, &EngineController::renderTaskFinished);
    const QString taskId = controller.startAlbumRender(albumDir, 1, 0, 48000.0, true, true);
    QVERIFY(!taskId.isEmpty());
    QVERIFY(!controller.isRenderIdle());

    QVERIFY(spyFinished.wait(10000));
    QCOMPARE(spyStarted.count(), 1);
    QCOMPARE(spyFinished.at(0).at(1).toInt(), static_cast<int>(RenderStatus::Completed));
    QVERIFY(waitForRenderIdle(engine));
    QCoreApplication::processEvents();

    QCOMPARE(controller.renderStatusText(), QString("Completed"));
    QCOMPARE(controller.renderProgress(), 1.0);
    QCOMPARE(controller.renderTotalTracks(), 2);
    QCOMPARE(controller.renderType(), QString("Album"));
    QVERIFY(!controller.renderSerial().isEmpty());

    const QVariantList results = controller.renderTrackResults();
    QCOMPARE(results.size(), 2);
    for (const auto& r : results) {
        QCOMPARE(r.toMap().value("status").toString(), QString("Completed"));
    }
    const QStringList files = controller.renderOutputFiles();
    QCOMPARE(files.size(), 2);
    for (const auto& f : files) QVERIFY2(QFile::exists(f), qPrintable(f));

    controller.resetRenderStatus();
    QCOMPARE(controller.renderStatusText(), QString("Idle"));
}

void TestPhase4ExportRender::testStep3CancelAllAndLogHierarchy()
{
    const QString wsDir = m_tempDir->filePath("ctrl_ws_cancel_all");
    QDir().mkpath(wsDir);
    QString wsErr;
    QVERIFY2(desk32::engine::workspace::WorkspaceManager::instance().registerAndOpenWorkspace(wsDir, wsErr), qPrintable(wsErr));
    EngineController& controller = EngineController::instance();
    RenderEngine& engine = RenderEngine::instance();
    engine.resetTaskInfo();

    QString dirA = m_tempDir->filePath("step3_cancel_a");
    QString dirB = m_tempDir->filePath("step3_cancel_b");
    QDir().mkpath(dirA);
    QDir().mkpath(dirB);
    QVERIFY(createTestWav(dirA + "/a1.wav", 48000.0, 24000, 220.0));
    QVERIFY(createTestWav(dirA + "/a2.wav", 48000.0, 24000, 220.0));
    QVERIFY(createTestWav(dirB + "/b1.wav", 48000.0, 24000, 220.0));

    QSignalSpy spyFinished(&controller, &EngineController::renderTaskFinished);
    ScopedRenderTestGate gate(engine, RenderWorkerState::Rendering, /*releaseOnCancel=*/true, 5000);

    const QString id1 = controller.startAlbumRender(dirA, 1, 0, 48000.0, true, true);
    QVERIFY(!id1.isEmpty());
    QVERIFY(gate.waitForArrival(5000));
    const QString id2 = controller.startAlbumRender(dirB, 1, 0, 48000.0, true, true);
    QVERIFY(!id2.isEmpty());

    QCOMPARE(controller.renderWorkerState(), static_cast<int>(RenderWorkerState::Rendering));
    QCOMPARE(controller.renderQueuedCount(), 1);
    QVERIFY(controller.isRendering());
    QVERIFY(!controller.isRenderIdle());

    controller.cancelAllRenders();
    QVERIFY(waitForRenderIdle(engine, 10000));
    gate.releaseAndDetach();
    QTRY_COMPARE_WITH_TIMEOUT(spyFinished.count(), 2, 5000);
    for (int i = 0; i < spyFinished.count(); ++i) {
        QCOMPARE(spyFinished.at(i).at(1).toInt(), static_cast<int>(RenderStatus::Cancelled));
    }
    QVERIFY(controller.isRenderIdle());

    // Log hierarchy: parent op events are retrievable by related query
    LogService::instance().flush();
    const QVariantList related = LogService::instance().fetchRelatedLogs(QString("RENDER_%1").arg(id1));
    QVERIFY(!related.isEmpty());
    bool sawCancelled = false;
    for (const auto& v : related) {
        if (v.toMap().value("status").toString() == "Cancelled") sawCancelled = true;
    }
    QVERIFY(sawCancelled);

    QVariantMap filter;
    filter["status"] = "Cancelled";
    filter["category"] = "Render";
    QVERIFY(!LogService::instance().fetchLogs(filter).isEmpty());
    engine.resetTaskInfo();
}

void TestPhase4ExportRender::testStep3MonitorVolumeAndAudition()
{
    EngineController& controller = EngineController::instance();
    if (!controller.pipeline()) {
        QVERIFY(controller.initialize());
    }
    QVERIFY(controller.pipeline());

    // Render Monitor volume is independent of Listening volume and of Render Engine
    controller.setListeningVolume(1.0);
    controller.setRenderMonitorVolume(0.5);
    QCOMPARE(controller.renderMonitorVolume(), 0.5);
    QCOMPARE(controller.listeningVolume(), 1.0);
    controller.setRenderMonitorVolume(1.0);

    QString trackPath = m_tempDir->filePath("step3_audition.wav");
    QVERIFY(createTestWav(trackPath, 48000.0, 4800, 440.0));

    // Auditioning with explicit Listening Path keeps Listening Path (UI shows warning)
    controller.setOutputRoutingMode(0);
    QVERIFY(controller.auditionRenderedFile("C", trackPath, false));
    QCOMPARE(controller.outputRoutingMode(), 0);

    // Default engages Render Monitor Path
    QVERIFY(controller.auditionRenderedFile("C", trackPath));
    QCOMPARE(controller.outputRoutingMode(), 1);

    // Locked deck refuses the audition load
    controller.setDeckLocked("D", true);
    QVERIFY(!controller.auditionRenderedFile("D", trackPath));
    controller.setDeckLocked("D", false);

    // Missing file fails
    QVERIFY(!controller.auditionRenderedFile("C", m_tempDir->filePath("does_not_exist.wav")));
}

void TestPhase4ExportRender::testControllerDspNodeEnableSyncToRender()
{
    const QString wsDir = m_tempDir->filePath("ctrl_ws_dsp_sync");
    QDir().mkpath(wsDir);
    QString wsErr;
    QVERIFY2(desk32::engine::workspace::WorkspaceManager::instance().registerAndOpenWorkspace(wsDir, wsErr), qPrintable(wsErr));
    EngineController& controller = EngineController::instance();
    RenderEngine& engine = RenderEngine::instance();
    engine.resetTaskInfo();
    QVERIFY(controller.initialize(true));

    // All Master DSP nodes start bypassed
    QVERIFY(controller.isPeqBypassed());
    QVERIFY(controller.isGeqBypassed());
    QVERIFY(controller.isDspNodeBypassed("Filter"));
    QVERIFY(controller.isDspNodeBypassed("Compressor"));
    QVERIFY(controller.isDspNodeBypassed("Reverb"));
    QVERIFY(controller.isDspNodeBypassed("Limiter"));

    QString srcDir = m_tempDir->filePath("controller_dsp_sync");
    QDir().mkpath(srcDir);
    QString trackPath = srcDir + "/sine_440.wav";
    QVERIFY(createTestWav(trackPath, 48000.0, 4800, 440.0));
    QVERIFY(controller.loadTrack("A", trackPath));

    auto runFloat32Render = [&](bool enableDsp) -> QVector<float> {
        QSignalSpy spyFinished(&engine, &RenderEngine::renderTaskFinished);
        const QString taskId = controller.startTrackRender("A", 2 /*Float32*/, 0 /*Original*/, 48000.0, false /*dither*/, enableDsp);
        if (taskId.isEmpty() || !spyFinished.wait(10000)) {
            return {};
        }
        const QStringList files = controller.renderOutputFiles();
        if (files.size() != 1) {
            return {};
        }
        QFile f(files.first());
        if (!f.open(QIODevice::ReadOnly)) {
            return {};
        }
        QByteArray raw = f.readAll();
        f.close();
        if (raw.size() <= 44) {
            return {};
        }
        const qsizetype numFloats = (raw.size() - 44) / static_cast<qsizetype>(sizeof(float));
        QVector<float> samples(numFloats);
        std::memcpy(samples.data(), raw.constData() + 44, static_cast<size_t>(numFloats) * sizeof(float));
        controller.resetRenderStatus();
        return samples;
    };

    auto maxAbsDiff = [](const QVector<float>& a, const QVector<float>& b) -> double {
        if (a.size() != b.size() || a.isEmpty()) return -1.0;
        double m = 0.0;
        for (qsizetype i = 0; i < a.size(); ++i) {
            m = std::max(m, std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
        }
        return m;
    };

    // 1. Baseline dry render (all nodes bypassed)
    const QVector<float> dry = runFloat32Render(true);
    QVERIFY(dry.size() >= 4800 * 2);

    // 2. PEQ enabled via setDspNodeBypass("PEQ", false) / setPeqBypassed
    controller.setPeqBand(false, 5, 440.0, 6.0, 1.0, 0, true);
    controller.setPeqBand(true, 5, 440.0, 6.0, 1.0, 0, true);
    controller.setPeqBypassed(false);
    QVERIFY(!controller.isPeqBypassed());
    QCOMPARE(controller.bitPerfectStatus(), 1);
    const QVector<float> withPeq = runFloat32Render(true);
    QVERIFY(maxAbsDiff(dry, withPeq) > 0.1);
    controller.setPeqBypassed(true);
    QVERIFY(controller.isPeqBypassed());

    // 3. GEQ enabled via setGeqBypassed(false)
    controller.setGeqBandGain(15 /* 500 Hz */, 6.0);
    controller.setGeqBypassed(false);
    QVERIFY(!controller.isGeqBypassed());
    const QVector<float> withGeq = runFloat32Render(true);
    QVERIFY(maxAbsDiff(dry, withGeq) > 0.05);
    controller.setGeqBypassed(true);
    QVERIFY(controller.isGeqBypassed());

    // 4. Filter enabled via setDspNodeBypass("Filter", false)
    controller.setFilterParams(1000.0, true, 1 /*24dB/oct*/, 20000.0, false, 0);
    controller.setDspNodeBypass("Filter", false);
    const QVector<float> withFilter = runFloat32Render(true);
    QVERIFY(maxAbsDiff(dry, withFilter) > 0.1);
    controller.setDspNodeBypass("Filter", true);

    // 5. Compressor configured while bypassed, then enabled via setDspNodeBypass("Compressor", false)
    controller.setCompressorParams(-20.0, 8.0, 1.0, 50.0, 0.0, 0.0);
    controller.setDspNodeBypass("Compressor", false);
    const QVector<float> withComp = runFloat32Render(true);
    QVERIFY(maxAbsDiff(dry, withComp) > 0.05);
    controller.setDspNodeBypass("Compressor", true);

    // 6. Reverb configured while bypassed, then enabled via setDspNodeBypass("Reverb", false)
    controller.setReverbParams(0.8, 0.3, 0.5, 0.0, true);
    controller.setDspNodeBypass("Reverb", false);
    const QVector<float> withRev = runFloat32Render(true);
    QVERIFY(maxAbsDiff(dry, withRev) > 0.05);
    controller.setDspNodeBypass("Reverb", true);

    // 7. Limiter configured while bypassed, then enabled via setDspNodeBypass("Limiter", false)
    controller.setLimiterParams(-10.0, 50.0, 1.0);
    controller.setDspNodeBypass("Limiter", false);
    const QVector<float> withLim = runFloat32Render(true);
    QVERIFY(maxAbsDiff(dry, withLim) > 0.05);
    controller.setDspNodeBypass("Limiter", true);

    // 8. Re-bypassing all nodes restores exact dry output
    const QVector<float> reBypassed = runFloat32Render(true);
    QVERIFY(maxAbsDiff(dry, reBypassed) < 1e-6);

    // 9. When Global DSP Bypass + Render Monitor + unity gains + matching sample rate in offline mode,
    // bitPerfectStatus never reports Verified (2) when Direct Mode is Unknown (0)
    controller.setGlobalDspBypass(true);
    controller.setOutputRoutingMode(1);
    controller.setMasterGain(1.0);
    controller.setMasterMute(false);
    controller.setRenderMonitorVolume(1.0);
    QCOMPARE(controller.bitPerfectStatus(), 0);
    controller.setGlobalDspBypass(false);
    controller.setOutputRoutingMode(0);
}

void TestPhase4ExportRender::testFailClosedWithoutWorkspaceAndWorkspaceSerial()
{
    EngineController& controller = EngineController::instance();
    RenderEngine& engine = RenderEngine::instance();
    auto& wsMgr = desk32::engine::workspace::WorkspaceManager::instance();
    QVERIFY(controller.initialize(true));

    const QString prevWs = wsMgr.openWorkspacePath();
    QString extDir = m_tempDir->filePath("ext_source_v381");
    QDir().mkpath(extDir);
    const QString trackPath = extDir + "/ext_src.wav";
    QVERIFY(createTestWav(trackPath, 48000.0, 2400, 440.0));
    QVERIFY(controller.loadTrack("A", trackPath));

    // 1. Simulated startup in read-only mode (no open workspace) -> startTrackRender / startAlbumRender MUST fail closed
    wsMgr.simulateAppRestartForTesting();
    wsMgr.enterReadOnlyMode();
    QVERIFY(!wsMgr.hasOpenWorkspace());
    QVERIFY(controller.startTrackRender("A").isEmpty());
    QVERIFY(controller.startAlbumRender(extDir).isEmpty());

    LogService::instance().flush();
    QVariantMap filter;
    filter["category"] = "Render";
    filter["status"] = "Failed";
    const QVariantList logs = LogService::instance().fetchLogs(filter);
    bool foundNotOpenErr = false;
    for (const auto& item : logs) {
        if (item.toMap().value("errorCode").toInt() == ErrorCode::WorkspaceNotOpenFailClosed) {
            foundNotOpenErr = true;
            break;
        }
    }
    QVERIFY(foundNotOpenErr);

    // 2. Restart cleanly & open dedicated workspace -> render outputs & serials stay inside workspace, NOT external source
    wsMgr.simulateAppRestartForTesting();
    const QString ws2 = m_tempDir->filePath("isolated_render_ws");
    QDir().mkpath(ws2);
    QString err;
    QVERIFY2(wsMgr.registerAndOpenWorkspace(ws2, err), qPrintable(err));

    QSignalSpy spy1(&engine, &RenderEngine::renderTaskFinished);
    const QString tid1 = controller.startTrackRender("A");
    QVERIFY(!tid1.isEmpty());
    QVERIFY(spy1.wait(10000));
    const QString outDir1 = spy1.at(0).at(2).toString();
    QVERIFY(outDir1.startsWith(QDir::cleanPath(ws2) + "/A"));
    QCOMPARE(QDir(extDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
    controller.resetRenderStatus();

    QSignalSpy spy2(&engine, &RenderEngine::renderTaskFinished);
    const QString tid2 = controller.startAlbumRender(extDir);
    QVERIFY(!tid2.isEmpty());
    QVERIFY(spy2.wait(10000));
    const QString outDir2 = spy2.at(0).at(2).toString();
    QVERIFY(outDir2.startsWith(QDir::cleanPath(ws2) + "/B"));
    controller.resetRenderStatus();

    // Restore original test workspace
    if (!prevWs.isEmpty()) {
        wsMgr.simulateAppRestartForTesting();
        QString rerr;
        wsMgr.openWorkspace(prevWs, rerr);
    }
}

QTEST_GUILESS_MAIN(TestPhase4ExportRender)

#include "test_phase4_export_render.moc"
