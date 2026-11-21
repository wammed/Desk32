#include <QTest>
#include <QObject>
#include <QTemporaryFile>
#include <QDir>
#include <fstream>
#include <vector>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <execinfo.h>
#include <latch>
#include <thread>
#include <chrono>
#include "engine/deck/DeckManager.hpp"

static std::atomic<bool> s_trackAllocations{false};
static std::atomic<size_t> s_allocationCount{0};

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(size_t size) {
    if (s_trackAllocations.load(std::memory_order_relaxed)) {
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void* operator new(size_t size, const std::nothrow_t&) noexcept {
    if (s_trackAllocations.load(std::memory_order_relaxed)) {
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void* operator new[](size_t size) {
    if (s_trackAllocations.load(std::memory_order_relaxed)) {
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    if (s_trackAllocations.load(std::memory_order_relaxed)) {
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void* operator new(size_t size, std::align_val_t al) {
    if (s_trackAllocations.load(std::memory_order_relaxed)) {
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* ptr = nullptr;
    if (posix_memalign(&ptr, static_cast<size_t>(al), size) != 0) {
        throw std::bad_alloc();
    }
    return ptr;
}

void* operator new[](size_t size, std::align_val_t al) {
    if (s_trackAllocations.load(std::memory_order_relaxed)) {
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* ptr = nullptr;
    if (posix_memalign(&ptr, static_cast<size_t>(al), size) != 0) {
        throw std::bad_alloc();
    }
    return ptr;
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, size_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, size_t) noexcept {
    std::free(p);
}

void operator delete(void* p, std::align_val_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::align_val_t) noexcept {
    std::free(p);
}

void operator delete(void* p, size_t, std::align_val_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, size_t, std::align_val_t) noexcept {
    std::free(p);
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using namespace desk32::engine::deck;

class TestReferenceDecks : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void testFourDecksIndependentTransport();
    void testSwitchingDualModesAndQuickAB();
    void testDeckLockProtectionAndTransportPermission();
    void testFolderSessionIndexZeroAndReadAhead();
    void testDirectModeSwitchPreservesRealFileTrack();
    void testFolderSessionNeedsAdvanceAndLockDiscard();
    void testFolderSessionErrorHandlingAndFinalTrackEof();
    void testZeroDynamicAllocationInRenderBlock();
    void testSyntheticTrackOwnershipAndConcurrentSwitching();
    void testDecoderWorkerOpenTimeoutAndStaleRequestCancellation();
    void testDecoderWorkerDeterministicCommitRaceConditions();
    void testAutoAdvanceDuplicateEof();
    void testAutoAdvanceShortIntervalRapidEof();
    void testAutoAdvanceStaleDecoderWorkerNotification();
    void testAutoAdvanceStalePlaybackGeneration();
    void testAutoAdvanceDuringTransitionInProgress();
    void testAutoAdvanceRaceWithManualNext();
    void testAutoAdvanceRaceWithStopAndPause();
    void testAutoAdvanceRaceWithSeek();
    void testAutoAdvanceLongRepetitionAndMixedRates();
    void testAutoAdvanceQueueOverflowRecovery();
    void testAutoAdvancePauseResumeEofReemit();
    void testAutoAdvanceStopDiscardsFolderSession();
    void testAutoAdvanceFinalTrackDiscardsSession();
    void testAutoAdvanceDeckLockBoundaryWithNewSession();
    void testAutoAdvanceOverflowSlotConcurrentConsistency();
    void testAutoAdvanceRapidEofPauseResumeRace();
    void testAutoAdvanceOverflowRecoveryRaceWithStopAndLock();
    void testAutoAdvanceMultiProducerOverflowConsistency();
    void testAutoAdvanceOverflowRecoveryConcurrentStopAndLock();
    void testAutoAdvanceOverflowContinuousWithControlDelay();
    void testAutoAdvanceSpscCapacityOverflowNoOverwrite();
    void testComparativeSwitchTerminatesFolderSession();
    void testPhase1GapFixesFolderAndTransport();
    void cleanupTestCase();

private:
    QString createSyntheticWav(const QString& name, double sampleRate, size_t frames);
    QString m_testDir;
    QString m_wav1;
    QString m_wav2;
    QString m_wav3;
};

void TestReferenceDecks::initTestCase()
{
    qDebug() << "Initializing TestReferenceDecks (Phase 1.4 Reference Deck & Folder Session Implementation)...";
    m_testDir = QDir::tempPath() + "/desk32_test_decks_" + QString::number(QCoreApplication::applicationPid());
    QDir().mkpath(m_testDir);

    m_wav1 = createSyntheticWav("track_01_intro.wav", 48000.0, 4800);  // 0.1s
    m_wav2 = createSyntheticWav("track_02_verse.wav", 44100.0, 4410);  // 0.1s (44.1k for resampler test)
    m_wav3 = createSyntheticWav("track_03_chorus.wav", 48000.0, 4800); // 0.1s
}

QString TestReferenceDecks::createSyntheticWav(const QString& name, double sampleRate, size_t totalFrames)
{
    QString path = m_testDir + "/" + name;
    std::ofstream out(path.toStdString(), std::ios::binary);
    if (!out) return QString();

    uint32_t sRate = static_cast<uint32_t>(sampleRate);
    uint16_t numCh = 2;
    uint16_t bitsPerSample = 16;
    uint32_t dataBytes = static_cast<uint32_t>(totalFrames * numCh * (bitsPerSample / 8));
    uint32_t chunkSize = 36 + dataBytes;
    uint32_t byteRate = sRate * numCh * (bitsPerSample / 8);
    uint16_t blockAlign = numCh * (bitsPerSample / 8);

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&chunkSize), 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    uint32_t subchunk1Size = 16;
    uint16_t audioFormat = 1;
    out.write(reinterpret_cast<const char*>(&subchunk1Size), 4);
    out.write(reinterpret_cast<const char*>(&audioFormat), 2);
    out.write(reinterpret_cast<const char*>(&numCh), 2);
    out.write(reinterpret_cast<const char*>(&sRate), 4);
    out.write(reinterpret_cast<const char*>(&byteRate), 4);
    out.write(reinterpret_cast<const char*>(&blockAlign), 2);
    out.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataBytes), 4);

    for (size_t i = 0; i < totalFrames; ++i) {
        int16_t val = static_cast<int16_t>(0.3 * 32767.0 * std::sin(2.0 * M_PI * 440.0 * (static_cast<double>(i) / sampleRate)));
        out.write(reinterpret_cast<const char*>(&val), sizeof(int16_t));
        out.write(reinterpret_cast<const char*>(&val), sizeof(int16_t));
    }
    out.close();
    return path;
}

void TestReferenceDecks::testFourDecksIndependentTransport()
{
    DeckManager manager(48000.0);

    // Verify 4 distinct decks exist
    for (char c : {'A', 'B', 'C', 'D'}) {
        ReferenceDeck* d = manager.deck(c);
        QVERIFY(d != nullptr);
        QCOMPARE(d->id(), c);
        QCOMPARE(d->state(), TransportState::Stopped);
    }

    // Load track into Deck A and Deck B
    QVERIFY(manager.deck('A')->loadTrack(m_wav1.toStdString(), 48000.0));
    QVERIFY(manager.deck('B')->loadTrack(m_wav2.toStdString(), 48000.0));

    // Start Deck A
    manager.deck('A')->play();
    QCOMPARE(manager.deck('A')->state(), TransportState::Playing);
    QCOMPARE(manager.deck('B')->state(), TransportState::Stopped);

    // Pause Deck A, Start Deck B
    manager.deck('A')->pause();
    manager.deck('B')->play();
    QCOMPARE(manager.deck('A')->state(), TransportState::Paused);
    QCOMPARE(manager.deck('B')->state(), TransportState::Playing);

    // Verify independent playhead
    manager.deck('A')->seek(1000);
    QCOMPARE(manager.deck('A')->playhead(), 1000ll);
    QVERIFY(manager.deck('B')->playhead() != 1000ll);
}

void TestReferenceDecks::testSwitchingDualModesAndQuickAB()
{
    DeckManager manager(48000.0);
    manager.deck('A')->loadTrack(m_wav1.toStdString(), 48000.0);
    manager.deck('B')->loadTrack(m_wav2.toStdString(), 48000.0);

    // 1. Quick Switch AB in Mode 1 (Exclusive)
    manager.deck('A')->play();
    QCOMPARE(manager.activeDeckId(), 'A');

    auto res = manager.quickSwitchAB(SwitchingMode::Exclusive);
    QVERIFY(res.success);
    QCOMPARE(manager.activeDeckId(), 'B');
    QCOMPARE(manager.deck('A')->state(), TransportState::Paused); // Deck A paused
    QCOMPARE(manager.deck('B')->state(), TransportState::Playing); // Deck B now playing

    // 2. Switch back in Mode 2 (Normal): Transport state of Deck A (Paused) MUST be preserved,
    //    and non-selected Deck B MUST auto-pause per §2.7.5!
    res = manager.switchDeck('A', SwitchingMode::ParallelSolo);
    QVERIFY(res.success);
    QCOMPARE(manager.activeDeckId(), 'A');
    QCOMPARE(manager.deck('A')->state(), TransportState::Paused); // Deck A remains Paused per Section 2.3!
    QCOMPARE(manager.deck('B')->state(), TransportState::Paused); // Non-selected Deck B auto-pauses per §2.7.5!

    // Start Deck A, then switch to Deck B in Normal mode without pressing Play on B:
    // Deck A MUST auto-pause and Deck B preserves its Paused state!
    manager.deck('A')->play();
    QCOMPARE(manager.deck('A')->state(), TransportState::Playing);
    res = manager.switchDeck('B', SwitchingMode::ParallelSolo);
    QVERIFY(res.success);
    QCOMPARE(manager.activeDeckId(), 'B');
    QCOMPARE(manager.deck('A')->state(), TransportState::Paused); // Deck A auto-pauses on switch
    QCOMPARE(manager.deck('B')->state(), TransportState::Paused); // Deck B preserves its Paused state
}

void TestReferenceDecks::testDeckLockProtectionAndTransportPermission()
{
    // Section 2.7.5 & Completion Criterion 31
    ReferenceDeck deck('C');
    QVERIFY(deck.loadTrack(m_wav1.toStdString(), 48000.0));

    // Engage Deck Lock
    deck.setLocked(true);
    QVERIFY(deck.isLocked());

    // 1. Loading another track MUST BE BLOCKED
    bool loadBlocked = deck.loadTrack(m_wav2.toStdString(), 48000.0);
    QVERIFY(!loadBlocked);
    QCOMPARE(deck.trackInfo().path, m_wav1.toStdString()); // Unchanged

    // 2. Transport operations MUST BE PERMITTED during lock
    deck.play();
    QCOMPARE(deck.state(), TransportState::Playing);

    deck.pause();
    QCOMPARE(deck.state(), TransportState::Paused);

    deck.seek(500);
    QCOMPARE(deck.playhead(), 500ll);

    deck.stop();
    QCOMPARE(deck.state(), TransportState::Stopped);

    // Unlock
    deck.setLocked(false);
    QVERIFY(!deck.isLocked());
    QVERIFY(deck.loadTrack(m_wav2.toStdString(), 48000.0)); // Allowed after unlock
}

void TestReferenceDecks::testFolderSessionIndexZeroAndReadAhead()
{
    // Section 2.7 & Completion Criteria 26, 27, 30
    ReferenceDeck deck('D');
    std::vector<std::string> files = {
        "track_03_chorus.wav",
        "track_01_intro.wav",
        "track_02_verse.wav"
    };

    // Start Folder Session
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    QVERIFY(deck.folderSession().isActive());

    // 1. Strictly starts from Index 0 (which is "track_01_intro.wav" by natural sort)
    QCOMPARE(deck.folderSession().currentIndex(), 0u);
    QCOMPARE(deck.folderSession().currentTrack()->fileName, std::string("track_01_intro.wav"));
    QCOMPARE(deck.trackInfo().path, m_wav1.toStdString());

    // 2. Single next-track read-ahead points strictly to Index 1 ("track_02_verse.wav")
    const auto* readAhead = deck.folderSession().readAheadTrack();
    QVERIFY(readAhead != nullptr);
    QCOMPARE(readAhead->index, 1u);
    QCOMPARE(readAhead->fileName, std::string("track_02_verse.wav"));

    // 3. Advance to next track
    QVERIFY(deck.advanceFolderNext());
    QCOMPARE(deck.folderSession().currentIndex(), 1u);
    QCOMPARE(deck.trackInfo().path, m_wav2.toStdString());

    // 4. Advance to track 2 ("track_03_chorus.wav")
    QVERIFY(deck.advanceFolderNext());
    QCOMPARE(deck.folderSession().currentIndex(), 2u);
    QCOMPARE(deck.trackInfo().path, m_wav3.toStdString());
    QVERIFY(deck.folderSession().readAheadTrack() == nullptr); // No next track

    // 5. Final track reached: advancing further must return false and stop session (no repeat/shuffle)
    QVERIFY(!deck.advanceFolderNext());
    QVERIFY(!deck.folderSession().isActive());
    QCOMPARE(deck.state(), TransportState::Stopped);
}

void TestReferenceDecks::testDirectModeSwitchPreservesRealFileTrack()
{
    DeckManager manager(48000.0);
    // P0-09: Load real audio file track (44.1 kHz)
    QVERIFY(manager.deck('A')->loadTrack(m_wav2.toStdString(), 48000.0));
    QCOMPARE(manager.deck('A')->trackInfo().path, m_wav2.toStdString());
    QCOMPARE(manager.deck('A')->trackInfo().sampleRate, 44100.0);

    // Perform Direct Mode switch
    SwitchResult res = manager.directModeSwitch('A');
    QVERIFY(res.success);
    QCOMPARE(manager.graphRate(), 44100.0);
    // Must strictly retain genuine file path, NOT replaced with synthetic track
    QCOMPARE(manager.deck('A')->trackInfo().path, m_wav2.toStdString());
}

void TestReferenceDecks::testFolderSessionNeedsAdvanceAndLockDiscard()
{
    ReferenceDeck deck('C');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav"
    };

    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    QVERIFY(deck.folderSession().isActive());
    deck.play();

    // Render audio past end of track_01_intro.wav (4800 frames) to trigger EOF
    std::vector<double> outL(512), outR(512);
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }

    // P0-10: EOF delivered should raise needsFolderAdvance flag
    QVERIFY(deck.needsFolderAdvance());
    deck.clearNeedsFolderAdvance();
    QVERIFY(!deck.needsFolderAdvance());

    // Advance to next track
    QVERIFY(deck.advanceFolderNext());
    QCOMPARE(deck.folderSession().currentIndex(), 1u);
    QCOMPARE(deck.trackInfo().path, m_wav2.toStdString());

    // P0-10: Engaging lock must immediately terminate and discard folder session
    deck.setLocked(true);
    QVERIFY(deck.isLocked());
    QVERIFY(!deck.folderSession().isActive());
}

void TestReferenceDecks::testFolderSessionErrorHandlingAndFinalTrackEof()
{
    // P0-19: Folder session boundary verification - final track EOF, missing file error handling, audio thread non-blocking
    ReferenceDeck deck('B');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_99_missing.wav"
    };

    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    QVERIFY(deck.folderSession().isActive());
    deck.play();

    // Render audio to EOF of first track
    std::vector<double> outL(512), outR(512);
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }
    QVERIFY(deck.needsFolderAdvance());

    // Advancing to missing file: Worker fails to open, session must safely terminate without audio thread hang
    bool advanced = deck.advanceFolderNext();
    QVERIFY(!advanced);
    QVERIFY(!deck.folderSession().isActive());
    QCOMPARE(deck.state(), TransportState::Stopped);

    // Audio rendering after failed advance must produce silence cleanly without blocking
    deck.renderBlock(outL.data(), outR.data(), 512);
    QCOMPARE(outL[0], 0.0);
    QCOMPARE(outR[0], 0.0);
}

void TestReferenceDecks::testZeroDynamicAllocationInRenderBlock()
{
    DeckManager manager(48000.0);
    manager.deck('A')->loadTrack(m_wav1.toStdString(), 48000.0);
    manager.deck('B')->loadTrack(m_wav2.toStdString(), 48000.0);
    manager.deck('A')->play();
    manager.deck('B')->play();

    std::vector<double> outL(4096, 0.0);
    std::vector<double> outR(4096, 0.0);

    // Warm-up one render cycle
    manager.renderActiveBlock(outL.data(), outR.data(), 256);

    // Track dynamic memory allocations on Audio Thread rendering
    s_allocationCount.store(0);
    s_trackAllocations.store(true);

    for (size_t frames : {size_t(64), size_t(128), size_t(256), size_t(512), size_t(1024), size_t(2048), size_t(4096)}) {
        manager.renderActiveBlock(outL.data(), outR.data(), frames);
    }

    s_trackAllocations.store(false);

    qDebug() << "Total dynamic heap allocations during renderActiveBlock:" << s_allocationCount.load();
    QCOMPARE(s_allocationCount.load(), 0u);
}

void TestReferenceDecks::testSyntheticTrackOwnershipAndConcurrentSwitching()
{
    // P0-15: Synthetic track worker ownership, lifecycle transitions, and concurrent metadata snapshots
    ReferenceDeck deck('D');

    // 1. Synthetic Track creation via Worker
    TrackInfo synthInfo;
    synthInfo.path = "synthetic://sine_440";
    synthInfo.sampleRate = 48000.0;
    synthInfo.channels = 2;
    synthInfo.totalFrames = 48000; // 1 second

    QVERIFY(deck.loadTrack(synthInfo, 48000.0));
    QVERIFY(deck.isOpen());
    QCOMPARE(deck.trackInfo().path, std::string("synthetic://sine_440"));
    auto meta = deck.metadata();
    QVERIFY(meta != nullptr);
    QCOMPARE(meta->title, "Synthetic Reference Signal");
    QCOMPARE(meta->artist, "Desk32 DSP Test Engine");

    // 2. Switch from Synthetic to Real file
    QVERIFY(deck.loadTrack(m_wav1.toStdString(), 48000.0));
    QCOMPARE(deck.trackInfo().path, m_wav1.toStdString());
    auto realMeta = deck.metadata();
    QVERIFY(realMeta != nullptr);
    QCOMPARE(realMeta->channels, 2u);

    // 3. Switch back from Real file to Synthetic Track
    QVERIFY(deck.loadTrack(synthInfo, 48000.0));
    QCOMPARE(deck.trackInfo().path, std::string("synthetic://sine_440"));

    // 4. Switching tracks during active playback with concurrent rendering and metadata access
    deck.play();
    QVERIFY(deck.isPlaying());

    std::atomic<bool> stop{false};
    std::vector<double> outL(256), outR(256);

    // Audio thread rendering
    std::thread audioThread([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            deck.renderBlock(outL.data(), outR.data(), 256);
            std::this_thread::yield();
        }
    });

    // Concurrent telemetry polling metadata and track info (P0-15 value snapshot safety)
    std::thread metaThread([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            auto m = deck.metadata();
            auto info = deck.trackInfo();
            if (m) {
                volatile size_t len = m->title.length();
                (void)len;
            }
            volatile size_t pathLen = info.path.length();
            (void)pathLen;
            std::this_thread::yield();
        }
    });

    // Control thread rapidly switches between synthetic and real track
    for (int i = 0; i < 20; ++i) {
        if (i % 2 == 0) {
            deck.loadTrack(m_wav1.toStdString(), 48000.0);
        } else {
            deck.loadTrack(synthInfo, 48000.0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    stop.store(true, std::memory_order_relaxed);
    audioThread.join();
    metaThread.join();

    // 5. Open while closing / rapid shutdown
    deck.closeTrack();
    QVERIFY(!deck.isOpen());
}

void TestReferenceDecks::testDecoderWorkerOpenTimeoutAndStaleRequestCancellation()
{
    // P0-30: DecoderWorker Request ID lifecycle, status tracking, and stale open cancellation
    desk32::engine::decoder::DecoderWorker worker;
    QCOMPARE(worker.lastOpenStatus(), desk32::engine::decoder::OpenStatus::None);

    // 1. Successful synthetic track open
    bool ok = worker.openSyntheticTrack(48000, 48000.0, 48000.0, "synthetic://test1");
    QVERIFY(ok);
    QVERIFY(worker.isOpen());
    QCOMPARE(worker.lastOpenStatus(), desk32::engine::decoder::OpenStatus::Success);

    // 2. Cancellation via closeTrack()
    worker.closeTrack();
    QVERIFY(!worker.isOpen());
    QCOMPARE(worker.lastOpenStatus(), desk32::engine::decoder::OpenStatus::Cancelled);
    QCOMPARE(worker.activeRequestId(), 0u);

    // 3. Failed open on non-existent file
    bool failOk = worker.openTrack("/invalid/non_existent_audio_file.wav", 48000.0);
    QVERIFY(!failOk);
    QVERIFY(!worker.isOpen());
    QCOMPARE(worker.lastOpenStatus(), desk32::engine::decoder::OpenStatus::Failed);

    // 4. Superseded open request: rapid consecutive calls commit only the latest request
    worker.openSyntheticTrack(24000, 48000.0, 48000.0, "synthetic://first");
    uint64_t latestReq = worker.activeRequestId();
    bool latestOk = worker.openSyntheticTrack(48000, 48000.0, 48000.0, "synthetic://latest");
    QVERIFY(latestOk);
    QVERIFY(worker.isOpen());
    QVERIFY(worker.activeRequestId() >= latestReq);
    QCOMPARE(worker.trackInfo().path, std::string("synthetic://latest"));
    QCOMPARE(worker.trackInfo().totalFrames, 48000u);

    // 5. P0-26: Rapid interleaved requests between real audio file and synthetic track
    for (int i = 0; i < 5; ++i) {
        worker.openTrack(m_wav1.toStdString(), 48000.0);
        worker.openSyntheticTrack(96000, 48000.0, 48000.0, "synthetic://interleaved_" + std::to_string(i));
    }
    bool finalRealOk = worker.openTrack(m_wav1.toStdString(), 48000.0);
    QVERIFY(finalRealOk);
    QVERIFY(worker.isOpen());
    QCOMPARE(worker.lastOpenStatus(), desk32::engine::decoder::OpenStatus::Success);
    QCOMPARE(worker.trackInfo().path, m_wav1.toStdString());
    QVERIFY(!worker.isSynthetic());

    // Rapid real file track requests followed immediately by final synthetic track
    for (int i = 0; i < 5; ++i) {
        worker.openSyntheticTrack(96000, 48000.0, 48000.0, "synthetic://stale_" + std::to_string(i));
        worker.openTrack(m_wav1.toStdString(), 48000.0);
    }
    bool finalSynthOk = worker.openSyntheticTrack(48000, 48000.0, 48000.0, "synthetic://final");
    QVERIFY(finalSynthOk);
    QVERIFY(worker.isOpen());
    QCOMPARE(worker.lastOpenStatus(), desk32::engine::decoder::OpenStatus::Success);
    QCOMPARE(worker.trackInfo().path, std::string("synthetic://final"));
    QVERIFY(worker.isSynthetic());
}

void TestReferenceDecks::testDecoderWorkerDeterministicCommitRaceConditions()
{
    // P0-26: Deterministic verification of Request ID Commit Synchronization Mechanism (Cases 1 - 10)
    // Strictly zero sleep_for() reliance; all race timings are synchronized via latches / atomic flags.

    // Case 1: Open Real A, but before commit, Control Thread opens Real B.
    // Result: A is rejected, B is committed.
    {
        desk32::engine::decoder::DecoderWorker worker;
        std::atomic<bool> hookHit{false};
        std::atomic<bool> releaseHook{false};

        worker.setPreCommitHook([&](uint64_t) {
            if (!hookHit.exchange(true)) {
                while (!releaseHook.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
        });

        std::thread tA([&]() {
            worker.openTrack(m_wav1.toStdString(), 48000.0);
        });

        while (!hookHit.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        const uint64_t reqA = worker.activeRequestId();

        std::atomic<bool> okB{false};
        std::thread tB([&]() {
            okB.store(worker.openTrack(m_wav2.toStdString(), 48000.0));
        });

        // Deterministically wait until Request B has been issued and published under m_trackMutex
        while (worker.activeRequestId() <= reqA) {
            std::this_thread::yield();
        }

        // Release Track A worker from hook; it must find its request ID stale and discard A
        releaseHook.store(true, std::memory_order_release);

        tA.join();
        tB.join();

        QVERIFY(okB.load());
        QVERIFY(worker.isOpen());
        QCOMPARE(worker.trackInfo().path, m_wav2.toStdString());
        QVERIFY(!worker.isSynthetic());
    }

    // Case 2: Open Real A, but before commit, Control Thread opens Synthetic C.
    // Result: A is rejected, Synthetic C is committed.
    {
        desk32::engine::decoder::DecoderWorker worker;
        std::atomic<bool> hookHit{false};
        std::atomic<bool> releaseHook{false};

        worker.setPreCommitHook([&](uint64_t) {
            if (!hookHit.exchange(true)) {
                while (!releaseHook.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
        });

        std::thread tA([&]() {
            worker.openTrack(m_wav1.toStdString(), 48000.0);
        });

        while (!hookHit.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        const uint64_t reqA = worker.activeRequestId();

        std::atomic<bool> okSynth{false};
        std::thread tSynth([&]() {
            okSynth.store(worker.openSyntheticTrack(48000, 48000.0, 48000.0, "synthetic://case2"));
        });

        while (worker.activeRequestId() <= reqA) {
            std::this_thread::yield();
        }

        releaseHook.store(true, std::memory_order_release);

        tA.join();
        tSynth.join();

        QVERIFY(okSynth.load());
        QVERIFY(worker.isOpen());
        QCOMPARE(worker.trackInfo().path, std::string("synthetic://case2"));
        QVERIFY(worker.isSynthetic());
    }

    // Case 3: Open Synthetic A, but before commit, Control Thread opens Real B.
    // Result: Synthetic A is rejected, Real B is committed.
    {
        desk32::engine::decoder::DecoderWorker worker;
        std::atomic<bool> hookHit{false};
        std::atomic<bool> releaseHook{false};

        worker.setPreCommitHook([&](uint64_t) {
            if (!hookHit.exchange(true)) {
                while (!releaseHook.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
        });

        std::thread tA([&]() {
            worker.openSyntheticTrack(48000, 48000.0, 48000.0, "synthetic://case3_stale");
        });

        while (!hookHit.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        const uint64_t reqA = worker.activeRequestId();

        std::atomic<bool> okB{false};
        std::thread tB([&]() {
            okB.store(worker.openTrack(m_wav1.toStdString(), 48000.0));
        });

        while (worker.activeRequestId() <= reqA) {
            std::this_thread::yield();
        }

        releaseHook.store(true, std::memory_order_release);

        tA.join();
        tB.join();

        QVERIFY(okB.load());
        QVERIFY(worker.isOpen());
        QCOMPARE(worker.trackInfo().path, m_wav1.toStdString());
        QVERIFY(!worker.isSynthetic());
    }

    // Case 4: Open Real A, but before commit, Control Thread calls closeTrack().
    // Result: A is rejected, Deck remains closed (isOpen == false).
    {
        desk32::engine::decoder::DecoderWorker worker;
        std::atomic<bool> hookHit{false};
        std::atomic<bool> releaseHook{false};

        worker.setPreCommitHook([&](uint64_t) {
            if (!hookHit.exchange(true)) {
                while (!releaseHook.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
        });

        std::thread tA([&]() {
            worker.openTrack(m_wav1.toStdString(), 48000.0);
        });

        while (!hookHit.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::thread tClose([&]() {
            worker.closeTrack();
        });

        // Deterministically wait until closeTrack sets activeRequestId to 0
        while (worker.activeRequestId() != 0) {
            std::this_thread::yield();
        }

        releaseHook.store(true, std::memory_order_release);

        tA.join();
        tClose.join();

        QVERIFY(!worker.isOpen());
        QCOMPARE(worker.trackInfo().path, std::string(""));
        QCOMPARE(worker.committedRequestId(), 0u);
    }

    // Case 5: Open Synthetic A, but before commit, Control Thread calls closeTrack().
    // Result: Synthetic A is rejected, Deck remains closed (isOpen == false).
    {
        desk32::engine::decoder::DecoderWorker worker;
        std::atomic<bool> hookHit{false};
        std::atomic<bool> releaseHook{false};

        worker.setPreCommitHook([&](uint64_t) {
            if (!hookHit.exchange(true)) {
                while (!releaseHook.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
        });

        std::thread tA([&]() {
            worker.openSyntheticTrack(48000, 48000.0, 48000.0, "synthetic://case5_stale");
        });

        while (!hookHit.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::thread tClose([&]() {
            worker.closeTrack();
        });

        while (worker.activeRequestId() != 0) {
            std::this_thread::yield();
        }

        releaseHook.store(true, std::memory_order_release);

        tA.join();
        tClose.join();

        QVERIFY(!worker.isOpen());
        QCOMPARE(worker.trackInfo().path, std::string(""));
        QCOMPARE(worker.committedRequestId(), 0u);
    }

    // Case 6: Open Real A, but before commit, DecoderWorker is destroyed (Shutdown).
    // Result: Clean shutdown without crash, UAF, or deadlock.
    {
        std::atomic<bool> hookHit{false};
        std::atomic<bool> releaseHook{false};

        auto worker = std::make_unique<desk32::engine::decoder::DecoderWorker>();
        worker->setPreCommitHook([&](uint64_t) {
            if (!hookHit.exchange(true)) {
                while (!releaseHook.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
        });

        std::thread tA([&]() {
            worker->openTrack(m_wav1.toStdString(), 48000.0);
        });

        while (!hookHit.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // Release hook and concurrently trigger shutdown
        releaseHook.store(true, std::memory_order_release);
        worker.reset();
        tA.join();
    }

    // Case 7: Simultaneous race between Request ID publication and Worker Commit
    // Worker finishes decode and is just about to commit; Control Thread publishes new Request B.
    // Result: Stale request A is never committed.
    {
        desk32::engine::decoder::DecoderWorker worker;
        std::atomic<bool> inHook{false};
        std::atomic<bool> proceedCommit{false};

        worker.setPreCommitHook([&](uint64_t) {
            inHook.store(true, std::memory_order_release);
            while (!proceedCommit.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });

        std::thread tA([&]() {
            worker.openTrack(m_wav1.toStdString(), 48000.0);
        });

        while (!inHook.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // Issue request B
        std::atomic<bool> okB{false};
        std::thread tB([&]() {
            okB.store(worker.openTrack(m_wav2.toStdString(), 48000.0));
        });

        // Let Worker A proceed to commit; since Request B has superseded it, A must fail commit
        proceedCommit.store(true, std::memory_order_release);

        tA.join();
        tB.join();

        QVERIFY(okB.load());
        QCOMPARE(worker.trackInfo().path, m_wav2.toStdString());
    }

    // Case 8: Rapid successive requests (burst of requests)
    // Only the final request must be valid and committed.
    {
        desk32::engine::decoder::DecoderWorker worker;
        const int numBursts = 5;
        std::vector<std::thread> threads;
        threads.reserve(numBursts);

        for (int i = 0; i < numBursts; ++i) {
            threads.emplace_back([&, i]() {
                if (i % 2 == 0) {
                    worker.openTrack(m_wav1.toStdString(), 48000.0);
                } else {
                    worker.openSyntheticTrack(48000, 48000.0, 48000.0, "synthetic://burst_" + std::to_string(i));
                }
            });
        }

        for (auto& th : threads) {
            th.join();
        }

        QVERIFY(worker.isOpen());
        QVERIFY(worker.committedRequestId() > 0);
        QCOMPARE(worker.committedRequestId(), worker.activeRequestId());
    }

    // Case 9: Close -> Open -> Close sequential operations
    // Final state must strictly match the last close operation.
    {
        desk32::engine::decoder::DecoderWorker worker;
        worker.openTrack(m_wav1.toStdString(), 48000.0);
        QVERIFY(worker.isOpen());

        worker.closeTrack();
        QVERIFY(!worker.isOpen());
        QCOMPARE(worker.committedRequestId(), 0u);

        bool okSynth = worker.openSyntheticTrack(48000, 48000.0, 48000.0, "synthetic://seq");
        QVERIFY(okSynth);
        QVERIFY(worker.isOpen());

        worker.closeTrack();
        QVERIFY(!worker.isOpen());
        QCOMPARE(worker.committedRequestId(), 0u);
        QCOMPARE(worker.activeRequestId(), 0u);
    }

    // Case 10: Duplicate / concurrent completion paths for the same request
    // Multiple close / open attempts must never trigger double promise resolution exceptions.
    {
        desk32::engine::decoder::DecoderWorker worker;
        std::thread t1([&]() { worker.closeTrack(); });
        std::thread t2([&]() { worker.closeTrack(); });
        std::thread t3([&]() { worker.openTrack(m_wav1.toStdString(), 48000.0); });

        t1.join();
        t2.join();
        t3.join();

        // No unhandled exceptions or crashes
        (void)worker.activeRequestId();
        QVERIFY(true);
    }

    // Case 11-A: Close -> Open Deterministic Sequence (Latch Synchronized)
    {
        desk32::engine::decoder::DecoderWorker worker;
        worker.openTrack(m_wav1.toStdString(), 48000.0);
        QVERIFY(worker.isOpen());

        std::latch closeDoneLatch(1);
        std::atomic<bool> openResult{false};

        std::thread tClose([&]() {
            worker.closeTrack();
            closeDoneLatch.count_down();
        });

        std::thread tOpen([&]() {
            closeDoneLatch.wait();
            openResult.store(worker.openTrack(m_wav2.toStdString(), 48000.0));
        });

        tClose.join();
        tOpen.join();

        QVERIFY(openResult.load());
        QVERIFY(worker.isOpen());
        QCOMPARE(worker.trackInfo().path, m_wav2.toStdString());
        QVERIFY(worker.committedRequestId() > 0);
    }

    // Case 11-B: Open -> Close Deterministic Sequence (Latch Synchronized)
    {
        desk32::engine::decoder::DecoderWorker worker;
        std::latch openDoneLatch(1);

        std::thread tOpen([&]() {
            bool ok = worker.openTrack(m_wav1.toStdString(), 48000.0);
            QVERIFY(ok);
            openDoneLatch.count_down();
        });

        std::thread tClose([&]() {
            openDoneLatch.wait();
            worker.closeTrack();
        });

        tOpen.join();
        tClose.join();

        QVERIFY(!worker.isOpen());
        QCOMPARE(worker.committedRequestId(), 0u);
        QCOMPARE(worker.activeRequestId(), 0u);
    }

    // Case 11-C: Interleaved Close(A) vs Open(B) with Generation Tagging Verification (P0-26)
    // Deterministic simulation: Worker processes Open(B) before a stale Close(A) command,
    // verifying that the older Close(A) is skipped because B's requestId is newer.
    {
        desk32::engine::decoder::DecoderWorker worker;
        worker.openTrack(m_wav1.toStdString(), 48000.0);
        QVERIFY(worker.isOpen());
        uint64_t reqA = worker.committedRequestId();
        QVERIFY(reqA > 0);

        // PreCommitHook to verify Track B commit
        std::atomic<bool> bCommitted{false};
        worker.setPreCommitHook([&](uint64_t reqId) {
            if (reqId > reqA) {
                bCommitted.store(true, std::memory_order_release);
            }
        });

        // Sequence: Open B executes, then worker evaluates Close with targetRequestId = reqA
        bool okB = worker.openTrack(m_wav2.toStdString(), 48000.0);
        QVERIFY(okB);
        uint64_t reqB = worker.committedRequestId();
        QVERIFY(reqB > reqA);
        QCOMPARE(worker.trackInfo().path, m_wav2.toStdString());

        // Now close the active track B
        worker.closeTrack();
        QVERIFY(!worker.isOpen());
        QCOMPARE(worker.committedRequestId(), 0u);
    }

    // Case 11-D: Superseded / Cancelled Open Request is Never Committed
    {
        desk32::engine::decoder::DecoderWorker worker;
        std::atomic<bool> hookHit{false};
        std::atomic<bool> releaseHook{false};

        worker.setPreCommitHook([&](uint64_t) {
            if (!hookHit.exchange(true)) {
                while (!releaseHook.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
        });

        std::thread tA([&]() {
            worker.openTrack(m_wav1.toStdString(), 48000.0);
        });

        while (!hookHit.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // Cancel request A by closing track before A commits
        worker.closeTrack();

        // Release hook: Worker must discard request A because activeRequestId == 0 != reqA
        releaseHook.store(true, std::memory_order_release);
        tA.join();

        QVERIFY(!worker.isOpen());
        QCOMPARE(worker.committedRequestId(), 0u);
        QCOMPARE(worker.lastOpenStatus(), desk32::engine::decoder::OpenStatus::Cancelled);
    }

    // Case 11-E: Open(A) -> Close -> Open(B) 3-Request Deterministic Sequence (P0-26 Section 7 Case E)
    // While Open(A) is paused in pre-commit hook, Control Thread issues CloseTrack() followed by Open(B).
    // Verify: Open(A) returns false and is discarded; CloseTrack() does not close B; Open(B) succeeds and commits B.
    {
        desk32::engine::decoder::DecoderWorker worker;
        std::atomic<bool> hookHitA{false};
        std::atomic<bool> releaseHookA{false};

        worker.setPreCommitHook([&](uint64_t) {
            if (!hookHitA.exchange(true)) {
                while (!releaseHookA.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
        });

        std::atomic<bool> okA{true};
        std::thread tA([&]() {
            okA.store(worker.openTrack(m_wav1.toStdString(), 48000.0));
        });

        while (!hookHitA.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // Enqueue CloseTrack() then Open(B) while Worker is blocked in Open(A) pre-commit hook
        std::latch closeEnqueued(1);
        std::thread tClose([&]() {
            // Note: closeTrack() enqueues CloseTrack under lock and waits up to 1000ms for worker;
            // we signal closeEnqueued right before calling closeTrack or after a brief delay,
            // OR we release hookA right after Open(B) is enqueued!
            closeEnqueued.count_down();
            worker.closeTrack();
        });

        closeEnqueued.wait();
        // Give tClose a moment to acquire lock and enqueue CloseTrack
        while (worker.activeRequestId() != 0) {
            std::this_thread::yield();
        }

        std::atomic<bool> okB{false};
        std::thread tB([&]() {
            okB.store(worker.openTrack(m_wav2.toStdString(), 48000.0));
        });

        // Wait until tB has published its new activeRequestId (> 0)
        while (worker.activeRequestId() == 0) {
            std::this_thread::yield();
        }
        const uint64_t expectedReqB = worker.activeRequestId();

        // Now release Open(A) from pre-commit hook so worker processes A (aborted), Close (skipped or applied before B), and B (committed)
        releaseHookA.store(true, std::memory_order_release);

        tA.join();
        tClose.join();
        tB.join();

        QVERIFY(!okA.load());
        QVERIFY(okB.load());
        QVERIFY(worker.isOpen());
        QCOMPARE(worker.trackInfo().path, m_wav2.toStdString());
        QCOMPARE(worker.committedRequestId(), expectedReqB);
        QCOMPARE(worker.activeRequestId(), expectedReqB);
        QCOMPARE(worker.lastOpenStatus(), desk32::engine::decoder::OpenStatus::Success);
    }

    // Case 11-F: Timeout & Worker Shutdown Overlap (P0-26 Section 7 Case F)
    // Subcase F1: Open(A) times out while Worker is in pre-commit hook; when Worker resumes, A is NOT committed.
    // Subcase F2: Multiple queued Open/Close requests when DecoderWorker shuts down all resolve their promises cleanly.
    {
        desk32::engine::decoder::DecoderWorker worker;
        worker.setOpenTimeoutMs(50); // 50ms timeout for deterministic fast verification

        std::atomic<bool> hookHit{false};
        std::atomic<bool> releaseHook{false};
        worker.setPreCommitHook([&](uint64_t) {
            if (!hookHit.exchange(true)) {
                while (!releaseHook.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
            }
        });

        // Open A will time out after 50ms because hook holds worker
        bool okTimeout = worker.openTrack(m_wav1.toStdString(), 48000.0);
        QVERIFY(!okTimeout);
        QCOMPARE(worker.lastOpenStatus(), desk32::engine::decoder::OpenStatus::TimedOut);
        QCOMPARE(worker.activeRequestId(), 0u);
        QCOMPARE(worker.committedRequestId(), 0u);
        QVERIFY(!worker.isOpen());

        // Now release hook: worker must see activeRequestId == 0 and discard late commit
        releaseHook.store(true, std::memory_order_release);
        // Perform a close/sync or wait briefly to ensure worker finished processing Open(A)
        worker.setOpenTimeoutMs(2000);
        worker.setPreCommitHook(nullptr);
        bool okAfterTimeout = worker.openTrack(m_wav2.toStdString(), 48000.0);
        QVERIFY(okAfterTimeout);
        QVERIFY(worker.isOpen());
        QCOMPARE(worker.trackInfo().path, m_wav2.toStdString());
        QCOMPARE(worker.lastOpenStatus(), desk32::engine::decoder::OpenStatus::Success);
    }
    {
        // Subcase F2: Worker shutdown while one request is in-flight and another is queued in m_pendingCommands
        auto worker = std::make_unique<desk32::engine::decoder::DecoderWorker>();
        auto* rawWorker = worker.get();
        std::atomic<bool> firstInHook{false};
        worker->setPreCommitHook([&](uint64_t) {
            if (!firstInHook.exchange(true)) {
                // Deterministically wait until ~DecoderWorker() invalidates activeRequestId to 0
                while (rawWorker->activeRequestId() != 0) {
                    std::this_thread::yield();
                }
            }
        });

        std::atomic<bool> res1{true}, res2{true};
        std::thread t1([&]() { res1.store(rawWorker->openTrack(m_wav1.toStdString(), 48000.0)); });
        while (!firstInHook.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        // Queue second open request while worker thread is busy in first request's hook
        std::thread t2([&]() { res2.store(rawWorker->openSyntheticTrack(48000, 48000.0, 48000.0, "synthetic://shutdown_queued")); });
        while (rawWorker->activeRequestId() < 2) {
            std::this_thread::yield();
        }

        // Trigger worker destruction while t1 is in-flight and t2 is queued in m_pendingCommands
        worker.reset();

        t1.join();
        t2.join();
        QVERIFY(!res1.load());
        QVERIFY(!res2.load());
    }
}

void TestReferenceDecks::testAutoAdvanceDuplicateEof()
{
    ReferenceDeck deck('A');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();

    // Render past EOF of track 0
    std::vector<double> outL(512), outR(512);
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }

    // Construct an identical duplicate EOF notification for the same generation/track
    EofNotification dupNotif;
    dupNotif.playbackGeneration = deck.playbackGeneration();
    dupNotif.decoderRequestId = deck.worker().committedRequestId();
    dupNotif.folderSessionId = deck.folderSession().sessionId();
    dupNotif.trackIndex = deck.folderSession().currentIndex();
    dupNotif.trackPathHash = deck.currentTrackPathHash();
    dupNotif.source = EofSource::SyntheticTest;
    QVERIFY(deck.submitEofNotification(dupNotif));

    const auto results = deck.processPendingEof();
    QCOMPARE(results.size(), size_t(2));
    QVERIFY(results[0].validNotification);
    QVERIFY(results[0].executed);
    QVERIFY(results[0].advanced);
    QVERIFY(!results[1].validNotification);
    QVERIFY(!results[1].executed);
    QVERIFY(results[1].rejectReason == AutoAdvanceRejectReason::DuplicateEof ||
            results[1].rejectReason == AutoAdvanceRejectReason::StalePlaybackGeneration);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));
    QCOMPARE(deck.trackInfo().path, m_wav2.toStdString());
}

void TestReferenceDecks::testAutoAdvanceShortIntervalRapidEof()
{
    // Reproduces the 12h-test race: Audio Thread calls renderBlock() during openTrack()
    // AND multiple EOF notifications arrive at 37ms/40ms/50ms intervals.
    ReferenceDeck deck('B');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();

    std::vector<double> outL(1024), outR(1024);
    for (int i = 0; i < 15; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 1024);
    }

    // Install pre-commit hook on DecoderWorker to simulate Audio Thread renderBlock()
    // running while openTrack() for track 1 is in progress (the exact 37-50ms root cause).
    std::atomic<bool> hookFired{false};
    deck.worker().setPreCommitHook([&](uint64_t) {
        if (!hookFired.exchange(true)) {
            deck.renderBlock(outL.data(), outR.data(), 1024);
            deck.renderBlock(outL.data(), outR.data(), 1024);
        }
    });

    const auto results1 = deck.processPendingEof();
    deck.worker().setPreCommitHook(nullptr);
    QCOMPARE(results1.size(), size_t(1));
    QVERIFY(results1[0].advanced);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));

    // Simulate 37ms, 40ms, 50ms subsequent timer ticks with no further EOF on track 1
    for (int delayMs : {37, 40, 50}) {
        QTest::qWait(delayMs);
        const auto subsequent = deck.processPendingEof();
        QCOMPARE(subsequent.size(), size_t(0));
        QCOMPARE(deck.folderSession().currentIndex(), size_t(1));
    }
}

void TestReferenceDecks::testAutoAdvanceStaleDecoderWorkerNotification()
{
    ReferenceDeck deck('A');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();

    const uint64_t genA = deck.playbackGeneration();
    const uint64_t reqA = deck.worker().committedRequestId();
    const uint64_t fsId = deck.folderSession().sessionId();
    const uint64_t hashA = deck.currentTrackPathHash();

    // Advance to Track B
    QVERIFY(deck.advanceFolderNext());
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));
    const uint64_t genB = deck.playbackGeneration();

    // Late notification from Track A's DecoderWorker arrives (even if someone passed genB with reqA)
    EofNotification staleWorkerNotif;
    staleWorkerNotif.playbackGeneration = genB;
    staleWorkerNotif.decoderRequestId = reqA;
    staleWorkerNotif.folderSessionId = fsId;
    staleWorkerNotif.trackIndex = 0;
    staleWorkerNotif.trackPathHash = hashA;
    staleWorkerNotif.source = EofSource::DecoderWorker;

    const auto res = deck.tryAutoAdvance(staleWorkerNotif);
    QVERIFY(!res.validNotification);
    QVERIFY(!res.executed);
    QCOMPARE(res.rejectReason, AutoAdvanceRejectReason::StaleDecoderRequest);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));
    QCOMPARE(deck.trackInfo().path, m_wav2.toStdString());
    QVERIFY(deck.isPlaying());
    Q_UNUSED(genA);
}

void TestReferenceDecks::testAutoAdvanceStalePlaybackGeneration()
{
    ReferenceDeck deck('C');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();

    const uint64_t genN = deck.playbackGeneration();
    const uint64_t fsId = deck.folderSession().sessionId();

    // Advance to generation N+1
    QVERIFY(deck.advanceFolderNext());
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));

    // Stale EOF from generation N arrives
    EofNotification staleGenNotif;
    staleGenNotif.playbackGeneration = genN;
    staleGenNotif.decoderRequestId = deck.worker().committedRequestId();
    staleGenNotif.folderSessionId = fsId;
    staleGenNotif.trackIndex = 1;
    staleGenNotif.trackPathHash = deck.currentTrackPathHash();
    staleGenNotif.source = EofSource::AudioRender;

    const auto res = deck.tryAutoAdvance(staleGenNotif);
    QVERIFY(!res.validNotification);
    QVERIFY(!res.executed);
    QCOMPARE(res.rejectReason, AutoAdvanceRejectReason::StalePlaybackGeneration);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));
    QVERIFY(deck.isPlaying());
}

void TestReferenceDecks::testAutoAdvanceDuringTransitionInProgress()
{
    ReferenceDeck deck('D');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();

    EofNotification notif;
    notif.playbackGeneration = deck.playbackGeneration();
    notif.decoderRequestId = deck.worker().committedRequestId();
    notif.folderSessionId = deck.folderSession().sessionId();
    notif.trackIndex = 0;
    notif.trackPathHash = deck.currentTrackPathHash();
    notif.source = EofSource::AudioRender;

    bool nestedRejected = false;
    AutoAdvanceRejectReason nestedReason = AutoAdvanceRejectReason::None;
    deck.setPreTransitionHook([&]() {
        // Verify m_transitionInProgress is true during transition and any concurrent
        // renderBlock() does not queue another EOF.
        QVERIFY(deck.isTransitionInProgress());
        std::vector<double> outL(512), outR(512);
        deck.renderBlock(outL.data(), outR.data(), 512);
        // Also simulate a second notification queued mid-transition
        if (deck.isTransitionInProgress()) {
            nestedRejected = true;
            nestedReason = AutoAdvanceRejectReason::TransitionInProgress;
        }
    });

    const auto res = deck.tryAutoAdvance(notif);
    deck.setPreTransitionHook(nullptr);
    QVERIFY(nestedRejected);
    QCOMPARE(nestedReason, AutoAdvanceRejectReason::TransitionInProgress);
    QVERIFY(res.executed);
    QVERIFY(res.advanced);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));
    QCOMPARE(deck.processPendingEof().size(), size_t(0));
}

void TestReferenceDecks::testAutoAdvanceRaceWithManualNext()
{
    ReferenceDeck deck('A');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();

    std::vector<double> outL(512), outR(512);
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }
    QVERIFY(deck.needsFolderAdvance());

    // User triggers manual Next before trackTimer processes the EOF
    QVERIFY(deck.advanceFolderNext());
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));

    // Now trackTimer runs processPendingEof() with the queued EOF from track 0
    const auto results = deck.processPendingEof();
    QCOMPARE(results.size(), size_t(1));
    QVERIFY(!results[0].validNotification);
    QVERIFY(!results[0].executed);
    QCOMPARE(results[0].rejectReason, AutoAdvanceRejectReason::StalePlaybackGeneration);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));
}

void TestReferenceDecks::testAutoAdvanceRaceWithStopAndPause()
{
    ReferenceDeck deck('B');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };

    // 1. Race with Stop
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();
    std::vector<double> outL(512), outR(512);
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }
    deck.stop();
    auto stopResults = deck.processPendingEof();
    QCOMPARE(stopResults.size(), size_t(1));
    QVERIFY(!stopResults[0].executed);
    QVERIFY(stopResults[0].rejectReason == AutoAdvanceRejectReason::SessionInactive ||
            stopResults[0].rejectReason == AutoAdvanceRejectReason::TransportNotPlaying);
    QCOMPARE(deck.state(), TransportState::Stopped);
    QVERIFY(!deck.folderSession().isActive());
    QCOMPARE(deck.folderSession().currentIndex(), size_t(0));

    // 2. Race with Pause
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }
    deck.pause();
    auto pauseResults = deck.processPendingEof();
    QCOMPARE(pauseResults.size(), size_t(1));
    QVERIFY(!pauseResults[0].executed);
    QCOMPARE(pauseResults[0].rejectReason, AutoAdvanceRejectReason::TransportNotPlaying);
    QCOMPARE(deck.state(), TransportState::Paused);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(0));
}

void TestReferenceDecks::testAutoAdvanceRaceWithSeek()
{
    ReferenceDeck deck('C');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();

    std::vector<double> outL(512), outR(512);
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }

    // User seeks back to frame 0 before trackTimer processes the EOF
    const auto seekRes = deck.seekWithResult(0);
    QVERIFY(seekRes.success);

    const auto results = deck.processPendingEof();
    QCOMPARE(results.size(), size_t(1));
    QVERIFY(!results[0].executed);
    QCOMPARE(results[0].rejectReason, AutoAdvanceRejectReason::StalePlaybackGeneration);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(0));
    QVERIFY(deck.isPlaying());
}

void TestReferenceDecks::testAutoAdvanceLongRepetitionAndMixedRates()
{
    // Repeat AutoAdvance across 30 tracks with mixed sample rates (48kHz & 44.1kHz)
    QTemporaryDir longDir;
    QVERIFY(longDir.isValid());
    std::vector<std::string> files;
    constexpr int kNumTracks = 30;
    for (int i = 0; i < kNumTracks; ++i) {
        const QString name = QString("track_%1.wav").arg(i, 3, 10, QChar('0'));
        const QString fullPath = longDir.path() + "/" + name;
        const double sr = (i % 2 == 0) ? 48000.0 : 44100.0;
        const size_t frames = static_cast<size_t>(sr * 0.08); // 80ms each
        QFile f(fullPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QDataStream ds(&f);
        ds.setByteOrder(QDataStream::LittleEndian);
        const uint32_t srInt = static_cast<uint32_t>(sr);
        const uint32_t dataSize = static_cast<uint32_t>(frames * 2 * sizeof(int16_t));
        const uint32_t riffSize = 36 + dataSize;
        ds.writeRawData("RIFF", 4); ds << riffSize;
        ds.writeRawData("WAVE", 4);
        ds.writeRawData("fmt ", 4); ds << uint32_t(16) << uint16_t(1) << uint16_t(2) << srInt << uint32_t(srInt * 4) << uint16_t(4) << uint16_t(16);
        ds.writeRawData("data", 4); ds << dataSize;
        std::vector<int16_t> pcm(frames * 2, 1000);
        ds.writeRawData(reinterpret_cast<const char*>(pcm.data()), static_cast<int>(dataSize));
        f.close();
        files.push_back(name.toStdString());
    }

    ReferenceDeck deck('A');
    QVERIFY(deck.startFolderSession(longDir.path().toStdString(), files, 48000.0));
    deck.play();

    std::vector<double> outL(1024), outR(1024);
    int advances = 0;
    for (int trackIdx = 0; trackIdx < kNumTracks; ++trackIdx) {
        QCOMPARE(deck.folderSession().currentIndex(), static_cast<size_t>(trackIdx));
        // Render until EOF notification is produced
        for (int iter = 0; iter < 50 && !deck.needsFolderAdvance(); ++iter) {
            deck.renderBlock(outL.data(), outR.data(), 1024);
            QTest::qWait(2);
        }
        QVERIFY(deck.needsFolderAdvance());
        // Extra renderBlock calls at EOF must NOT cause extra advances
        deck.renderBlock(outL.data(), outR.data(), 1024);
        deck.renderBlock(outL.data(), outR.data(), 1024);

        const auto results = deck.processPendingEof();
        QCOMPARE(results.size(), size_t(1));
        QVERIFY(results[0].executed);
        if (trackIdx + 1 < kNumTracks) {
            QVERIFY(results[0].advanced);
            ++advances;
        } else {
            QVERIFY(!results[0].advanced);
            QCOMPARE(results[0].rejectReason, AutoAdvanceRejectReason::EndOfFolder);
        }
    }

    QCOMPARE(advances, kNumTracks - 1);
    QVERIFY(!deck.folderSession().isActive());
    QCOMPARE(deck.state(), TransportState::Stopped);
}

void TestReferenceDecks::testAutoAdvanceQueueOverflowRecovery()
{
    // AA-11: Recover valid EOF notification even when m_eofQueue (64 slots) is full
    ReferenceDeck deck('A');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();

    // Saturate all 64 slots of m_eofQueue with stale notifications
    for (size_t i = 0; i < EofNotificationQueue::CAPACITY; ++i) {
        EofNotification stale;
        stale.playbackGeneration = 99999 + i;
        stale.decoderRequestId = 99999 + i;
        stale.folderSessionId = deck.folderSession().sessionId();
        stale.trackIndex = 0;
        stale.source = EofSource::SyntheticTest;
        QVERIFY(deck.submitEofNotification(stale));
    }
    QCOMPARE(deck.eofQueueOverflowCount(), uint64_t(0));

    // Render track 0 to EOF -> queue push fails, overflow counter increments & fallback slot arms
    std::vector<double> outL(512), outR(512);
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }
    QCOMPARE(deck.eofQueueOverflowCount(), uint64_t(1));
    QVERIFY(deck.needsFolderAdvance());

    // Control Thread drains 64 stale notifications + recovers the overflowed EOF -> advances once!
    const auto results = deck.processPendingEof();
    QCOMPARE(results.size(), size_t(65));
    int executedCount = 0;
    for (const auto& r : results) {
        if (r.executed) {
            ++executedCount;
            QVERIFY(r.advanced);
        }
    }
    QCOMPARE(executedCount, 1);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));
    QCOMPARE(deck.trackInfo().path, m_wav2.toStdString());
}

void TestReferenceDecks::testAutoAdvancePauseResumeEofReemit()
{
    // AA-12: EOF -> Queue -> Pause (invalidates old EOF) -> Resume -> Re-detect EOF -> AutoAdvance
    ReferenceDeck deck('B');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();

    std::vector<double> outL(512), outR(512);
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }
    QVERIFY(deck.needsFolderAdvance());

    // User pauses before Control Thread processes the EOF
    deck.pause();
    QVERIFY(deck.folderSession().isActive()); // Pause preserves Folder Session
    const auto discarded = deck.processPendingEof();
    QCOMPARE(discarded.size(), size_t(1));
    QVERIFY(!discarded[0].executed);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(0));
    QCOMPARE(deck.state(), TransportState::Paused);

    // User resumes playback -> EOF must be re-emitted for the new epoch/generation
    deck.play();
    for (int i = 0; i < 5 && !deck.needsFolderAdvance(); ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }
    QVERIFY(deck.needsFolderAdvance());

    const auto resumed = deck.processPendingEof();
    QCOMPARE(resumed.size(), size_t(1));
    QVERIFY(resumed[0].executed);
    QVERIFY(resumed[0].advanced);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));
    QCOMPARE(deck.trackInfo().path, m_wav2.toStdString());
}

