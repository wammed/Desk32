#include <QTest>
#include <QObject>
#include <QTemporaryDir>
#include <QFile>
#include <QDebug>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
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
    void testSeekDeterministicRace();
    void testSeekReleasesGuardAfterFirstChunkPrefill();
    void testPlayingSeekSingleAndBurstStress();
    void testConfigureRateDuringPlaybackNoUnderrun();
    void testGenuineUnderrunStillDetected();
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

    // 5. Verify Candidate A race is eliminated: underrunCount == 0 and no false snapshot
    QCOMPARE(deck.worker().underrunCount(), uint64_t(0));
    UnderrunSnapshot snap{};
    QVERIFY(!deck.worker().popUnderrunSnapshot(snap));

    // 6. Verify subsequent normal playback after Open completes produces ZERO underruns
    for (int i = 0; i < 10; ++i) {
        deck.renderBlock(outL.data(), outR.data(), 1024);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    QCOMPARE(deck.worker().underrunCount(), uint64_t(0));
    deck.stop();
}

void TestUnderrunInvestigation::testSeekDeterministicRace() {
    ReferenceDeck deck('A');
    const std::string longWav = (m_tempDir.path() + "/track_seek_race_44k.wav").toStdString();
    QVERIFY(writeTestWav(longWav, 44100.0, 5.0, 440.0));
    QVERIFY(deck.loadTrack(longWav, 48000.0));
    deck.play();

    std::vector<double> outL(1024, 0.0), outR(1024, 0.0);
    QCOMPARE(deck.renderBlock(outL.data(), outR.data(), 1024), size_t(1024));
    QCOMPARE(deck.worker().underrunCount(), uint64_t(0));

    // Pause Worker right inside Seek after resetProducer() + isEof=false, before seek()/prefill
    std::mutex mtx;
    std::condition_variable cv;
    bool workerInSeekWindow = false;
    bool audioThreadDone = false;

    deck.worker().setInSeekTransitionHook([&](uint64_t) {
        std::unique_lock<std::mutex> lock(mtx);
        workerInSeekWindow = true;
        cv.notify_all();
        cv.wait_for(lock, std::chrono::seconds(5), [&]() { return audioThreadDone; });
    });

    size_t inWindowRead = 999999;
    double inWindowPeak = 1.0;
    std::thread audioThread([&]() {
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(5), [&]() { return workerInSeekWindow; });
        }
        std::fill(outL.begin(), outL.end(), 1.0);
        std::fill(outR.begin(), outR.end(), 1.0);
        inWindowRead = deck.renderBlock(outL.data(), outR.data(), 1024);
        double peak = 0.0;
        for (size_t i = 0; i < 1024; ++i) {
            peak = std::max(peak, std::max(std::abs(outL[i]), std::abs(outR[i])));
        }
        inWindowPeak = peak;
        {
            std::lock_guard<std::mutex> lock(mtx);
            audioThreadDone = true;
        }
        cv.notify_all();
    });

    const auto seekRes = deck.seekWithResult(44100); // 1.0s
    audioThread.join();
    deck.worker().setInSeekTransitionHook(nullptr);

    QVERIFY(seekRes.success);
    QCOMPARE(inWindowRead, size_t(0));
    QCOMPARE(inWindowPeak, 0.0);
    QCOMPARE(deck.worker().underrunCount(), uint64_t(0));

    UnderrunSnapshot snap{};
    QVERIFY(!deck.worker().popUnderrunSnapshot(snap));

    // Normal playback resumes immediately after Seek with zero underruns
    const size_t afterRead = deck.renderBlock(outL.data(), outR.data(), 1024);
    QCOMPARE(afterRead, size_t(1024));
    double afterPeak = 0.0;
    for (size_t i = 0; i < afterRead; ++i) {
        afterPeak = std::max(afterPeak, std::max(std::abs(outL[i]), std::abs(outR[i])));
    }
    QVERIFY(afterPeak > 0.01);
    QCOMPARE(deck.worker().underrunCount(), uint64_t(0));
    deck.stop();
}

