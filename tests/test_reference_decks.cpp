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
#include "engine/deck/DeckManager.hpp"

static std::atomic<bool> s_trackAllocations{false};
static std::atomic<size_t> s_allocationCount{0};

void* operator new(size_t size) {
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

using namespace desk32::engine::deck;

class TestReferenceDecks : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void testFourDecksIndependentTransport();
    void testSwitchingDualModesAndQuickAB();
    void testDeckLockProtectionAndTransportPermission();
    void testFolderSessionIndexZeroAndReadAhead();
    void testZeroDynamicAllocationInRenderBlock();
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

    // 2. Switch back in Mode 2 (ParallelSolo)
    res = manager.switchDeck('A', SwitchingMode::ParallelSolo);
    QVERIFY(res.success);
    QCOMPARE(manager.activeDeckId(), 'A');
    QCOMPARE(manager.deck('A')->state(), TransportState::Playing);
    QCOMPARE(manager.deck('B')->state(), TransportState::Playing); // Deck B continues playing in background!
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