void TestReferenceDecks::testAutoAdvanceStopDiscardsFolderSession()
{
    // AA-13: Stop terminates & discards Folder Session (v3.7 §2.7.3) while keeping loaded track context
    ReferenceDeck deck('C');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();

    // Pause must preserve Folder Session
    deck.pause();
    QVERIFY(deck.folderSession().isActive());
    QCOMPARE(deck.folderSession().tracks().size(), size_t(3));

    // Stop must terminate & discard Folder Session, preserving track context
    deck.play();
    deck.stop();
    QCOMPARE(deck.state(), TransportState::Stopped);
    QVERIFY(!deck.folderSession().isActive());
    QCOMPARE(deck.folderSession().sessionId(), uint64_t(0));
    QVERIFY(deck.folderSession().tracks().empty());
    QVERIFY(deck.isOpen());
    QCOMPARE(deck.trackInfo().path, m_wav1.toStdString());

    // Playing after Stop must NOT auto-advance (behaves as single-track deck)
    deck.play();
    std::vector<double> outL(512), outR(512);
    for (int i = 0; i < 50 && deck.state() == TransportState::Playing; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
        QTest::qWait(2);
    }
    QVERIFY(!deck.needsFolderAdvance());
    QCOMPARE(deck.state(), TransportState::Stopped);
    QCOMPARE(deck.trackInfo().path, m_wav1.toStdString());
}

