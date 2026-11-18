#include <QTest>
#include <QObject>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <cmath>
#include <vector>
#include <array>
#include <cstring>
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "engine/workspace/WorkspaceManager.hpp"
#include "engine/metadata/TrackMetadata.hpp"
#include "engine/decoder/FFmpegAudioDecoder.hpp"
#include "engine/render/WavFileWriter.hpp"
#include "engine/render/RenderEngine.hpp"
#include "engine/render/RenderSequenceManager.hpp"
#include "engine/dsp/ParametricEQ.hpp"
#include "engine/dsp/HighLowPassFilter.hpp"
#include "core/EngineController.hpp"
#include "core/log/LogService.hpp"
#include "core/log/LogErrorCodes.hpp"

using namespace desk32::engine::workspace;
using namespace desk32::engine::metadata;
using namespace desk32::engine::render;
using namespace desk32::engine::decoder;
using namespace desk32::core;
using namespace desk32::core::log;

namespace {

bool createTestWav(const QString& path, double sampleRate, size_t numFrames, double freq = 440.0, double amp = 0.5)
{
    WavFileWriter writer;
    if (!writer.open(path, sampleRate, WavBitDepth::Int24)) {
        return false;
    }
    std::vector<double> left(numFrames);
    std::vector<double> right(numFrames);
    for (size_t i = 0; i < numFrames; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        left[i] = amp * std::sin(2.0 * M_PI * freq * t);
        right[i] = amp * std::cos(2.0 * M_PI * freq * t);
    }
    const bool ok = writer.writeBlock(left.data(), right.data(), numFrames, false);
    writer.close();
    return ok;
}

bool createTestFlacViaFfmpeg(const QString& wavSrc, const QString& flacDst)
{
    QProcess proc;
    proc.start(QStringLiteral("ffmpeg"),
               {QStringLiteral("-y"), QStringLiteral("-v"), QStringLiteral("quiet"),
                QStringLiteral("-i"), wavSrc, QStringLiteral("-c:a"), QStringLiteral("flac"), flacDst});
    return proc.waitForFinished(10000) && proc.exitCode() == 0 && QFile::exists(flacDst);
}

QByteArray computeSha256(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return h.result().toHex();
}

} // namespace

class TestWorkspaceV381 : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();

    void testWorkspaceRegistrationAndPathConflict();
    void testStartupSingleWorkspaceAndReadOnlyMode();
    void testCrossProcessFlockAndLockedAccessBlock();
    void testImportTagLibProvenanceCollisionSymlinkHardlink();
    void testTagEditingCowCheckpointsDiffAuditionRollbackGc();
    void testRollbackPartialRecoveryFaultLogging();
    void testExternalModificationTwoStageDetection();
    void testDirectModeSampleRateFollowAndBypass();
    void testDeckLevelMatchPostTapAndOffsetFrames();
    void testPeq16BandsHpfLpfQMasterDspPresetAndPlaybackHistory();

private:
    std::unique_ptr<QTemporaryDir> m_tempDir;
    QString m_dbPath;
    QString m_configPath;
};

void TestWorkspaceV381::initTestCase()
{
    m_tempDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tempDir->isValid());
    m_dbPath = m_tempDir->filePath("test_v381_log.sqlite");
    m_configPath = m_tempDir->filePath("test_v381_workspaces.json");

    LogService::instance().start(m_dbPath);
    RenderSequenceManager::instance().initialize(m_dbPath);
    RenderEngine::instance().setDatabasePath(m_dbPath);

    auto& ws = WorkspaceManager::instance();
    ws.setConfigFilePathForTesting(m_configPath);
    ws.resetForTesting();
    EngineController::instance().presetManager().setCustomRootPath(m_tempDir->filePath("presets"));
}

void TestWorkspaceV381::init()
{
    WorkspaceManager::instance().setTestFaultCallback(nullptr);
    WorkspaceManager::instance().resetForTesting();
}

void TestWorkspaceV381::cleanup()
{
    WorkspaceManager::instance().setTestFaultCallback(nullptr);
    WorkspaceManager::instance().closeWorkspaceForShutdown();
    RenderEngine::instance().setWorkspacePath(QString());
}

void TestWorkspaceV381::cleanupTestCase()
{
    RenderEngine::instance().stop();
    WorkspaceManager::instance().resetForTesting();
    RenderSequenceManager::instance().close();
    LogService::instance().stop();
    m_tempDir.reset();
}

