#include <QTest>
#include <QObject>
#include <QTemporaryDir>
#include <QFile>
#include <QDebug>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <thread>
#include <vector>
#include <algorithm>
#include <numeric>
#include "engine/deck/ReferenceDeck.hpp"

using namespace desk32::engine::deck;
using namespace desk32::engine::decoder;

namespace {

bool writeTestWav(const std::string& path, double sampleRate, double durationSec, double freqHz) {
    const uint32_t sr = static_cast<uint32_t>(sampleRate);
    const uint16_t channels = 2;
    const uint16_t bitsPerSample = 16;
    const uint32_t totalFrames = static_cast<uint32_t>(sampleRate * durationSec);
    const uint32_t dataSize = totalFrames * channels * (bitsPerSample / 8);
    const uint32_t riffSize = 36 + dataSize;

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;

    auto writeU32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto writeU16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };

    out.write("RIFF", 4);
    writeU32(riffSize);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeU32(16);
    writeU16(1); // PCM
    writeU16(channels);
    writeU32(sr);
    writeU32(sr * channels * (bitsPerSample / 8));
    writeU16(channels * (bitsPerSample / 8));
    writeU16(bitsPerSample);
    out.write("data", 4);
    writeU32(dataSize);

    for (uint32_t i = 0; i < totalFrames; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double val = 0.25 * std::sin(2.0 * M_PI * freqHz * t);
        const int16_t s = static_cast<int16_t>(val * 32767.0);
        writeU16(static_cast<uint16_t>(s));
        writeU16(static_cast<uint16_t>(s));
    }
    return out.good();
}

double percentileMs(std::vector<double> vals, double p) {
    if (vals.empty()) return 0.0;
    std::sort(vals.begin(), vals.end());
    const size_t idx = std::min(vals.size() - 1, static_cast<size_t>(p * static_cast<double>(vals.size() - 1)));
    return vals[idx];
}

} // namespace

class TestUnderrunInvestigation : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void testCandidateADeterministicRace();
    void testCandidateBLatencyAndPrefill();
    void testHighFrequencyAutoAdvance600();
    void cleanupTestCase();

private:
    QTemporaryDir m_tempDir;
    std::string m_wav44kA;
    std::string m_wav44kB;
    std::string m_wav48kA;
};

void TestUnderrunInvestigation::initTestCase() {
    QVERIFY(m_tempDir.isValid());
    m_wav44kA = (m_tempDir.path() + "/track_01_44k.wav").toStdString();
    m_wav44kB = (m_tempDir.path() + "/track_02_44k.wav").toStdString();
    m_wav48kA = (m_tempDir.path() + "/track_03_48k.wav").toStdString();
    QVERIFY(writeTestWav(m_wav44kA, 44100.0, 0.25, 440.0));
    QVERIFY(writeTestWav(m_wav44kB, 44100.0, 0.50, 554.37));
    QVERIFY(writeTestWav(m_wav48kA, 48000.0, 0.50, 659.25));
}