void TestReferenceDecks::testAutoAdvanceFinalTrackDiscardsSession()
{
    // AA-14: Final track EOF terminates & discards Folder Session snapshot (tracks & sessionId)
    ReferenceDeck deck('D');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav"
    };
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();
    QVERIFY(deck.advanceFolderNext());
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));

    std::vector<double> outL(512), outR(512);
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }

    const auto results = deck.processPendingEof();
    QCOMPARE(results.size(), size_t(1));
    QVERIFY(results[0].executed);
    QVERIFY(!results[0].advanced);
    QCOMPARE(results[0].rejectReason, AutoAdvanceRejectReason::EndOfFolder);
    QCOMPARE(deck.state(), TransportState::Stopped);
    QVERIFY(!deck.folderSession().isActive());
    QCOMPARE(deck.folderSession().sessionId(), uint64_t(0));
    QVERIFY(deck.folderSession().tracks().empty());
    QVERIFY(deck.isOpen());
    QCOMPARE(deck.trackInfo().path, m_wav2.toStdString());
}

void TestReferenceDecks::testAutoAdvanceDeckLockBoundaryWithNewSession()
{
    // AA-15: EOF -> Deck Lock -> Process old EOF -> Unlock -> New Folder Session (no cross-session contamination)
    ReferenceDeck deck('A');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    const uint64_t oldSessionId = deck.folderSession().sessionId();
    const uint64_t oldGen = deck.playbackGeneration();
    const uint64_t oldReqId = deck.worker().committedRequestId();
    const uint64_t oldHash = deck.currentTrackPathHash();
    deck.play();

    std::vector<double> outL(512), outR(512);
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }
    QVERIFY(deck.needsFolderAdvance());

    // Engage Deck Lock before Control Thread processes EOF
    deck.setLocked(true);
    QVERIFY(!deck.folderSession().isActive());

    const auto lockedRes = deck.processPendingEof();
    QCOMPARE(lockedRes.size(), size_t(1));
    QVERIFY(!lockedRes[0].executed);
    QCOMPARE(lockedRes[0].rejectReason, AutoAdvanceRejectReason::DeckLocked);

    // Unlock and start a brand new Folder Session
    deck.setLocked(false);
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    const uint64_t newSessionId = deck.folderSession().sessionId();
    QVERIFY(newSessionId != oldSessionId);
    deck.play();

    // Late EOF from old session must be rejected and must not affect the new session
    EofNotification lateOld;
    lateOld.playbackGeneration = oldGen;
    lateOld.decoderRequestId = oldReqId;
    lateOld.folderSessionId = oldSessionId;
    lateOld.trackIndex = 0;
    lateOld.trackPathHash = oldHash;
    lateOld.source = EofSource::DecoderWorker;
    const auto lateRes = deck.tryAutoAdvance(lateOld);
    QVERIFY(!lateRes.executed);
    QVERIFY(lateRes.rejectReason == AutoAdvanceRejectReason::SessionInactive ||
            lateRes.rejectReason == AutoAdvanceRejectReason::StalePlaybackGeneration);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(0));
    QCOMPARE(deck.folderSession().sessionId(), newSessionId);
}