void TestWorkspaceV381::testWorkspaceRegistrationAndPathConflict()
{
    auto& ws = WorkspaceManager::instance();
    const QString parentDir = m_tempDir->filePath("ws_reg_parent");
    const QString childDir = parentDir + "/nested_child";
    const QString siblingDir = m_tempDir->filePath("ws_reg_sibling");
    const QString filePath = m_tempDir->filePath("not_a_dir.txt");
    QDir().mkpath(childDir);
    QDir().mkpath(siblingDir);
    {
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }

    QString err;
    // Non-directory / non-existent rejected
    QVERIFY(!ws.registerWorkspace(filePath, err));
    QVERIFY(!ws.registerWorkspace(m_tempDir->filePath("nonexistent_ws"), err));
    QVERIFY(!ws.registerWorkspace(QStringLiteral("relative/ws"), err));

    // Register parent -> succeeds
    QVERIFY2(ws.registerWorkspace(parentDir, err), qPrintable(err));
    // Re-register same path -> succeeds (idempotent) or clear, but let's verify child inside parent rejected
    QVERIFY(!ws.registerWorkspace(childDir, err));

    // Distinct sibling -> succeeds
    QVERIFY2(ws.registerWorkspace(siblingDir, err), qPrintable(err));

    // Now test reverse: register child first, then enclosing parent must be rejected
    ws.resetForTesting();
    const QString child2 = m_tempDir->filePath("ws_rev/sub/leaf");
    const QString parent2 = m_tempDir->filePath("ws_rev");
    QDir().mkpath(child2);
    QVERIFY2(ws.registerWorkspace(child2, err), qPrintable(err));
    QVERIFY(!ws.registerWorkspace(parent2, err));
}

void TestWorkspaceV381::testStartupSingleWorkspaceAndReadOnlyMode()
{
    auto& ws = WorkspaceManager::instance();
    const QString wsA = m_tempDir->filePath("single_ws_a");
    const QString wsB = m_tempDir->filePath("single_ws_b");
    QDir().mkpath(wsA);
    QDir().mkpath(wsB);

    QString err;
    QVERIFY(ws.registerWorkspace(wsA, err));
    QVERIFY(ws.registerWorkspace(wsB, err));

    // Before startup selection -> Read-Only, no workspace open
    QVERIFY(!ws.hasOpenWorkspace());
    QVERIFY(ws.isReadOnlyMode());

    // Open A -> A is Open, B is Locked
    QVERIFY2(ws.openWorkspace(wsA, err), qPrintable(err));
    QVERIFY(ws.hasOpenWorkspace());
    QVERIFY(!ws.isReadOnlyMode());
    QCOMPARE(QDir::cleanPath(ws.openWorkspacePath()), QDir::cleanPath(wsA));

    auto list = ws.listRegisteredWorkspaces();
    QCOMPARE(list.size(), size_t(2));
    for (const auto& info : list) {
        if (QDir::cleanPath(info.path) == QDir::cleanPath(wsA)) {
            QCOMPARE(info.lockState, WorkspaceLockState::Open);
        } else {
            QCOMPARE(info.lockState, WorkspaceLockState::Locked);
        }
    }

    // Mid-session switching without restart must be refused (Spec §2.5)
    QVERIFY(!ws.openWorkspace(wsB, err));

    // Simulate restart -> open B (now A is Locked, B is Open)
    ws.simulateAppRestartForTesting();
    QVERIFY2(ws.openWorkspace(wsB, err), qPrintable(err));
    QCOMPARE(QDir::cleanPath(ws.openWorkspacePath()), QDir::cleanPath(wsB));
    QVERIFY(!ws.isPathBlockedByLockedWorkspace(wsB + "/track.wav"));
    QVERIFY(ws.isPathBlockedByLockedWorkspace(wsA + "/track.wav"));

    // Simulate restart -> Read-Only fallback closes open workspace cleanly
    ws.simulateAppRestartForTesting();
    ws.enterReadOnlyMode();
    QVERIFY(!ws.hasOpenWorkspace());
    QVERIFY(ws.isReadOnlyMode());
    QVERIFY(ws.isPathBlockedByLockedWorkspace(wsA + "/track.wav"));
    QVERIFY(ws.isPathBlockedByLockedWorkspace(wsB + "/track.wav"));
}