void TestUnderrunInvestigation::testSeekReleasesGuardAfterFirstChunkPrefill() {
    ReferenceDeck deck('A');
    const std::string longWav = (m_tempDir.path() + "/track_seek_first_chunk_44k.wav").toStdString();
    QVERIFY(writeTestWav(longWav, 44100.0, 5.0, 440.0));
    QVERIFY(deck.loadTrack(longWav, 48000.0));
    deck.play();

    std::mutex mtx;
    std::condition_variable cv;
    bool seekEntered = false;
    int seekChunkCount = 0;
    bool afterFirstChunk = false;
    bool audioVerified = false;
    bool abortWait = false;

    struct HookGuard {
        ReferenceDeck& deckRef;
        std::mutex& mtxRef;
        std::condition_variable& cvRef;
        bool& abortRef;
        ~HookGuard() {
            {
                std::lock_guard<std::mutex> lock(mtxRef);
                abortRef = true;
            }
            cvRef.notify_all();
            deckRef.worker().setInSeekTransitionHook(nullptr);
            deckRef.worker().setBeforeDecodeChunkHook(nullptr);
            deckRef.stop();
        }
    } cleanup{deck, mtx, cv, abortWait};

    deck.worker().setInSeekTransitionHook([&](uint64_t) {
        std::lock_guard<std::mutex> lock(mtx);
        seekEntered = true;
        seekChunkCount = 0;
    });

    deck.worker().setBeforeDecodeChunkHook([&](uint64_t) {
        std::unique_lock<std::mutex> lock(mtx);
        if (!seekEntered) {
            return;
        }
        const int idx = seekChunkCount++;
        if (idx == 1) {
            // Chunk 0 (1st prefill chunk) has completed and guard should already be released.
            afterFirstChunk = true;
            cv.notify_all();
            cv.wait_for(lock, std::chrono::seconds(5), [&]() { return audioVerified || abortWait; });
        }
    });

    size_t midPrefillRead = 0;
    double midPrefillPeak = 0.0;
    std::thread audioThread([&]() {
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(5), [&]() { return afterFirstChunk || abortWait; });
        }
        std::vector<double> outL(256, 0.0), outR(256, 0.0);
        midPrefillRead = deck.renderBlock(outL.data(), outR.data(), 256);
        double peak = 0.0;
        for (size_t i = 0; i < midPrefillRead; ++i) {
            peak = std::max(peak, std::max(std::abs(outL[i]), std::abs(outR[i])));
        }
        midPrefillPeak = peak;
        {
            std::lock_guard<std::mutex> lock(mtx);
            audioVerified = true;
        }
        cv.notify_all();
    });

    const auto seekRes = deck.seekWithResult(88200); // 2.0s
    audioThread.join();

    QVERIFY(seekRes.success);
    QCOMPARE(midPrefillRead, size_t(256));
    QVERIFY(midPrefillPeak > 0.01);
    QCOMPARE(deck.worker().underrunCount(), uint64_t(0));
    UnderrunSnapshot snap{};
    QVERIFY(!deck.worker().popUnderrunSnapshot(snap));
}