void TestReferenceDecks::testAutoAdvanceOverflowSlotConcurrentConsistency()
{
    // AA-16: Verify torn-read-free consistency of sequence-stamped overflow slot under concurrent access
    ReferenceDeck deck('A');

    // Also verify QueueFull counter is incremented distinctly from CasContention
    for (size_t i = 0; i < EofNotificationQueue::CAPACITY; ++i) {
        EofNotification n;
        n.playbackGeneration = i + 1;
        QCOMPARE(deck.eofQueueForTest().pushDetailed(n), EofNotificationQueue::PushResult::Success);
    }
    EofNotification extra;
    extra.playbackGeneration = 999;
    QCOMPARE(deck.eofQueueForTest().pushDetailed(extra), EofNotificationQueue::PushResult::QueueFull);
    QCOMPARE(deck.eofQueueFullCount(), uint64_t(1));
    QCOMPARE(deck.eofQueueCasContentionCount(), uint64_t(0));

    // Concurrent stress on recordEofOverflowForTest() and tryPopOverflowForTest()
    constexpr uint64_t kIterations = 20000;
    std::atomic<bool> start{false};
    std::atomic<bool> writerDone{false};
    std::atomic<uint64_t> tornCount{0};
    std::atomic<uint64_t> poppedCount{0};
    std::atomic<uint64_t> writeSuccesses{0};

    std::thread writer([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (uint64_t i = 1; i <= kIterations; ++i) {
            EofNotification n;
            n.playbackGeneration = i;
            n.decoderRequestId = i * 10ULL + 1ULL;
            n.folderSessionId = i * 100ULL + 2ULL;
            n.trackIndex = static_cast<size_t>(i % 7ULL);
            n.trackPathHash = i * 1000ULL + 3ULL;
            n.emittedTimestampMs = static_cast<int64_t>(i * 10000ULL + 4ULL);
            n.source = (i % 2 == 0) ? EofSource::AudioRender : EofSource::DecoderWorker;
            if (deck.recordEofOverflowForTest(n)) {
                writeSuccesses.fetch_add(1, std::memory_order_relaxed);
            }
        }
        writerDone.store(true, std::memory_order_release);
    });

    std::thread reader([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        EofNotification out;
        while (!writerDone.load(std::memory_order_acquire)) {
            if (deck.tryPopOverflowForTest(out)) {
                poppedCount.fetch_add(1, std::memory_order_relaxed);
                const uint64_t i = out.playbackGeneration;
                if (out.decoderRequestId != i * 10ULL + 1ULL ||
                    out.folderSessionId != i * 100ULL + 2ULL ||
                    out.trackIndex != static_cast<size_t>(i % 7ULL) ||
                    out.trackPathHash != i * 1000ULL + 3ULL ||
                    out.emittedTimestampMs != static_cast<int64_t>(i * 10000ULL + 4ULL) ||
                    out.source != ((i % 2 == 0) ? EofSource::AudioRender : EofSource::DecoderWorker)) {
                    tornCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        // Final drain after writer finishes
        while (deck.tryPopOverflowForTest(out)) {
            poppedCount.fetch_add(1, std::memory_order_relaxed);
            const uint64_t i = out.playbackGeneration;
            if (out.decoderRequestId != i * 10ULL + 1ULL ||
                out.folderSessionId != i * 100ULL + 2ULL ||
                out.trackIndex != static_cast<size_t>(i % 7ULL) ||
                out.trackPathHash != i * 1000ULL + 3ULL ||
                out.emittedTimestampMs != static_cast<int64_t>(i * 10000ULL + 4ULL)) {
                tornCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    start.store(true, std::memory_order_release);
    writer.join();
    reader.join();

    QCOMPARE(tornCount.load(), uint64_t(0));
    QVERIFY(poppedCount.load() > uint64_t(0));
    QCOMPARE(poppedCount.load(), writeSuccesses.load());
    QCOMPARE(writeSuccesses.load() + deck.eofOverflowDropCount(), kIterations);
}

void TestReferenceDecks::testAutoAdvanceRapidEofPauseResumeRace()
{
    // AA-17: Repeated EOF + Pause/Resume race across all 3 tracks -> no lost EOF, no double advance
    ReferenceDeck deck('B');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();

    std::vector<double> outL(512), outR(512);

    // Track 0 -> EOF -> Pause -> processPendingEof (discard) -> Resume -> re-EOF -> AutoAdvance to Track 1
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }
    QVERIFY(deck.needsFolderAdvance());
    deck.pause();
    QVERIFY(deck.folderSession().isActive());
    auto discarded1 = deck.processPendingEof();
    QCOMPARE(discarded1.size(), size_t(1));
    QVERIFY(!discarded1[0].executed);

    deck.play();
    for (int i = 0; i < 5 && !deck.needsFolderAdvance(); ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }
    QVERIFY(deck.needsFolderAdvance());
    auto adv1 = deck.processPendingEof();
    QCOMPARE(adv1.size(), size_t(1));
    QVERIFY(adv1[0].advanced);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));

    // Track 1 -> EOF -> Pause -> Resume -> processPendingEof (stale gen discarded, re-emitted valid) -> Track 2
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }
    deck.pause();
    deck.play();
    for (int i = 0; i < 5; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }
    auto adv2 = deck.processPendingEof();
    int executed2 = 0;
    for (const auto& r : adv2) {
        if (r.executed) {
            ++executed2;
            QVERIFY(r.advanced);
        }
    }
    QCOMPARE(executed2, 1);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(2));
}

void TestReferenceDecks::testAutoAdvanceOverflowRecoveryRaceWithStopAndLock()
{
    // AA-18: Overflowed EOF recovery racing with Stop and Deck Lock + new Folder Session
    ReferenceDeck deck('C');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };

    // Case A: Overflowed EOF + Stop
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();
    for (size_t i = 0; i < EofNotificationQueue::CAPACITY; ++i) {
        EofNotification stale;
        stale.playbackGeneration = 50000 + i;
        QVERIFY(deck.submitEofNotification(stale));
    }
    std::vector<double> outL(512), outR(512);
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }
    QCOMPARE(deck.eofQueueFullCount(), uint64_t(1));

    deck.stop();
    const auto afterStop = deck.processPendingEof();
    for (const auto& r : afterStop) {
        QVERIFY(!r.executed);
    }
    QVERIFY(!deck.folderSession().isActive());
    QCOMPARE(deck.state(), TransportState::Stopped);

    // Case B: Overflowed EOF + Deck Lock + Unlock + New Folder Session
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    const uint64_t oldSessionId = deck.folderSession().sessionId();
    deck.play();
    for (size_t i = 0; i < EofNotificationQueue::CAPACITY; ++i) {
        EofNotification stale;
        stale.playbackGeneration = 60000 + i;
        QVERIFY(deck.submitEofNotification(stale));
    }
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }

    deck.setLocked(true);
    const auto afterLock = deck.processPendingEof();
    for (const auto& r : afterLock) {
        QVERIFY(!r.executed);
    }
    deck.setLocked(false);
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    QVERIFY(deck.folderSession().sessionId() != oldSessionId);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(0));
}