void TestWorkspaceV381::testCrossProcessFlockAndLockedAccessBlock()
{
    auto& ws = WorkspaceManager::instance();
    EngineController& ctrl = EngineController::instance();
    QVERIFY(ctrl.initialize(true));

    const QString ws1 = m_tempDir->filePath("lock_ws_1");
    const QString ws2 = m_tempDir->filePath("lock_ws_2");
    QDir().mkpath(ws1 + "/.desk32");
    QDir().mkpath(ws2);
    const QString ws1Track = ws1 + "/locked_track.wav";
    QVERIFY(createTestWav(ws1Track, 48000.0, 2400));

    QString err;
    QVERIFY(ws.registerWorkspace(ws1, err));
    QVERIFY(ws.registerAndOpenWorkspace(ws2, err));

    // Locked workspace access block (§2.5): ws1 is Locked while ws2 is Open
    QVERIFY(!ctrl.loadTrack("A", ws1Track));
    QVERIFY(!ctrl.startFolderSession("A", ws1, {}));
    const auto impRes = ws.importFile(ws1Track);
    QCOMPARE(impRes.status, ImportResultStatus::Failed);

    // Cross-process flock(2) mutual exclusion via child process
    const QString lockFile = ws1 + "/.desk32/workspace.lock";
    int pipeFds[2]{-1, -1};
    int readyPipe[2]{-1, -1};
    QCOMPARE(::pipe(pipeFds), 0);
    QCOMPARE(::pipe(readyPipe), 0);

    pid_t pid = ::fork();
    QVERIFY(pid >= 0);
    if (pid == 0) {
        ::close(pipeFds[1]);
        ::close(readyPipe[0]);
        const int lfd = ::open(lockFile.toUtf8().constData(), O_CREAT | O_RDWR, 0644);
        const int flk = (lfd >= 0) ? ::flock(lfd, LOCK_EX | LOCK_NB) : -1;
        char b = (flk == 0) ? '1' : '0';
        (void)::write(readyPipe[1], &b, 1);
        ::close(readyPipe[1]);
        // Wait until parent closes pipeFds[0]
        char dummy = 0;
        (void)::read(pipeFds[0], &dummy, 1);
        if (lfd >= 0) {
            ::flock(lfd, LOCK_UN);
            ::close(lfd);
        }
        ::close(pipeFds[0]);
        ::_exit(0);
    }

    ::close(pipeFds[0]);
    ::close(readyPipe[1]);
    char childAck = 0;
    QCOMPARE(::read(readyPipe[0], &childAck, 1), ssize_t(1));
    ::close(readyPipe[0]);
    QCOMPARE(childAck, '1');

    // While child holds flock, parent MUST detect InUse and fail to open ws1
    QVERIFY(ws.isWorkspaceInUseByOtherProcess(ws1));
    QVERIFY(!ws.openWorkspace(ws1, err));

    // Release child process -> flock released -> after restart parent CAN open ws1
    ::close(pipeFds[1]);
    int status = 0;
    ::waitpid(pid, &status, 0);

    ws.simulateAppRestartForTesting();
    QVERIFY(!ws.isWorkspaceInUseByOtherProcess(ws1));
    QVERIFY2(ws.openWorkspace(ws1, err), qPrintable(err));
    QVERIFY(ws.hasOpenWorkspace());
}