void TestUnderrunInvestigation::testPlayingSeekSingleAndBurstStress() {
    ReferenceDeck deck('A');
    const std::string longWav = (m_tempDir.path() + "/track_seek_stress_44k.wav").toStdString();
    QVERIFY(writeTestWav(longWav, 44100.0, 5.0, 440.0));
    QVERIFY(deck.loadTrack(longWav, 48000.0));
    deck.play();

    std::atomic<bool> stopAudio{false};
    std::atomic<uint64_t> cbCount{0};
    std::thread audioThread([&]() {
        std::vector<double> outL(256, 0.0), outR(256, 0.0);
        auto nextWake = std::chrono::steady_clock::now();
        while (!stopAudio.load(std::memory_order_acquire)) {
            deck.renderBlock(outL.data(), outR.data(), 256);
            cbCount.fetch_add(1, std::memory_order_relaxed);
            nextWake += std::chrono::microseconds(5333);
            std::this_thread::sleep_until(nextWake);
        }
    });

    // 1. Single seeks interspersed with playback
    for (int i = 0; i < 10; ++i) {
        const int64_t target = static_cast<int64_t>((i * 17640) % 176400);
        const auto res = deck.seekWithResult(target);
        QVERIFY(res.success);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    // 2. Rapid consecutive seek burst
    for (int i = 0; i < 30; ++i) {
        const int64_t target = static_cast<int64_t>(((i * 11025) + 4410) % 176400);
        const auto res = deck.seekWithResult(target);
        QVERIFY(res.success);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stopAudio.store(true, std::memory_order_release);
    audioThread.join();

    QVERIFY(cbCount.load() > 0);
    QCOMPARE(deck.worker().underrunCount(), uint64_t(0));
    UnderrunSnapshot snap{};
    QVERIFY(!deck.worker().popUnderrunSnapshot(snap));
    deck.stop();
}

void TestUnderrunInvestigation::testConfigureRateDuringPlaybackNoUnderrun() {
    ReferenceDeck deck('A');
    const std::string longWav = (m_tempDir.path() + "/track_rate_change_44k.wav").toStdString();
    QVERIFY(writeTestWav(longWav, 44100.0, 5.0, 440.0));
    QVERIFY(deck.loadTrack(longWav, 48000.0));
    deck.play();

    std::atomic<bool> stopAudio{false};
    std::atomic<uint64_t> cbCount{0};
    std::thread audioThread([&]() {
        std::vector<double> outL(256, 0.0), outR(256, 0.0);
        auto nextWake = std::chrono::steady_clock::now();
        while (!stopAudio.load(std::memory_order_acquire)) {
            deck.renderBlock(outL.data(), outR.data(), 256);
            cbCount.fetch_add(1, std::memory_order_relaxed);
            nextWake += std::chrono::microseconds(5333);
            std::this_thread::sleep_until(nextWake);
        }
    });

    const std::array<double, 6> rates{96000.0, 44100.0, 48000.0, 88200.0, 44100.0, 48000.0};
    for (double rate : rates) {
        deck.configureGraphRate(rate);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    stopAudio.store(true, std::memory_order_release);
    audioThread.join();

    QVERIFY(cbCount.load() > 0);
    QCOMPARE(deck.worker().underrunCount(), uint64_t(0));
    UnderrunSnapshot snap{};
    QVERIFY(!deck.worker().popUnderrunSnapshot(snap));
    deck.stop();
}

void TestUnderrunInvestigation::testGenuineUnderrunStillDetected() {
    // Verify genuine underrun during active playback (post-Open, non-EOF) and post-starvation recovery
    // deterministically using a test-only producer supply gate (no timing/sleep dependence).
    struct SupplyGate {
        std::mutex mtx;
        std::condition_variable cv;
        bool stalled{false};
        bool allowProceed{false};
        bool resumed{false};
        bool abort{false};

        void onBeforeChunk(uint64_t chunkIdx) {
            if (chunkIdx < 4) {
                // Allow Open prefill (chunks 0..3) to populate the ring buffer normally.
                return;
            }
            if (chunkIdx == 4) {
                std::unique_lock<std::mutex> lock(mtx);
                stalled = true;
                cv.notify_all();
                cv.wait_for(lock, std::chrono::seconds(10), [this]() {
                    return allowProceed || abort;
                });
                return;
            }
            if (chunkIdx == 6) {
                // Chunks 4 and 5 have completed decoding and written PCM to the ring buffer.
                std::lock_guard<std::mutex> lock(mtx);
                resumed = true;
                cv.notify_all();
            }
        }

        bool waitUntilStalled() {
            std::unique_lock<std::mutex> lock(mtx);
            return cv.wait_for(lock, std::chrono::seconds(10), [this]() {
                return stalled || abort;
            }) && stalled;
        }

        void resumeSupply() {
            {
                std::lock_guard<std::mutex> lock(mtx);
                allowProceed = true;
            }
            cv.notify_all();
        }

        bool waitUntilResumed() {
            std::unique_lock<std::mutex> lock(mtx);
            return cv.wait_for(lock, std::chrono::seconds(10), [this]() {
                return resumed || abort;
            }) && resumed;
        }

        void releaseForTeardown() {
            {
                std::lock_guard<std::mutex> lock(mtx);
                abort = true;
                allowProceed = true;
            }
            cv.notify_all();
        }
    };

    ReferenceDeck deck('A');
    SupplyGate gate;

    struct HookGuard {
        ReferenceDeck& deckRef;
        SupplyGate& gateRef;
        ~HookGuard() {
            gateRef.releaseForTeardown();
            deckRef.worker().setBeforeDecodeChunkHook(nullptr);
            deckRef.stop();
        }
    } cleanup{deck, gate};

    const std::string longWav = (m_tempDir.path() + "/track_long_44k.wav").toStdString();
    QVERIFY(writeTestWav(longWav, 44100.0, 5.0, 440.0));

    // Install supply gate before loadTrack so worker stalls deterministically after Open prefill (before chunk 4).
    deck.worker().setBeforeDecodeChunkHook([&gate](uint64_t chunkIdx) {
        gate.onBeforeChunk(chunkIdx);
    });

    QVERIFY(deck.loadTrack(longWav, 48000.0));
    QVERIFY(gate.waitUntilStalled());

    QVERIFY(deck.isOpen());
    QVERIFY(!deck.worker().isEof());
    QVERIFY(deck.worker().ringBuffer().availableRead() > 0);

    deck.play();
    QVERIFY(deck.isPlaying());
    QVERIFY(deck.worker().isPlaying());

    // 1. Drain the prefilled ring buffer with normal audio blocks until an underrun is observed.
    constexpr size_t blockFrames = 1024;
    std::vector<double> outL(blockFrames, 0.0), outR(blockFrames, 0.0);
    bool sawPartialUnderrun = false;
    for (int i = 0; i < 64; ++i) {
        const size_t read = deck.renderBlock(outL.data(), outR.data(), blockFrames);
        if (read < blockFrames) {
            sawPartialUnderrun = true;
            break;
        }
    }
    QVERIFY(sawPartialUnderrun);

    // 2. Render one more block while supply is still stalled to confirm continued playback detects underrun.
    const size_t emptyRead = deck.renderBlock(outL.data(), outR.data(), blockFrames);
    QCOMPARE(emptyRead, size_t(0));

    // 3. Verify Worker is NOT EOF, playback request remains active, and underrun snapshots are accurate.
    QVERIFY(!deck.worker().isEof());
    QVERIFY(!deck.worker().isEofDelivered());
    QVERIFY(deck.isPlaying());
    QVERIFY(deck.worker().isPlaying());
    QVERIFY(deck.worker().underrunCount() >= uint64_t(2));

    std::vector<UnderrunSnapshot> snaps;
    UnderrunSnapshot snap{};
    while (deck.worker().popUnderrunSnapshot(snap)) {
        snaps.push_back(snap);
    }
    QVERIFY(snaps.size() >= size_t(2));
    for (const auto& s : snaps) {
        QVERIFY(s.isOpenAtEntry);
        QVERIFY(s.isPlayingAtEntry);
        QVERIFY(!s.isEofAtCheck);
        QVERIFY(s.isOpenAtUnderrun);
        QVERIFY(s.isPlayingAtUnderrun);
        QVERIFY(s.readFrames < s.requestedFrames);
    }
    QCOMPARE(snaps.back().readFrames, uint64_t(0));
    QCOMPARE(snaps.back().ringAvailBeforeRead, uint64_t(0));

    // 4. Resume data supply and verify recovery without additional underruns.
    const uint64_t underrunsBeforeRecovery = deck.worker().underrunCount();
    gate.resumeSupply();
    QVERIFY(gate.waitUntilResumed());

    QVERIFY(!deck.worker().isEof());
    QVERIFY(deck.isPlaying());
    QVERIFY(deck.worker().isPlaying());
    QVERIFY(deck.worker().ringBuffer().availableRead() >= blockFrames);

    std::fill(outL.begin(), outL.end(), 0.0);
    std::fill(outR.begin(), outR.end(), 0.0);
    const size_t recoveredRead = deck.renderBlock(outL.data(), outR.data(), blockFrames);
    QCOMPARE(recoveredRead, blockFrames);
    QCOMPARE(deck.worker().underrunCount(), underrunsBeforeRecovery);
    QVERIFY(!deck.worker().isEof());

    double peak = 0.0;
    for (size_t i = 0; i < recoveredRead; ++i) {
        peak = std::max(peak, std::max(std::abs(outL[i]), std::abs(outR[i])));
    }
    QVERIFY(peak > 0.01);

    deck.worker().setBeforeDecodeChunkHook(nullptr);
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