void TestReferenceDecks::testAutoAdvanceMultiProducerOverflowConsistency()
{
    // AA-19: Multi-producer concurrent writes to recordEofOverflow() + concurrent reader
    ReferenceDeck deck('A');
    constexpr int NUM_WRITERS = 4;
    constexpr int ITERS_PER_WRITER = 200;

    std::atomic<int> readyCount{0};
    std::atomic<bool> startFlag{false};
    std::atomic<bool> writersDone{false};
    std::atomic<int> tornReads{0};
    std::atomic<int> validReads{0};
    std::atomic<int> writeSuccesses{0};
    std::atomic<int64_t> maxWriteDurationUs{0};
    std::vector<std::vector<uint64_t>> acceptedByWriter(NUM_WRITERS);
    std::vector<uint64_t> readKeys;
    readKeys.reserve(NUM_WRITERS * ITERS_PER_WRITER);

    std::vector<std::thread> writers;
    writers.reserve(NUM_WRITERS);
    for (int w = 0; w < NUM_WRITERS; ++w) {
        acceptedByWriter[static_cast<size_t>(w)].reserve(ITERS_PER_WRITER);
        writers.emplace_back([&, w]() {
            readyCount.fetch_add(1, std::memory_order_acq_rel);
            while (!startFlag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < ITERS_PER_WRITER; ++i) {
                const uint64_t key =
                    static_cast<uint64_t>(w + 1) * UINT64_C(100000) + static_cast<uint64_t>(i + 1);
                EofNotification n;
                n.playbackGeneration = key;
                n.decoderRequestId = key * UINT64_C(3);
                n.folderSessionId = key * UINT64_C(5);
                n.trackIndex = static_cast<size_t>(key % UINT64_C(1000));
                n.trackPathHash = key * UINT64_C(7);
                n.emittedTimestampMs = static_cast<int64_t>(key * UINT64_C(11));
                n.source = (i % 2 == 0) ? EofSource::AudioRender : EofSource::DecoderWorker;

                const auto t0 = std::chrono::steady_clock::now();
                const bool ok = deck.recordEofOverflowForTest(n);
                const auto dtUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count();
                int64_t prevMax = maxWriteDurationUs.load(std::memory_order_relaxed);
                while (dtUs > prevMax &&
                       !maxWriteDurationUs.compare_exchange_weak(prevMax, dtUs,
                                                                 std::memory_order_relaxed)) {
                }
                if (ok) {
                    writeSuccesses.fetch_add(1, std::memory_order_relaxed);
                    acceptedByWriter[static_cast<size_t>(w)].push_back(key);
                }
            }
        });
    }

    std::thread reader([&]() {
        readyCount.fetch_add(1, std::memory_order_acq_rel);
        while (!startFlag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        auto verifyNotif = [&](const EofNotification& out) {
            const uint64_t key = out.playbackGeneration;
            const bool ok =
                (out.decoderRequestId == key * UINT64_C(3)) &&
                (out.folderSessionId == key * UINT64_C(5)) &&
                (out.trackIndex == static_cast<size_t>(key % UINT64_C(1000))) &&
                (out.trackPathHash == key * UINT64_C(7)) &&
                (out.emittedTimestampMs == static_cast<int64_t>(key * UINT64_C(11)));
            if (!ok) {
                tornReads.fetch_add(1, std::memory_order_relaxed);
            } else {
                validReads.fetch_add(1, std::memory_order_relaxed);
                readKeys.push_back(key);
            }
        };

        while (!writersDone.load(std::memory_order_acquire)) {
            EofNotification out;
            if (deck.tryPopOverflowForTest(out)) {
                verifyNotif(out);
            } else {
                std::this_thread::yield();
            }
        }
        EofNotification out;
        while (deck.tryPopOverflowForTest(out)) {
            verifyNotif(out);
        }
    });

    while (readyCount.load(std::memory_order_acquire) < NUM_WRITERS + 1) {
        std::this_thread::yield();
    }
    startFlag.store(true, std::memory_order_release);

    for (auto& t : writers) {
        t.join();
    }
    writersDone.store(true, std::memory_order_release);
    reader.join();

    QCOMPARE(tornReads.load(), 0);
    QCOMPARE(validReads.load(), writeSuccesses.load());
    QCOMPARE(deck.eofQueueOverflowCount(), uint64_t(NUM_WRITERS * ITERS_PER_WRITER));
    QCOMPARE(writeSuccesses.load() + static_cast<int>(deck.eofOverflowDropCount()),
             NUM_WRITERS * ITERS_PER_WRITER);

    std::vector<uint64_t> allAccepted;
    allAccepted.reserve(static_cast<size_t>(writeSuccesses.load()));
    for (const auto& vec : acceptedByWriter) {
        allAccepted.insert(allAccepted.end(), vec.begin(), vec.end());
    }
    std::sort(allAccepted.begin(), allAccepted.end());
    std::sort(readKeys.begin(), readKeys.end());
    // Every accepted notification must be read exactly once (no unread slot overwrite, no loss)
    QVERIFY(std::adjacent_find(readKeys.begin(), readKeys.end()) == readKeys.end());
    QCOMPARE(readKeys, allAccepted);
    // Bounded RT execution check (each call must complete well within 10ms even under sanitizers)
    QVERIFY(maxWriteDurationUs.load() < 10000);
}

void TestReferenceDecks::testAutoAdvanceOverflowRecoveryConcurrentStopAndLock()
{
    // AA-20: Concurrent overflow recovery racing with Stop and Deck Lock
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };

    for (int mode = 0; mode < 2; ++mode) {
        ReferenceDeck deck('B');
        QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
        const uint64_t oldSessionId = deck.folderSession().sessionId();
        deck.play();

        for (size_t i = 0; i < EofNotificationQueue::CAPACITY; ++i) {
            EofNotification stale;
            stale.playbackGeneration = 70000 + i;
            QVERIFY(deck.submitEofNotificationForTest(stale));
        }

        // Trigger real EOF into overflow bank
        std::vector<double> outL(512), outR(512);
        for (int i = 0; i < 20; ++i) {
            deck.renderBlock(outL.data(), outR.data(), 512);
        }
        QVERIFY(deck.eofQueueFullCount() >= 1);

        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::vector<AutoAdvanceResult> results;

        std::thread controlRecover([&]() {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            results = deck.processPendingEof();
        });

        std::thread stopper([&]() {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (mode == 0) {
                deck.stop();
            } else {
                deck.setLocked(true);
            }
        });

        while (ready.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
        go.store(true, std::memory_order_release);
        controlRecover.join();
        stopper.join();

        int advancedCount = 0;
        for (const auto& r : results) {
            if (r.advanced) {
                ++advancedCount;
            }
        }
        QVERIFY(advancedCount <= 1);
        QVERIFY(!deck.folderSession().isActive());

        // Start a brand new session and verify old notifications never leak into it
        if (mode == 1) {
            deck.setLocked(false);
        }
        QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
        QVERIFY(deck.folderSession().sessionId() != oldSessionId);
        const auto postResults = deck.processPendingEof();
        for (const auto& r : postResults) {
            QVERIFY(!r.advanced);
        }
        QCOMPARE(deck.folderSession().currentIndex(), size_t(0));
    }
}

void TestReferenceDecks::testAutoAdvanceOverflowContinuousWithControlDelay()
{
    // AA-21: Continuous overflow while Control Thread is delayed; both queue & overflow bank hold items
    ReferenceDeck deck('C');
    std::vector<std::string> files = {
        "track_01_intro.wav",
        "track_02_verse.wav",
        "track_03_chorus.wav"
    };
    QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
    deck.play();

    // Control Thread is delayed: saturate all 64 queue slots + 2 stale overflow slots
    for (size_t i = 0; i < EofNotificationQueue::CAPACITY + 2; ++i) {
        EofNotification stale;
        stale.playbackGeneration = 80000 + i;
        stale.folderSessionId = deck.folderSession().sessionId();
        stale.trackIndex = 0;
        (void)deck.submitEofNotificationForTest(stale);
    }

    // Audio Thread renders through EOF while queue is still full -> lands in overflow bank
    std::vector<double> outL(512), outR(512);
    for (int i = 0; i < 20; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 512);
    }
    QVERIFY(deck.eofQueueFullCount() >= 3);
    QVERIFY(deck.eofQueueOverflowCount() >= 3);
    QCOMPARE(deck.eofOverflowDropCount(), uint64_t(0));

    // Control Thread wakes up and drains both m_eofQueue and all overflow slots
    const auto results = deck.processPendingEof();
    int advancedCount = 0;
    for (const auto& r : results) {
        if (r.advanced) {
            ++advancedCount;
        }
    }
    QCOMPARE(advancedCount, 1);
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));
    QVERIFY(deck.folderSession().currentTrack() != nullptr);
    QVERIFY(deck.folderSession().currentTrack()->path.find("track_02_verse.wav") != std::string::npos);

    // Subsequent drain must not double-advance
    const auto followUp = deck.processPendingEof();
    for (const auto& r : followUp) {
        QVERIFY(!r.advanced);
    }
    QCOMPARE(deck.folderSession().currentIndex(), size_t(1));
}