void TestWorkspaceV381::testImportTagLibProvenanceCollisionSymlinkHardlink()
{
    auto& ws = WorkspaceManager::instance();
    const QString wsDir = m_tempDir->filePath("import_ws");
    const QString srcDir = m_tempDir->filePath("import_src");
    QDir().mkpath(wsDir);
    QDir().mkpath(srcDir);

    QString err;
    QVERIFY2(ws.registerAndOpenWorkspace(wsDir, err), qPrintable(err));

    const QString srcWav = srcDir + "/orig_song.wav";
    QVERIFY(createTestWav(srcWav, 48000.0, 4800, 440.0));
    const QByteArray origWavSha = computeSha256(srcWav);

    const QString srcFlac = srcDir + "/orig_song.flac";
    QVERIFY(createTestFlacViaFfmpeg(srcWav, srcFlac));
    const QByteArray origFlacSha = computeSha256(srcFlac);

    // 1. Symlink rejection
    const QString symPath = srcDir + "/sym_song.wav";
    QVERIFY(QFile::link(srcWav, symPath));
    const auto symRes = ws.importFile(symPath);
    QCOMPARE(symRes.status, ImportResultStatus::Failed);

    // 2. Hardlink (st_nlink >= 2) rejection
    const QString hardSrc = srcDir + "/hard_song.wav";
    QCOMPARE(::link(srcWav.toUtf8().constData(), hardSrc.toUtf8().constData()), 0);
    const auto hardRes = ws.importFile(hardSrc);
    QCOMPARE(hardRes.status, ImportResultStatus::Failed);
    QFile::remove(hardSrc); // restore nlink == 1 on srcWav

    // 3. Normal transactional import with TagLib provenance embedding & original immutability
    const auto wavRes = ws.importFile(srcWav);
    QCOMPARE(wavRes.status, ImportResultStatus::Imported);
    QVERIFY(wavRes.provenanceEmbedded);
    QCOMPARE(wavRes.originalSha256, QString::fromLatin1(origWavSha));
    // Original file SHA-256 MUST be identical (read-only guarantee)
    QCOMPARE(computeSha256(srcWav), origWavSha);

    const auto flacRes = ws.importFile(srcFlac);
    QCOMPARE(flacRes.status, ImportResultStatus::Imported);
    QVERIFY(flacRes.provenanceEmbedded);
    QCOMPARE(computeSha256(srcFlac), origFlacSha);

    // Verify provenance tags persisted inside workspace copies
    const auto readWavMeta = MetadataReader::parseTags(wavRes.destPath.toStdString());
    QVERIFY(readWavMeta.hasProvenance);
    QVERIFY(readWavMeta.provenanceValid);
    QCOMPARE(QString::fromStdString(readWavMeta.provenanceMarker), QString::fromLatin1(TrackMetadata::PROVENANCE_MARKER_V1));
    QCOMPARE(QString::fromStdString(readWavMeta.provenanceOriginalSha256), QString::fromLatin1(origWavSha));
    QVERIFY(!readWavMeta.provenanceOriginalPath.empty());
    QVERIFY(!readWavMeta.provenanceCopyTimestamp.empty());

    const auto readFlacMeta = MetadataReader::parseTags(flacRes.destPath.toStdString());
    QVERIFY(readFlacMeta.hasProvenance);
    QVERIFY(readFlacMeta.provenanceValid);
    QCOMPARE(QString::fromStdString(readFlacMeta.provenanceMarker), QString::fromLatin1(TrackMetadata::PROVENANCE_MARKER_V1));
    QCOMPARE(QString::fromStdString(readFlacMeta.provenanceOriginalSha256), QString::fromLatin1(origFlacSha));

    // 4. Duplicate import of exact same source file -> SkippedIdentical (no duplicate file created)
    const auto dupSame = ws.importFile(srcWav);
    QCOMPARE(dupSame.status, ImportResultStatus::SkippedIdentical);
    QCOMPARE(ws.listWorkspaceFiles().size(), size_t(2));

    // 5. Same filename with different audio contents -> Conflict (non-overwrite)
    QVERIFY(createTestWav(srcWav, 48000.0, 4800, 880.0));
    const QByteArray wsBeforeConflictSha = computeSha256(wavRes.destPath);
    const auto conflictRes = ws.importFile(srcWav);
    QCOMPARE(conflictRes.status, ImportResultStatus::Conflict);
    QCOMPARE(computeSha256(wavRes.destPath), wsBeforeConflictSha);

    // 6. .aac provenance support flag check (unsupported PropertyMap -> log fallback guarantee)
    const QString dummyAac = srcDir + "/raw_adts.aac";
    {
        QFile af(dummyAac);
        QVERIFY(af.open(QIODevice::WriteOnly));
        af.write("\xFF\xF1\x50\x80\x02\x1F\xFC", 7);
    }
    QVERIFY(!MetadataReader::supportsPropertyEmbedding(dummyAac.toStdString()));
    QVERIFY(MetadataReader::supportsPropertyEmbedding(flacRes.destPath.toStdString()));
    QVERIFY(MetadataReader::supportsPropertyEmbedding(wavRes.destPath.toStdString()));
}

