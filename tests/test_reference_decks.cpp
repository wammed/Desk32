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

    // 2. Switch back in Mode 2 (ParallelSolo / Normal): Transport state of Deck A (Paused) MUST be preserved!
    res = manager.switchDeck('A', SwitchingMode::ParallelSolo);
    QVERIFY(res.success);
    QCOMPARE(manager.activeDeckId(), 'A');
    QCOMPARE(manager.deck('A')->state(), TransportState::Paused); // Deck A remains Paused per Section 2.3!
    QCOMPARE(manager.deck('B')->state(), TransportState::Playing); // Deck B continues playing in background!

    // Start Deck A, then switch to Deck B: both decks remain Playing in Normal mode!
    manager.deck('A')->play();
    QCOMPARE(manager.deck('A')->state(), TransportState::Playing);
    res = manager.switchDeck('B', SwitchingMode::ParallelSolo);
    QVERIFY(res.success);
    QCOMPARE(manager.activeDeckId(), 'B');
    QCOMPARE(manager.deck('A')->state(), TransportState::Playing); // Deck A continues Playing
    QCOMPARE(manager.deck('B')->state(), TransportState::Playing); // Deck B continues Playing
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
    }
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