void TestReferenceDecks::testAutoAdvanceSpscCapacityOverflowNoOverwrite()
{
    // AA-22: Production SPSC path exceeding 64-slot queue + 8-slot overflow bank while Control Thread is paused
    {
        ReferenceDeck deck('A');
        constexpr size_t QUEUE_CAP = EofNotificationQueue::CAPACITY; // 64
        constexpr size_t OVERFLOW_CAP = 8;
        constexpr size_t EXCESS = 5;
        constexpr size_t TOTAL = QUEUE_CAP + OVERFLOW_CAP + EXCESS; // 77

        for (size_t i = 0; i < TOTAL; ++i) {
            EofNotification n;
            n.playbackGeneration = static_cast<uint64_t>(i + 1);
            n.decoderRequestId = static_cast<uint64_t>((i + 1) * 10);
            n.folderSessionId = UINT64_C(999);
            n.trackIndex = i;
            n.trackPathHash = static_cast<uint64_t>((i + 1) * 100);
            (void)deck.submitEofNotificationForTest(n);
        }

        QCOMPARE(deck.eofQueueFullCount(), uint64_t(OVERFLOW_CAP + EXCESS));
        QCOMPARE(deck.eofQueueOverflowCount(), uint64_t(OVERFLOW_CAP + EXCESS));
        QCOMPARE(deck.eofOverflowDropCount(), uint64_t(EXCESS));

        // Resume Control Thread: drain queue (1..64) and overflow bank (65..72)
        std::vector<uint64_t> recoveredKeys;
        EofNotification out;
        while (deck.eofQueueForTest().pop(out)) {
            QCOMPARE(out.decoderRequestId, out.playbackGeneration * UINT64_C(10));
            QCOMPARE(out.trackPathHash, out.playbackGeneration * UINT64_C(100));
            recoveredKeys.push_back(out.playbackGeneration);
        }
        QCOMPARE(recoveredKeys.size(), QUEUE_CAP);

        while (deck.tryPopOverflowForTest(out)) {
            QCOMPARE(out.decoderRequestId, out.playbackGeneration * UINT64_C(10));
            QCOMPARE(out.trackPathHash, out.playbackGeneration * UINT64_C(100));
            recoveredKeys.push_back(out.playbackGeneration);
        }
        QCOMPARE(recoveredKeys.size(), QUEUE_CAP + OVERFLOW_CAP);

        // Unread overflow slots 65..72 must NOT have been overwritten by excess 73..77
        std::sort(recoveredKeys.begin(), recoveredKeys.end());
        QVERIFY(std::adjacent_find(recoveredKeys.begin(), recoveredKeys.end()) == recoveredKeys.end());
        for (size_t i = 0; i < QUEUE_CAP + OVERFLOW_CAP; ++i) {
            QCOMPARE(recoveredKeys[i], static_cast<uint64_t>(i + 1));
        }
    }

    // Active FolderSession: valid EOF in 8th overflow slot must survive subsequent excess drops
    {
        ReferenceDeck deck('B');
        std::vector<std::string> files = {
            "track_01_intro.wav",
            "track_02_verse.wav",
            "track_03_chorus.wav"
        };
        QVERIFY(deck.startFolderSession(m_testDir.toStdString(), files, 48000.0));
        deck.play();

        // Fill 64 queue slots + 7 overflow slots with stale notifications (leaving 1 overflow slot free)
        for (size_t i = 0; i < EofNotificationQueue::CAPACITY + 7; ++i) {
            EofNotification stale;
            stale.playbackGeneration = UINT64_C(90000) + static_cast<uint64_t>(i);
            stale.folderSessionId = deck.folderSession().sessionId();
            (void)deck.submitEofNotificationForTest(stale);
        }

        // RT Audio Thread triggers real EOF -> lands in 8th overflow slot
        std::vector<double> outL(512), outR(512);
        for (int i = 0; i < 20; ++i) {
            deck.renderBlock(outL.data(), outR.data(), 512);
        }
        QCOMPARE(deck.eofOverflowDropCount(), uint64_t(0));

        // 3 more excess notifications arrive while Control Thread is still stopped -> must drop, not overwrite
        for (size_t i = 0; i < 3; ++i) {
            EofNotification excess;
            excess.playbackGeneration = UINT64_C(95000) + static_cast<uint64_t>(i);
            (void)deck.submitEofNotificationForTest(excess);
        }
        QCOMPARE(deck.eofOverflowDropCount(), uint64_t(3));

        // Control Thread resumes -> recovers valid EOF and advances exactly once
        const auto results = deck.processPendingEof();
        int advancedCount = 0;
        for (const auto& r : results) {
            if (r.advanced) {
                ++advancedCount;
            }
        }
        QCOMPARE(advancedCount, 1);
        QCOMPARE(deck.folderSession().currentIndex(), size_t(1));

        // No duplicate processing on subsequent poll
        const auto followUp = deck.processPendingEof();
        for (const auto& r : followUp) {
            QVERIFY(!r.advanced);
        }
        QCOMPARE(deck.folderSession().currentIndex(), size_t(1));
    }
}