void TestWorkspaceV381::testTagEditingCowCheckpointsDiffAuditionRollbackGc()
{
    auto& ws = WorkspaceManager::instance();
    EngineController& ctrl = EngineController::instance();
    QVERIFY(ctrl.initialize(true));

    const QString wsDir = m_tempDir->filePath("cow_save_ws");
    const QString srcDir = m_tempDir->filePath("cow_src");
    QDir().mkpath(wsDir);
    QDir().mkpath(srcDir);

    QString err;
    QVERIFY2(ws.registerAndOpenWorkspace(wsDir, err), qPrintable(err));

    const QString srcWav = srcDir + "/master_piece.wav";
    QVERIFY(createTestWav(srcWav, 48000.0, 4800, 440.0));
    const QString srcFlac = srcDir + "/master_piece.flac";
    QVERIFY(createTestFlacViaFfmpeg(srcWav, srcFlac));

    const auto imp = ws.importFile(srcFlac);
    QCOMPARE(imp.status, ImportResultStatus::Imported);

    // Set initial tags and Save 1
    TrackMetadata m1;
    m1.title = "First Movement";
    m1.artist = "Studio Ensemble";
    m1.album = "Session One";
    m1.trackNumber = 1;
    QVERIFY2(ws.editFileTags("master_piece.flac", m1, err), qPrintable(err));

    QString save1Id;
    QVERIFY2(ws.createSave("Take 1", save1Id, err), qPrintable(err));
    QVERIFY(!save1Id.isEmpty());

    // Edit tags again -> triggers Copy-on-Write lazy evacuation to .desk32/objects/
    TrackMetadata m2 = m1;
    m2.title = "First Movement (Remastered)";
    m2.trackNumber = 2;
    QVERIFY2(ws.editFileTags("master_piece.flac", m2, err), qPrintable(err));

    QString save2Id;
    QVERIFY2(ws.createSave("Take 2", save2Id, err), qPrintable(err));
    QVERIFY(!save2Id.isEmpty());

    // Diff Save 1 vs Save 2 -> must detect Modified + exact title diff
    const SaveComparisonResult diff = ws.compareSaves(save1Id, save2Id);
    QVERIFY2(diff.valid, qPrintable(diff.errorMessage));
    QCOMPARE(diff.modifiedCount, 1);
    QVERIFY(!diff.diffs.empty());
    bool foundTitleDiff = false;
    for (const auto& td : diff.diffs[0].tagDiffs) {
        if (td.key == "TITLE") {
            QCOMPARE(td.beforeValue, QString("First Movement"));
            QCOMPARE(td.afterValue, QString("First Movement (Remastered)"));
            foundTitleDiff = true;
        }
    }
    QVERIFY(foundTitleDiff);

    // Audition Take 1 file
    QString auditionPath;
    QVERIFY2(ws.resolveSavedTrackForAudition(save1Id, "master_piece.flac", auditionPath, err), qPrintable(err));
    QVERIFY(QFile::exists(auditionPath));
    const auto audMeta = MetadataReader::parseTags(auditionPath.toStdString());
    QCOMPARE(QString::fromStdString(audMeta.title), QString("First Movement"));

    // Make an unsaved tag edit so adoptSave creates an automatic pre-rollback checkpoint,
    // start playing on Deck A, then perform rollback -> StopPlaybackCallback stops playback and auto-save is created
    TrackMetadata m3 = m2;
    m3.title = "First Movement (Unsaved Draft)";
    QVERIFY2(ws.editFileTags("master_piece.flac", m3, err), qPrintable(err));

    QVERIFY(ctrl.loadTrack("A", wsDir + "/master_piece.flac"));
    ctrl.play("A");
    QCOMPARE(ctrl.deckManager()->deck('A')->state(), desk32::engine::deck::TransportState::Playing);

    const RollbackResult rb = ws.adoptSave(save1Id);
    QVERIFY2(rb.success, qPrintable(rb.errorMessage));
    QVERIFY(!rb.autoSaveId.isEmpty());
    QCOMPARE(ctrl.deckManager()->deck('A')->state(), desk32::engine::deck::TransportState::Stopped);

    // Live title restored to "First Movement"
    const auto liveAfterRb = MetadataReader::parseTags((wsDir + "/master_piece.flac").toStdString());
    QCOMPARE(QString::fromStdString(liveAfterRb.title), QString("First Movement"));

    // Delete saves (Save 2 + autoSave) -> saveStoreTotalBytes drops as deleteSave invokes GC,
    // and Save 1 objects remain protected; explicit runGarbageCollection cleans any orphan objects.
    const qint64 bytesBeforeDelete = ws.saveStoreTotalBytes();
    QVERIFY(bytesBeforeDelete > 0);
    QVERIFY(ws.deleteSave(save2Id, err));
    QVERIFY(ws.deleteSave(rb.autoSaveId, err));
    QVERIFY(ws.saveStoreTotalBytes() < bytesBeforeDelete);

    // Create an unreferenced orphan object and verify runGarbageCollection removes it
    const QString orphanObj = wsDir + "/.desk32/objects/0000orphan_test_hash";
    {
        QFile of(orphanObj);
        QVERIFY(of.open(QIODevice::WriteOnly));
        of.write("orphan");
    }
    const int removed = ws.runGarbageCollection();
    QVERIFY(removed >= 1);
    QVERIFY(!QFile::exists(orphanObj));
}