void TestUnderrunInvestigation::testCandidateADeterministicRace() {
    ReferenceDeck deck('A');
    std::vector<std::string> files = {"track_01_44k.wav", "track_02_44k.wav"};
    QVERIFY(deck.startFolderSession(m_tempDir.path().toStdString(), files, 48000.0));
    deck.play();

    // 1. Drain track 0 to EOF via RT-style renderBlock calls
    std::vector<double> outL(1024, 0.0), outR(1024, 0.0);
    for (int i = 0; i < 60 && !deck.needsFolderAdvance(); ++i) {
        deck.renderBlock(outL.data(), outR.data(), 1024);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QVERIFY(deck.needsFolderAdvance());
    QCOMPARE(deck.worker().underrunCount(), uint64_t(0));

    // 2. Install deterministic in-transition hook (fires after m_isEof=false, before m_isPlaying=false)
    std::atomic<bool> workerInWindow{false};
    std::atomic<bool> audioThreadDone{false};
    deck.worker().setInTransitionHook([&](uint64_t) {
        workerInWindow.store(true, std::memory_order_release);
        while (!audioThreadDone.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    // 3. Simulated Audio Thread waits for worker to enter the exact race window, then calls renderBlock()
    std::thread audioThread([&]() {
        while (!workerInWindow.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        deck.renderBlock(outL.data(), outR.data(), 1024);
        audioThreadDone.store(true, std::memory_order_release);
    });

    // 4. Control Thread triggers AutoAdvance
    const auto results = deck.processPendingEof();
    audioThread.join();
    deck.worker().setInTransitionHook(nullptr);

    QCOMPARE(results.size(), size_t(1));
    QVERIFY(results[0].advanced);

    // 5. Verify Candidate A reproduced: underrunCount == 1 with exact transition-window snapshot
    QCOMPARE(deck.worker().underrunCount(), uint64_t(1));

    UnderrunSnapshot snap{};
    QVERIFY(deck.worker().popUnderrunSnapshot(snap));
    qInfo() << "[Candidate A Deterministic]"
            << "openPhase=" << snap.openPhase
            << "isOpenEntry=" << snap.isOpenAtEntry
            << "isPlayingEntry=" << snap.isPlayingAtEntry
            << "isEofCheck=" << snap.isEofAtCheck
            << "ringAvail=" << snap.ringAvailBeforeRead
            << "reqFrames=" << snap.requestedFrames
            << "readFrames=" << snap.readFrames
            << "reqId=" << snap.requestId
            << "activeReqId=" << snap.activeRequestId;

    QCOMPARE(snap.openPhase, static_cast<uint8_t>(OpenTransitionPhase::AfterEofClearedBeforePlayCleared));
    QVERIFY(snap.isOpenAtEntry);
    QVERIFY(snap.isPlayingAtEntry);
    QVERIFY(!snap.isEofAtCheck);
    QCOMPARE(snap.ringAvailBeforeRead, uint64_t(0));
    QCOMPARE(snap.readFrames, uint64_t(0));

    // 6. Verify subsequent normal playback after Open completes produces ZERO additional underruns
    for (int i = 0; i < 10; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 1024);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    QCOMPARE(deck.worker().underrunCount(), uint64_t(1));
    deck.stop();
}

void TestUnderrunInvestigation::testCandidateBLatencyAndPrefill() {
    auto measureFormat = [&](const std::string& label, const std::string& path) {
        ReferenceDeck deck('A');
        QVERIFY(deck.loadTrack(path, 48000.0));
        const auto openSnap = deck.worker().readOpenTransitionSnapshot();
        const auto perf = deck.worker().readDecodePerfStats();
        QVERIFY(openSnap.valid);
        const double wakeDelayMs = static_cast<double>(openSnap.workerWokeNs - openSnap.cmdNotifiedNs) / 1e6;
        const double decOpenMs = static_cast<double>(openSnap.decoderOpenEndNs - openSnap.decoderOpenStartNs) / 1e6;
        const double eofToPlayMs = static_cast<double>(openSnap.playingClearedNs - openSnap.eofClearedNs) / 1e6;
        const double prefillMs = static_cast<double>(openSnap.prefillEndNs - openSnap.prefillStartNs) / 1e6;
        const double firstPcmMs = static_cast<double>(openSnap.firstPcmSupplyNs - openSnap.openRequestedNs) / 1e6;
        const double totalOpenMs = static_cast<double>(openSnap.openCompleteNs - openSnap.openRequestedNs) / 1e6;
        const double prefillAudioMs = (static_cast<double>(openSnap.prefillRingAvailAfter) * 1000.0) / 48000.0;

        qInfo() << "[Candidate B]" << QString::fromStdString(label)
                << "wakeDelayMs=" << wakeDelayMs
                << "decOpenMs=" << decOpenMs
                << "eofToPlayClearedMs=" << eofToPlayMs
                << "prefillMs=" << prefillMs
                << "firstPcmMs=" << firstPcmMs
                << "totalOpenMs=" << totalOpenMs
                << "prefillRingFrames=" << openSnap.prefillRingAvailAfter
                << "prefillAudioMs=" << prefillAudioMs
                << "maxChunkMs=" << (static_cast<double>(perf.maxChunkTotalNs) / 1e6)
                << "maxResampleMs=" << (static_cast<double>(perf.maxResampleNs) / 1e6);
        QVERIFY(openSnap.prefillRingAvailAfter > 0);
        deck.stop();
    };

    measureFormat("44.1kHz->48kHz (SOXR_VHQ)", m_wav44kB);
    measureFormat("48kHz->48kHz (Bypass)", m_wav48kA);
}

void TestUnderrunInvestigation::testHighFrequencyAutoAdvance600() {
    // 600 AutoAdvance transitions over 0.25s tracks (44.1kHz -> 48kHz) with real-time 21.33ms Audio Thread
    ReferenceDeck deck('A');
    std::vector<std::string> files;
    files.reserve(601);
    for (int i = 0; i < 601; ++i) {
        files.push_back((i % 2 == 0) ? "track_01_44k.wav" : "track_02_44k.wav");
    }
    QVERIFY(deck.startFolderSession(m_tempDir.path().toStdString(), files, 48000.0));
    deck.play();

    std::atomic<bool> stopAudio{false};
    std::atomic<uint64_t> cbCount{0};
    std::thread audioThread([&]() {
        std::vector<double> outL(1024, 0.0), outR(1024, 0.0);
        auto nextWake = std::chrono::steady_clock::now();
        while (!stopAudio.load(std::memory_order_acquire)) {
            deck.renderBlock(outL.data(), outR.data(), 1024);
            cbCount.fetch_add(1, std::memory_order_relaxed);
            nextWake += std::chrono::microseconds(21333);
            std::this_thread::sleep_until(nextWake);
        }
    });

    std::vector<double> openTotalMs;
    std::vector<double> eofToPlayMs;
    std::vector<double> prefillMs;
    int advances = 0;
    const auto testStart = std::chrono::steady_clock::now();

    while (advances < 600) {
        if (std::chrono::steady_clock::now() - testStart > std::chrono::seconds(240)) {
            break;
        }
        const auto resList = deck.processPendingEof();
        for (const auto& r : resList) {
            if (r.advanced) {
                ++advances;
                const auto snap = deck.worker().readOpenTransitionSnapshot();
                if (snap.valid) {
                    openTotalMs.push_back(static_cast<double>(snap.openCompleteNs - snap.openRequestedNs) / 1e6);
                    eofToPlayMs.push_back(static_cast<double>(snap.playingClearedNs - snap.eofClearedNs) / 1e6);
                    prefillMs.push_back(static_cast<double>(snap.prefillEndNs - snap.prefillStartNs) / 1e6);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    stopAudio.store(true, std::memory_order_release);
    audioThread.join();

    const uint64_t underruns = deck.worker().underrunCount();
    qInfo() << "[HighFreq AutoAdvance]"
            << "advances=" << advances
            << "callbacks=" << cbCount.load()
            << "underruns=" << underruns
            << "openTotal_p50_ms=" << percentileMs(openTotalMs, 0.50)
            << "openTotal_p95_ms=" << percentileMs(openTotalMs, 0.95)
            << "openTotal_max_ms=" << percentileMs(openTotalMs, 1.00)
            << "eofToPlay_p50_ms=" << percentileMs(eofToPlayMs, 0.50)
            << "eofToPlay_max_ms=" << percentileMs(eofToPlayMs, 1.00)
            << "prefill_p50_ms=" << percentileMs(prefillMs, 0.50)
            << "prefill_max_ms=" << percentileMs(prefillMs, 1.00);

    UnderrunSnapshot uSnap{};
    while (deck.worker().popUnderrunSnapshot(uSnap)) {
        qInfo() << "[HighFreq UnderrunSnapshot]"
                << "total=" << uSnap.underrunTotal
                << "openPhase=" << uSnap.openPhase
                << "reqId=" << uSnap.requestId
                << "activeReqId=" << uSnap.activeRequestId
                << "isOpenEntry=" << uSnap.isOpenAtEntry
                << "isPlayingEntry=" << uSnap.isPlayingAtEntry
                << "isEofCheck=" << uSnap.isEofAtCheck
                << "ringAvail=" << uSnap.ringAvailBeforeRead;
    }

    QVERIFY(advances >= 600);
    deck.stop();
}

void TestUnderrunInvestigation::cleanupTestCase() {
}

QTEST_MAIN(TestUnderrunInvestigation)
#include "test_underrun_investigation.moc"