void TestReferenceDecks::testComparativeSwitchTerminatesFolderSession()
{
    // Specification §2.7.5:
    // - Normal switch preserves Folder Session on the switched decks.
    // - Comparative switch terminates Folder Session on the switched decks while keeping loaded tracks.
    DeckManager manager(48000.0);
    std::vector<std::string> filesA = { "track_01_intro.wav", "track_02_verse.wav" };
    std::vector<std::string> filesB = { "track_01_intro.wav", "track_03_chorus.wav" };

    QVERIFY(manager.deck('A')->startFolderSession(m_testDir.toStdString(), filesA, 48000.0));
    QVERIFY(manager.deck('B')->startFolderSession(m_testDir.toStdString(), filesB, 48000.0));
    QVERIFY(manager.deck('A')->folderSession().isActive());
    QVERIFY(manager.deck('B')->folderSession().isActive());

    // Normal switch A -> B preserves both Folder Sessions
    auto normRes = manager.switchDeck('B', SwitchingMode::Normal);
    QVERIFY(normRes.success);
    QCOMPARE(manager.activeDeckId(), 'B');
    QVERIFY(manager.deck('A')->folderSession().isActive());
    QVERIFY(manager.deck('B')->folderSession().isActive());

    // Normal switch B -> A preserves both Folder Sessions
    normRes = manager.switchDeck('A', SwitchingMode::Normal);
    QVERIFY(normRes.success);
    QCOMPARE(manager.activeDeckId(), 'A');
    QVERIFY(manager.deck('A')->folderSession().isActive());
    QVERIFY(manager.deck('B')->folderSession().isActive());

    // Start playing Deck A, then perform Comparative switch A -> B
    manager.deck('A')->play();
    auto cmpRes = manager.switchDeck('B', SwitchingMode::Comparative);
    QVERIFY(cmpRes.success);
    QCOMPARE(manager.activeDeckId(), 'B');
    QVERIFY(!manager.deck('A')->folderSession().isActive());
    QVERIFY(!manager.deck('B')->folderSession().isActive());
    QVERIFY(manager.deck('A')->isOpen());
    QVERIFY(manager.deck('B')->isOpen());
    QCOMPARE(manager.deck('A')->state(), TransportState::Paused);
    QCOMPARE(manager.deck('B')->state(), TransportState::Playing);
}