void TestWorkspaceV381::testRollbackPartialRecoveryFaultLogging()
{
    auto& ws = WorkspaceManager::instance();
    const QString wsDir = m_tempDir->filePath("fault_rb_ws");
    const QString srcDir = m_tempDir->filePath("fault_src");
    QDir().mkpath(wsDir);
    QDir().mkpath(srcDir);

    QString err;
    QVERIFY(ws.registerAndOpenWorkspace(wsDir, err));

    const QString w1 = srcDir + "/t1.wav";
    QVERIFY(createTestWav(w1, 48000.0, 2400, 440.0));
    QCOMPARE(ws.importFile(w1).status, ImportResultStatus::Imported);

    QString s1;
    QVERIFY(ws.createSave("S1", s1, err));

    // Modify file content via external edit / delete & re-import or another track
    QVERIFY(createTestWav(srcDir + "/t2.wav", 48000.0, 2400, 880.0));
    QCOMPARE(ws.importFile(srcDir + "/t2.wav").status, ImportResultStatus::Imported);

    // Remove extra t2.wav succeeds, then inject fault on restoring t1.wav -> recovery of t2.wav faults -> partialRecovery == true!
    ws.setTestFaultCallback([](WorkspaceFaultPoint pt, const QString& detailPath) -> bool {
        if (pt == WorkspaceFaultPoint::DuringRollbackApply && detailPath == "t1.wav") {
            return true;
        }
        return pt == WorkspaceFaultPoint::DuringRollbackRecover;
    });

    const RollbackResult rb = ws.adoptSave(s1);
    QVERIFY(!rb.success);
    QVERIFY(rb.partialRecovery);
    ws.setTestFaultCallback(nullptr);

    LogService::instance().flush();
    QVariantMap f;
    f["category"] = "Workspace";
    f["status"] = "Failed";
    const auto logs = LogService::instance().fetchLogs(f);
    bool found7013 = false;
    for (const auto& l : logs) {
        if (l.toMap().value("errorCode").toInt() == ErrorCode::WorkspacePartialRecovery) {
            found7013 = true;
            break;
        }
    }
    QVERIFY(found7013);
}

void TestWorkspaceV381::testExternalModificationTwoStageDetection()
{
    auto& ws = WorkspaceManager::instance();
    const QString wsDir = m_tempDir->filePath("ext_mod_ws");
    const QString srcDir = m_tempDir->filePath("ext_mod_src");
    QDir().mkpath(wsDir);
    QDir().mkpath(srcDir);

    QString err;
    QVERIFY(ws.registerAndOpenWorkspace(wsDir, err));

    const QString w1 = srcDir + "/watch_me.wav";
    QVERIFY(createTestWav(w1, 48000.0, 2400, 440.0));
    const auto imp = ws.importFile(w1);
    QCOMPARE(imp.status, ImportResultStatus::Imported);

    QSignalSpy modSpy(&ws, &WorkspaceManager::externalModificationDetected);

    // Externally tamper with file inside open workspace outside Desk32
    {
        QFile f(imp.destPath);
        QVERIFY(f.open(QIODevice::Append));
        f.write("TAMPER");
    }
    const QByteArray tamperedSha = computeSha256(imp.destPath);

    const QStringList detected = ws.verifyWorkspaceIntegrity();
    QVERIFY(!detected.isEmpty());
    QVERIFY(modSpy.count() >= 1);
    QVERIFY(ws.externalModificationCount() >= 1);

    // Must NOT auto-repair tampered file (§2.5)
    QCOMPARE(computeSha256(imp.destPath), tamperedSha);
}

void TestWorkspaceV381::testDirectModeSampleRateFollowAndBypass()
{
    EngineController& ctrl = EngineController::instance();
    QVERIFY(ctrl.initialize(true));

    const QString dir = m_tempDir->filePath("direct_mode_tracks");
    QDir().mkpath(dir);
    const QString wav96 = dir + "/sine_96k.wav";
    QVERIFY(createTestWav(wav96, 96000.0, 96000, 1000.0, 0.5));

    QVERIFY(ctrl.loadTrack("A", wav96));
    QCOMPARE(ctrl.graphSampleRate(), 48000.0);

    // Enable Direct Mode -> sample rate must follow active Deck A track (96000 Hz)
    ctrl.setDirectModeEnabled(true);
    QVERIFY(ctrl.isDirectModeEnabled());
    QCOMPARE(ctrl.graphSampleRate(), 96000.0);
    QCOMPARE(ctrl.audioOutput()->diagnostics().requestedSampleRate, uint32_t(96000));
    QVERIFY(ctrl.lastRenegotiationSilenceMs() >= 0.0);

    // Verify Direct Mode bypasses DSP (e.g. PEQ +12 dB) & software Master Gain (0.1)
    ctrl.setPeqBand(false, 8, 1000.0, 12.0, 1.0, 0, true);
    ctrl.setPeqBand(true, 8, 1000.0, 12.0, 1.0, 0, true);
    ctrl.setPeqBypassed(false);
    ctrl.setMasterGain(0.1);

    ctrl.play("A");
    auto* deckA = ctrl.deckManager()->deck('A');
    QTRY_VERIFY_WITH_TIMEOUT(deckA->worker().ringBuffer().availableRead() >= 2048, 3000);

    std::vector<float> directPcm(1024 * 2, 0.0f);
    ctrl.renderOfflineBlock(directPcm.data(), 1024);
    double directPeak = 0.0;
    for (float s : directPcm) directPeak = std::max(directPeak, std::abs(static_cast<double>(s)));
    // Sine amplitude was 0.5 -> unattenuated by 0.1 master gain and unboosted by +12 dB PEQ
    QVERIFY2(directPeak > 0.35 && directPeak < 0.65,
             qPrintable(QString("directPeak=%1").arg(directPeak)));

    // Turn Direct Mode OFF -> graph rate reverts to 48000, software gain 0.1 takes effect
    ctrl.setDirectModeEnabled(false);
    QCOMPARE(ctrl.graphSampleRate(), 48000.0);
    QTRY_VERIFY_WITH_TIMEOUT(deckA->worker().ringBuffer().availableRead() >= 2048, 3000);
    std::vector<float> nonDirectPcm(1024 * 2, 0.0f);
    for (int i = 0; i < 8; ++i) {
        ctrl.renderOfflineBlock(nonDirectPcm.data(), 1024);
    }
    double nonDirectPeak = 0.0;
    for (float s : nonDirectPcm) nonDirectPeak = std::max(nonDirectPeak, std::abs(static_cast<double>(s)));
    QVERIFY2(nonDirectPeak < 0.25, qPrintable(QString("nonDirectPeak=%1").arg(nonDirectPeak)));

    ctrl.stop("A");
    ctrl.setMasterGain(1.0);
    ctrl.setPeqBypassed(true);
}

void TestWorkspaceV381::testDeckLevelMatchPostTapAndOffsetFrames()
{
    EngineController& ctrl = EngineController::instance();
    QVERIFY(ctrl.initialize(true));

    const QString dir = m_tempDir->filePath("level_match_tracks");
    QDir().mkpath(dir);
    const QString wav = dir + "/tone.wav";
    QVERIFY(createTestWav(wav, 48000.0, 48000, 440.0, 0.5));
    QVERIFY(ctrl.loadTrack("A", wav));

    ctrl.setDeckOffsetFrames("A", 1234);
    QCOMPARE(ctrl.getDeckOffsetFrames("A"), qint64(1234));
    QCOMPARE(ctrl.getDeckState("A").value("offsetFrames").toLongLong(), qint64(1234));

    ctrl.setDeckLevelMatchDb("A", -12.0);
    QCOMPARE(ctrl.getDeckLevelMatchDb("A"), -12.0);
    QCOMPARE(ctrl.getDeckState("A").value("levelMatchDb").toDouble(), -12.0);

    ctrl.setMasterGain(0.5);
    ctrl.play("A");
    auto* deckA = ctrl.deckManager()->deck('A');
    QTRY_VERIFY_WITH_TIMEOUT(deckA->worker().ringBuffer().availableRead() >= 2048, 3000);
    std::vector<float> buf(1024 * 2, 0.0f);
    for (int i = 0; i < 8; ++i) {
        ctrl.renderOfflineBlock(buf.data(), 1024);
    }

    // Render Tap Peak MUST still reflect ~0.5 (post Master Chain, PRE Master Gain & Level Match)
    const double tapPeak = ctrl.renderTapPeakL();
    const double outPeak = ctrl.masterPeakL();
    QVERIFY2(tapPeak > 0.40 && tapPeak < 0.55, qPrintable(QString("tapPeak=%1").arg(tapPeak)));
    // Out peak reflects 0.5 * 0.5 * 10^(-12/20) (~0.063)
    QVERIFY2(outPeak > 0.03 && outPeak < 0.12, qPrintable(QString("outPeak=%1").arg(outPeak)));

    ctrl.stop("A");
    ctrl.setDeckLevelMatchDb("A", 0.0);
    ctrl.setMasterGain(1.0);
}