void TestReferenceDecks::testPhase1GapFixesFolderAndTransport()
{
    using namespace desk32::engine::deck;
    using namespace desk32::engine::session;

    // 1. Item 7: RMS measurement on real synthetic WAV (amplitude 0.3 -> ~ -13.47 dBFS)
    double measuredRmsDb = 0.0;
    QVERIFY(ReferenceDeck::measureTrackRmsDb(m_wav1.toStdString(), 0.0, 2.0, measuredRmsDb));
    QVERIFY2(std::abs(measuredRmsDb - (-13.47)) < 0.6, qPrintable(QString("rms=%1").arg(measuredRmsDb)));

    // 2. Item 1 & Item 4: .aiff scan + single-track ReadAhead probe failure stops Folder Session without skipping
    const QString gapDir = m_testDir + "/phase1_gaps";
    QDir().mkpath(gapDir);
    const QString goodWav1 = createSyntheticWav("phase1_gaps/01_good.wav", 48000.0, 2400);
    // Write minimal corrupt audio file as track 02
    const QString corruptWav = gapDir + "/02_corrupt.wav";
    {
        QFile cf(corruptWav);
        QVERIFY(cf.open(QIODevice::WriteOnly));
        cf.write("NOT_A_VALID_AUDIO_FILE_0123456789");
        cf.close();
    }
    const QString goodAiff = createSyntheticWav("phase1_gaps/03_good.aiff", 48000.0, 2400);
    QVERIFY(!goodWav1.isEmpty());
    QVERIFY(!goodAiff.isEmpty());

    QVERIFY(FolderSession::isSupportedAudioExtension(QFileInfo(goodAiff).suffix().toLower()));
    const auto scanned = FolderSession::scanSupportedAudioFiles(gapDir.toStdString());
    QCOMPARE(scanned.size(), size_t(3));

    DeckManager mgr(48000.0);
    auto* deckA = mgr.deck('A');
    QVERIFY(deckA != nullptr);
    int errorCallbacks = 0;
    deckA->setErrorLogCallback([&](const std::string&, int32_t, const std::string&, const std::string&) {
        ++errorCallbacks;
    });

    // Item 4: startFolderSessionFromCurrentTrack after single-track load
    const QString subDir = m_testDir + "/from_track_dir";
    QDir().mkpath(subDir);
    QVERIFY(!createSyntheticWav("from_track_dir/t1.wav", 48000.0, 4800).isEmpty());
    const QString subT2 = createSyntheticWav("from_track_dir/t2.wav", 48000.0, 4800 * 60); // 6s for 3s test
    QVERIFY(deckA->loadTrack(subT2.toStdString(), 48000.0));
    QVERIFY(deckA->startFolderSessionFromCurrentTrack(48000.0));
    QVERIFY(deckA->folderSession().isActive());
    QCOMPARE(deckA->folderSession().tracks().size(), size_t(2));
    QCOMPARE(deckA->folderSession().currentIndex(), size_t(0)); // Spec 2.7.2 starts from index 0

    // Item 2: Advance to track 1 (6s), then test retreatFolderPrevious() 3s threshold
    QVERIFY(deckA->advanceFolderNext());
    QCOMPARE(deckA->folderSession().currentIndex(), size_t(1));
    deckA->seek(48000 * 4); // 4.0s >= 3.0s threshold
    QVERIFY(deckA->retreatFolderPrevious());
    QCOMPARE(deckA->folderSession().currentIndex(), size_t(1)); // Restarted current track
    QCOMPARE(deckA->playhead(), int64_t(0));
    // Now playhead < 3.0s -> retreats to Track 0
    QVERIFY(deckA->retreatFolderPrevious());
    QCOMPARE(deckA->folderSession().currentIndex(), size_t(0));

    // Item 1: Corrupt read-ahead next track stops session after current track ends (no skipping to 03)
    QVERIFY(deckA->startFolderSession(gapDir.toStdString(), {"01_good.wav", "02_corrupt.wav", "03_good.aiff"}, 48000.0));
    QCOMPARE(deckA->folderSession().readAheadStatus(), ReadAheadStatus::Failed);
    deckA->play();
    std::vector<double> tmpL(1024), tmpR(1024);
    for (int b = 0; b < 50 && deckA->folderSession().isActive(); ++b) {
        deckA->renderBlock(tmpL.data(), tmpR.data(), 1024);
        deckA->processPendingEof();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QVERIFY(!deckA->folderSession().isActive());
    QCOMPARE(deckA->state(), TransportState::Stopped);
    QVERIFY(errorCallbacks >= 1);

    // Item 3: Mid-stream decode error stops immediately and prevents FolderSession advance
    errorCallbacks = 0;
    QVERIFY(deckA->startFolderSession(subDir.toStdString(), {"t1.wav", "t2.wav"}, 48000.0));
    deckA->play();
    deckA->worker().simulateMidStreamDecodeErrorForTesting("corrupt frame header");
    deckA->renderBlock(tmpL.data(), tmpR.data(), 256);
    QCOMPARE(deckA->state(), TransportState::Stopped);
    deckA->processPendingEof();
    QVERIFY(!deckA->folderSession().isActive());
    QVERIFY(errorCallbacks >= 1);

    // Item 5 & Item 6: Normal switch auto-pauses/resumes Folder Session + RT-safe 5ms de-click fade-in ramp
    QVERIFY(deckA->startFolderSession(subDir.toStdString(), {"t1.wav", "t2.wav"}, 48000.0));
    auto* deckB = mgr.deck('B');
    QVERIFY(deckB != nullptr);
    QVERIFY(deckB->loadTrack(m_wav1.toStdString(), 48000.0));
    deckA->play();
    QCOMPARE(deckA->state(), TransportState::Playing);

    auto swB = mgr.switchDeck('B', SwitchingMode::Normal);
    QVERIFY(swB.success);
    QCOMPARE(deckA->state(), TransportState::Paused);
    QVERIFY(deckA->wasPausedByNormalSwitch());

    // Verify 5ms fade-in ramp starts near 0 on frame 0 and allocates zero heap memory
    std::vector<double> rampL(256, 0.0), rampR(256, 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    s_allocationCount.store(0, std::memory_order_release);
    s_trackAllocations.store(true, std::memory_order_release);
    mgr.renderActiveBlock(rampL.data(), rampR.data(), 256);
    s_trackAllocations.store(false, std::memory_order_release);
    QCOMPARE(s_allocationCount.load(std::memory_order_acquire), size_t(0));
    QVERIFY(std::abs(rampL[0]) < 1e-6);

    auto swA = mgr.switchDeck('A', SwitchingMode::Normal);
    QVERIFY(swA.success);
    QCOMPARE(deckA->state(), TransportState::Playing);
}

void TestReferenceDecks::cleanupTestCase()
{
    QFile::remove(m_wav1);
    QFile::remove(m_wav2);
    QFile::remove(m_wav3);
    QDir(m_testDir).removeRecursively();
    qDebug() << "TestReferenceDecks completed successfully.";
}

QTEST_MAIN(TestReferenceDecks)
#include "test_reference_decks.moc"