void TestWorkspaceV381::testPeq16BandsHpfLpfQMasterDspPresetAndPlaybackHistory()
{
    EngineController& ctrl = EngineController::instance();
    QVERIFY(ctrl.initialize(true));

    // 1. 16-Band PEQ per channel (index 15 accessible & preserved)
    QCOMPARE(ParametricEQ::BANDS_PER_CHANNEL, size_t(16));
    ctrl.setPeqBand(false, 15, 17500.0, -3.5, 2.2, 2 /*HighShelf*/, true);
    ctrl.setPeqBand(true, 14, 13000.0, 2.5, 1.5, 0 /*Peak*/, true);
    ctrl.setPeqBypassed(false);

    // 2. Variable HPF Q & LPF Q [0.1, 10.0]
    ctrl.setFilterParams(120.0, true, 1, 8000.0, true, 0, 3.5, 0.3);
    ctrl.setDspNodeBypass("Filter", false);
    const QVariantMap fltMap = ctrl.getFilterParams();
    QVERIFY(std::abs(fltMap.value("hpfQ").toDouble() - 3.5) < 1e-4);
    QVERIFY(std::abs(fltMap.value("lpfQ").toDouble() - 0.3) < 1e-4);

    // 3. Configure other nodes & save full 6-node master_dsp preset
    ctrl.setGeqBandGain(0, -4.0);
    ctrl.setGeqBandGain(31, 3.0);
    ctrl.setGeqBypassed(false);
    ctrl.setCompressorParams(-18.0, 4.5, 5.0, 80.0, 2.5, 4.0);
    ctrl.setDspNodeBypass("Compressor", false);
    ctrl.setLimiterParams(-1.2, 40.0, 1.5);
    ctrl.setDspNodeBypass("Limiter", false);
    ctrl.setReverbParams(0.65, 0.4, 0.25, 12.0, true);
    ctrl.setDspNodeBypass("Reverb", false);

    const QVariantMap captured = ctrl.captureCurrentStateAsPreset("master_dsp");
    QVERIFY(ctrl.savePreset("master_dsp", "test_full_master", captured));

    // Scrambling all parameters & bypassing all nodes
    ctrl.setPeqBand(false, 15, 1000.0, 0.0, 1.0, 0, false);
    ctrl.setGeqBandGain(0, 0.0);
    ctrl.setFilterParams(20.0, false, 0, 20000.0, false, 0, 0.707, 0.707);
    ctrl.setPeqBypassed(true);
    ctrl.setGeqBypassed(true);
    ctrl.setDspNodeBypass("Filter", true);
    ctrl.setDspNodeBypass("Compressor", true);
    ctrl.setDspNodeBypass("Limiter", true);
    ctrl.setDspNodeBypass("Reverb", true);

    // Apply saved 6-node preset -> all nodes & Q & band 15 restored
    QVERIFY(ctrl.applyPresetProfile("master_dsp", "test_full_master"));
    QCOMPARE(ctrl.isPeqBypassed(), false);
    QCOMPARE(ctrl.isGeqBypassed(), false);
    QCOMPARE(ctrl.isDspNodeBypassed("Filter"), false);
    QCOMPARE(ctrl.isDspNodeBypassed("Compressor"), false);
    QCOMPARE(ctrl.isDspNodeBypassed("Limiter"), false);
    QCOMPARE(ctrl.isDspNodeBypassed("Reverb"), false);
    QCOMPARE(ctrl.getGeqBandGain(0), -4.0);
    QCOMPARE(ctrl.getGeqBandGain(31), 3.0);
    QCOMPARE(ctrl.getPeqBand(false, 15).value("gainDb").toDouble(), -3.5);
    QCOMPARE(ctrl.getPeqBand(true, 14).value("gainDb").toDouble(), 2.5);
    QVERIFY(std::abs(ctrl.getFilterParams().value("hpfQ").toDouble() - 3.5) < 1e-4);
    QVERIFY(std::abs(ctrl.getFilterParams().value("lpfQ").toDouble() - 0.3) < 1e-4);
    QCOMPARE(ctrl.getCompressorParams().value("thresholdDb").toDouble(), -18.0);
    QCOMPARE(ctrl.getLimiterParams().value("ceilingDb").toDouble(), -1.2);
    QCOMPARE(ctrl.getReverbParams().value("roomSize").toDouble(), 0.65);

    // 4. Playback history extraction from SQLite log
    const QString histWav = m_tempDir->filePath("history_check.wav");
    QVERIFY(createTestWav(histWav, 48000.0, 2400));
    QVERIFY(ctrl.loadTrack("A", histWav));
    ctrl.play("A");
    ctrl.stop("A");
    LogService::instance().flush();

    const QVariantList history = LogService::instance().fetchPlaybackHistory(50);
    QVERIFY(!history.isEmpty());
    bool foundHist = false;
    for (const auto& item : history) {
        const QVariantMap row = item.toMap();
        if (row.value("fileName").toString() == "history_check.wav") {
            foundHist = true;
            QCOMPARE(row.value("deck").toString(), QString("A"));
            break;
        }
    }
    QVERIFY(foundHist);
}

QTEST_GUILESS_MAIN(TestWorkspaceV381)
#include "test_workspace_v381.moc"
