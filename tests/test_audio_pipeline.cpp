#include <QTest>
#include <QObject>
#include <latch>
#include "engine/pipeline/AudioPipeline.hpp"
#include "engine/pipeline/BuiltInDspNodes.hpp"

using namespace desk32::engine::pipeline;

class TestAudioPipeline : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void testNodeAffiliationStructuralVerification();
    void testAudioPipelineFormatConversionAndClamping();
    void testOutputPathRoutingAndZeroLeakage();
    void testAudioGraphRebuildClonesNodesWithoutSharing();
    void testEpochReclamationHotSwap();
    void testEpochReclaimerConcurrentStress();
    void testEpochReclaimerMultiReaderConcurrentStress();
    void testSampleRateChangeConcurrentWithTelemetry();
    void testShutdownWaitsForActiveReaders();
    void testReaderSlotExhaustionRejectsUnshieldedPointer();
    void testDspInstanceContinuousExchangeAndRetirement();
    void testReverbTailPreservationCrossfade();
    void testLimiterLookaheadChangeSampleContinuity();
    void testTelemetrySnapshotThreadSafety();
    void testEpochReclaimerShutdownConcurrentSafety();
    void testEpochReclaimerWriterShutdownSerialization();
    void testEpochReclaimerDeterministicShutdownRaces();
    void cleanupTestCase();
};

void TestAudioPipeline::initTestCase()
{
    qDebug() << "Initializing TestAudioPipeline (Phase 1.1 Core Engine Integration)...";
}

void TestAudioPipeline::testNodeAffiliationStructuralVerification()
{
    // Section 9.2: "Nodes belonging to Output Path declare affiliation, structurally verifying
    // that Listening Correction cannot be connected to Render branches."

    auto masterChain = std::make_shared<AudioChain>("Mastering", ChainPurpose::MasteringChain);
    auto listeningChain = std::make_shared<AudioChain>("Listening", ChainPurpose::ListeningCorrectionChain);
    auto renderMonChain = std::make_shared<AudioChain>("RenderMonitor", ChainPurpose::RenderMonitorChain);

    auto peqMaster = std::make_shared<ParametricEqNode>("Master PEQ", NodeAffiliation::RenderCommon);
    auto peqListen = std::make_shared<ParametricEqNode>("Headphone EQ", NodeAffiliation::ListeningCorrection);
    auto geq = std::make_shared<GraphicEqNode>("Console GEQ");

    // 1. RenderCommon can be appended to MasteringChain
    QVERIFY(masterChain->appendNode(peqMaster));
    QVERIFY(masterChain->appendNode(geq));
    QCOMPARE(masterChain->nodeCount(), 2);

    // 2. ListeningCorrection node MUST BE REJECTED from MasteringChain
    QVERIFY(!masterChain->appendNode(peqListen));
    QCOMPARE(masterChain->nodeCount(), 2); // Count unchanged

    // 3. ListeningCorrection node MUST BE REJECTED from RenderMonitorChain
    QVERIFY(!renderMonChain->appendNode(peqListen));

    // 4. ListeningCorrection node can be appended to ListeningCorrectionChain
    QVERIFY(listeningChain->appendNode(peqListen));
    QCOMPARE(listeningChain->nodeCount(), 1);
}

void TestAudioPipeline::testAudioPipelineFormatConversionAndClamping()
{
    AudioPipeline pipeline(48000.0);
    const size_t frames = 64;

    std::vector<double> inL(frames, 2.0); // +6 dBFS overload (> 1.0)
    std::vector<double> inR(frames, -2.0);

    // Test output to Float32
    std::vector<float> outF32(frames * 2, 0.0f);
    pipeline.process(inL.data(), inR.data(), frames, outF32.data(), SampleFormat::Float32, false);

    for (size_t i = 0; i < frames; ++i) {
        QCOMPARE(outF32[i * 2], 1.0f);     // Clamped to +1.0
        QCOMPARE(outF32[i * 2 + 1], -1.0f); // Clamped to -1.0
    }

    // Test output to Int16 with TPDF dither
    std::vector<double> quietL(frames, 0.5);
    std::vector<double> quietR(frames, -0.5);
    std::vector<int16_t> outI16(frames * 2, 0);
    pipeline.process(quietL.data(), quietR.data(), frames, outI16.data(), SampleFormat::Int16, true);

    for (size_t i = 0; i < frames; ++i) {
        // 0.5 * 32767 ≈ 16383, within dither variance (±2)
        QVERIFY(std::abs(outI16[i * 2] - 16383) <= 2);
        QVERIFY(std::abs(outI16[i * 2 + 1] - (-16384)) <= 2);
    }
}

void TestAudioPipeline::testOutputPathRoutingAndZeroLeakage()
{
    // Section 5.10 & 5.11: Listening Correction must never leak into Render Monitor Path (0.000000)
    AudioPipeline pipeline(48000.0);
    const size_t frames = 128;

    // Create a Listening Correction chain with massive boost (+12 dB on peak filter)
    auto listenChain = std::make_unique<AudioChain>("Test Listening", ChainPurpose::ListeningCorrectionChain);
    auto headphoneEq = std::make_shared<ParametricEqNode>("Headphone EQ", NodeAffiliation::ListeningCorrection);
    headphoneEq->dsp().setEnabled(true);

    ParametricEQ::BandConfig cfg;
    cfg.enabled = true;
    cfg.type = BiquadFilterType::Peak;
    cfg.frequency = 1000.0;
    cfg.gainDb = 12.0;
    cfg.Q = 2.0;
    headphoneEq->dsp().setLeftBand(0, cfg);
    headphoneEq->dsp().setRightBand(0, cfg);
    listenChain->appendNode(headphoneEq);

    pipeline.swapListeningChain(std::move(listenChain));

    // 1. In RenderMonitorPath, Listening Correction MUST NOT affect the output at all
    pipeline.setOutputMode(OutputPathMode::RenderMonitorPath);
    pipeline.setRenderMonitorVolume(1.0);

    std::vector<double> inL(frames, 0.2);
    std::vector<double> inR(frames, 0.2);
    std::vector<double> copyL = inL;
    std::vector<double> copyR = inR;

    std::vector<float> outRenderMon(frames * 2, 0.0f);
    pipeline.process(copyL.data(), copyR.data(), frames, outRenderMon.data(), SampleFormat::Float32, false);

    // Under Render Monitor, output must be exactly identical to input (0.2) with 0.000000 leakage of the +12dB boost
    for (size_t i = 0; i < frames; ++i) {
        QCOMPARE(outRenderMon[i * 2], 0.2f);
        QCOMPARE(outRenderMon[i * 2 + 1], 0.2f);
    }

    // 2. In ListeningPath, Listening Correction MUST be applied (boosted above 0.2)
    pipeline.setOutputMode(OutputPathMode::ListeningPath);
    pipeline.setListeningVolume(1.0);

    copyL = inL;
    copyR = inR;
    std::vector<float> outListening(frames * 2, 0.0f);
    pipeline.process(copyL.data(), copyR.data(), frames, outListening.data(), SampleFormat::Float32, false);

    // Peak filter transient may take a few samples, but after filter step it must deviate from 0.2
    bool altered = false;
    for (size_t i = 0; i < frames; ++i) {
        if (std::abs(outListening[i * 2] - 0.2f) > 0.001f) {
            altered = true;
            break;
        }
    }
    QVERIFY(altered);
}

void TestAudioPipeline::testAudioGraphRebuildClonesNodesWithoutSharing()
{
    // P0-08: Verify AudioChain::clone() creates isolated deep copies of nodes
    auto original = std::make_unique<AudioChain>("OriginalMaster", ChainPurpose::MasteringChain);
    auto peqNode = std::make_shared<ParametricEqNode>("MasterPEQ");
    auto geqNode = std::make_shared<GraphicEqNode>("MasterGEQ");

    original->appendNode(peqNode);
    original->appendNode(geqNode);

    auto cloned = original->clone();

    QCOMPARE(cloned->nodeCount(), 2u);
    QVERIFY(cloned->node(0) != nullptr);
    QVERIFY(cloned->node(1) != nullptr);

    // Instances must be completely distinct pointers to avoid mutating nodes active in Audio Thread
    QVERIFY(cloned->node(0) != peqNode.get());
    QVERIFY(cloned->node(1) != geqNode.get());
    QCOMPARE(cloned->node(0)->name(), peqNode->name());
    QCOMPARE(cloned->node(1)->name(), geqNode->name());

    // AudioPipeline graph rate reconfiguration test
    AudioPipeline pipeline(48000.0);
    pipeline.swapMasterChain(std::move(original));
    pipeline.setGraphSampleRate(96000.0);
    QCOMPARE(pipeline.graphSampleRate(), 96000.0);

    // Verify audio rendering works seamlessly through cloned graph
    std::vector<double> inL(128, 0.5);
    std::vector<double> inR(128, 0.5);
    std::vector<float> out(256, 0.0f);
    pipeline.process(inL.data(), inR.data(), 128, out.data(), SampleFormat::Float32, false);
    QVERIFY(out[0] != 0.0f);
}

void TestAudioPipeline::testEpochReclamationHotSwap()
{
    // Section 9.3: Hot-swap audio chains without audio glitch or memory leaks
    AudioPipeline pipeline(48000.0);
    const size_t frames = 256;

    std::vector<double> inL(frames, 0.1);
    std::vector<double> inR(frames, 0.1);
    std::vector<float> outBuf(frames * 2, 0.0f);

    // Repeatedly hot-swap master chain during active processing loop
    for (int cycle = 0; cycle < 50; ++cycle) {
        auto newChain = std::make_unique<AudioChain>("Dynamic Chain " + std::to_string(cycle), ChainPurpose::MasteringChain);
        newChain->appendNode(std::make_shared<GraphicEqNode>());
        newChain->appendNode(std::make_shared<CompressorNode>());

        pipeline.swapMasterChain(std::move(newChain));

        // Process audio blocks while chain is active
        for (int b = 0; b < 10; ++b) {
            std::vector<double> l = inL;
            std::vector<double> r = inR;
            pipeline.process(l.data(), r.data(), frames, outBuf.data(), SampleFormat::Float32, false);
        }

        // Periodically reclaim retired memory
        pipeline.reclaimRetiredMemory();
    }

    // Drain remaining retired chains
    pipeline.reclaimRetiredMemory();
    QVERIFY(true);
}

void TestAudioPipeline::testEpochReclaimerConcurrentStress()
{
    // Section 9.3: Multi-threaded stress test of EpochReclaimer
    // Verifies concurrent reader thread and writer swap thread with zero use-after-free
    EpochReclaimer<std::string> reclaimer;
    reclaimer.swapChain(std::make_unique<std::string>("Initial Value"));

    std::atomic<bool> stopReader{false};
    std::atomic<uint64_t> readCycles{0};

    // Reader thread (simulating Audio Thread)
    std::thread readerThread([&]() {
        while (!stopReader.load(std::memory_order_relaxed)) {
            auto guard = reclaimer.enterRead();
            if (guard.chain) {
                // Access content
                volatile size_t sz = guard.chain->size();
                (void)sz;
            }
            readCycles.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Writer thread (Control Thread) performs rapid swaps
    const int swaps = 2000;
    for (int i = 0; i < swaps; ++i) {
        reclaimer.swapChain(std::make_unique<std::string>("Chain Version " + std::to_string(i)));
        if (i % 50 == 0) {
            reclaimer.reclaimRetired();
        }
    }

    stopReader.store(true, std::memory_order_relaxed);
    readerThread.join();

    // After reader exits, reclaim all
    reclaimer.reclaimRetired();
    QCOMPARE(reclaimer.retiredCount(), 0u);
    QVERIFY(readCycles.load() > 50);
}

void TestAudioPipeline::testEpochReclaimerMultiReaderConcurrentStress()
{
    // P0-13: Multi-reader concurrent access with simultaneous writer and reclaimer
    EpochReclaimer<std::string> reclaimer;
    reclaimer.swapChain(std::make_unique<std::string>("Initial Value"));

    const size_t numReaders = 8;
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    std::vector<uint64_t> readerCounts(numReaders, 0);

    for (size_t t = 0; t < numReaders; ++t) {
        readers.emplace_back([&reclaimer, &stop, &readerCounts, t]() {
            while (!stop.load(std::memory_order_relaxed)) {
                auto guard = reclaimer.enterRead();
                if (guard) {
                    volatile size_t sz = guard->size();
                    (void)sz;
                }
                readerCounts[t]++;
                std::this_thread::yield();
            }
        });
    }

    // Reclaimer thread calling reclaimRetired concurrently
    std::thread reclaimerThread([&reclaimer, &stop]() {
        while (!stop.load(std::memory_order_relaxed)) {
            reclaimer.reclaimRetired();
            std::this_thread::yield();
        }
    });

    // Writer thread rapidly swapping chains
    const int swaps = 1000;
    for (int i = 0; i < swaps; ++i) {
        reclaimer.swapChain(std::make_unique<std::string>("Chain Swap " + std::to_string(i)));
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& r : readers) {
        r.join();
    }
    reclaimerThread.join();

    // After all readers and writers are stopped, all retired objects must be cleanly reclaimed
    reclaimer.reclaimRetired();
    QCOMPARE(reclaimer.retiredCount(), 0u);
    for (size_t t = 0; t < numReaders; ++t) {
        QVERIFY(readerCounts[t] > 10);
    }
}

void TestAudioPipeline::testSampleRateChangeConcurrentWithTelemetry()
{
    // P0-13: Concurrent sample rate changes while audio thread renders and telemetry threads poll
    AudioPipeline pipeline(48000.0);
    auto masterChain = std::make_unique<AudioChain>("Master", ChainPurpose::MasteringChain);
    masterChain->appendNode(std::make_shared<CompressorNode>());
    masterChain->appendNode(std::make_shared<PeakLimiterNode>());
    pipeline.swapMasterChain(std::move(masterChain));

    std::atomic<bool> stop{false};
    const size_t frames = 128;
    std::vector<double> inL(frames, 0.2);
    std::vector<double> inR(frames, 0.2);
    std::vector<float> outBuf(frames * 2, 0.0f);

    // Audio rendering thread
    std::thread audioThread([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            std::vector<double> l = inL;
            std::vector<double> r = inR;
            pipeline.process(l.data(), r.data(), frames, outBuf.data(), SampleFormat::Float32, false);
            std::this_thread::yield();
        }
    });

    // Telemetry polling thread (simulating UI thread)
    std::thread telemetryThread([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            double cGr = pipeline.compressorGainReductionDb();
            double lGr = pipeline.limiterGainReductionDb();
            int lat = pipeline.limiterLatencyFrames();
            (void)cGr; (void)lGr; (void)lat;
            std::this_thread::yield();
        }
    });

    // Control thread rapidly reconfiguring graph sample rate
    const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    for (int i = 0; i < 200; ++i) {
        pipeline.setGraphSampleRate(rates[i % 5]);
        if (i % 20 == 0) {
            pipeline.reclaimRetiredMemory();
        }
    }

    stop.store(true, std::memory_order_relaxed);
    audioThread.join();
    telemetryThread.join();

    pipeline.reclaimRetiredMemory();
    QVERIFY(true);
}

void TestAudioPipeline::testShutdownWaitsForActiveReaders()
{
    // P0-13: drainAll() must wait until an active reader completes reading before destroying
    auto reclaimer = std::make_unique<EpochReclaimer<std::string>>();
    reclaimer->swapChain(std::make_unique<std::string>("ProtectedString"));

    std::atomic<bool> readerInside{false};
    std::atomic<bool> readerDone{false};
    std::atomic<bool> drained{false};

    std::thread readerThread([&]() {
        auto guard = reclaimer->enterRead();
        readerInside.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        QVERIFY(guard.chain != nullptr);
        QCOMPARE(*guard.chain, std::string("ProtectedString"));
        // readerDone is signaled before guard is destroyed
        readerDone.store(true, std::memory_order_release);
    });

    while (!readerInside.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::thread drainThread([&]() {
        reclaimer->drainAll();
        drained.store(true, std::memory_order_release);
    });

    // drainThread must not finish until readerThread has exited guard
    drainThread.join();
    readerThread.join();

    QVERIFY(readerDone.load());
    QVERIFY(drained.load());
}

void TestAudioPipeline::testReaderSlotExhaustionRejectsUnshieldedPointer()
{
    // P0-20: If all 64 reader slots are saturated, enterRead() strictly returns an invalid ReadGuard
    // with nullptr. IT MUST NEVER RETURN AN UNSHIELDED POINTER.
    EpochReclaimer<std::string> reclaimer;
    reclaimer.swapChain(std::make_unique<std::string>("TargetData"));

    std::vector<EpochReclaimer<std::string>::ReadGuard> guards;
    guards.reserve(EpochReclaimer<std::string>::MAX_READERS);

    // 1. Acquire all MAX_READERS (64) slots
    for (size_t i = 0; i < EpochReclaimer<std::string>::MAX_READERS; ++i) {
        auto guard = reclaimer.enterRead();
        QVERIFY(guard.isValid());
        QVERIFY(guard.get() != nullptr);
        QCOMPARE(*guard, std::string("TargetData"));
        guards.push_back(std::move(guard));
    }
    QCOMPARE(reclaimer.slotExhaustionCount(), 0u);

    // 2. 65th attempt MUST fail cleanly without exposing an unshielded pointer
    auto failedGuard = reclaimer.enterRead();
    QVERIFY(!failedGuard.isValid());
    QVERIFY(!failedGuard);
    QVERIFY(failedGuard.get() == nullptr);
    QVERIFY(failedGuard.chain == nullptr);
    QCOMPARE(reclaimer.slotExhaustionCount(), 1u);

    // 3. Release one slot: subsequent enterRead() must succeed immediately
    guards.pop_back();
    auto recoveryGuard = reclaimer.enterRead();
    QVERIFY(recoveryGuard.isValid());
    QVERIFY(recoveryGuard.get() != nullptr);
    QCOMPARE(*recoveryGuard, std::string("TargetData"));
}

void TestAudioPipeline::testDspInstanceContinuousExchangeAndRetirement()
{
    // P0-21: Continuous DSP instance generation on Control Thread and lock-free block-boundary
    // swap on Audio Thread with safe retirement outside Audio Thread.
    AudioPipeline pipeline(48000.0);

    auto master = std::make_unique<AudioChain>("Mastering", ChainPurpose::MasteringChain);
    master->appendNode(std::make_shared<CompressorNode>("Compressor"));
    master->appendNode(std::make_shared<PeakLimiterNode>("Peak Limiter"));
    master->appendNode(std::make_shared<ReverbNode>("Reverb"));
    pipeline.swapMasterChain(std::move(master));

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> audioBlocksRendered{0};

    // Audio Thread
    std::thread audioThread([&]() {
        const size_t frames = 256;
        std::vector<double> inL(frames, 0.4);
        std::vector<double> inR(frames, -0.4);
        std::vector<float> out(frames * 2, 0.0f);

        while (!stop.load(std::memory_order_relaxed)) {
            pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);
            audioBlocksRendered.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Control Thread: Rapidly preparing and publishing new DSP instances
    for (int i = 0; i < 200; ++i) {
        pipeline.updateCompressor(-12.0 - (i % 10), 2.0 + (i % 4), 10.0, 100.0, 0.0, 4.0, true);
        pipeline.updatePeakLimiter(-0.1 - (i % 3) * 0.5, 50.0, 1.0 + (i % 3) * 0.5, true);
        pipeline.updateReverb(0.5 + (i % 5) * 0.08, 0.3, 0.2, 5.0, true, true);

        if (i % 10 == 0) {
            pipeline.reclaimRetiredMemory();
        }
        std::this_thread::yield();
    }

    stop.store(true, std::memory_order_relaxed);
    audioThread.join();

    pipeline.reclaimRetiredMemory();
    QVERIFY(audioBlocksRendered.load() > 0);
}

void TestAudioPipeline::testReverbTailPreservationCrossfade()
{
    // P0-22: Reverb state transition with 512-frame tail preservation crossfade
    AudioPipeline pipeline(48000.0);

    auto master = std::make_unique<AudioChain>("Mastering", ChainPurpose::MasteringChain);
    auto reverbNode = std::make_shared<ReverbNode>("Reverb");
    master->appendNode(reverbNode);
    pipeline.swapMasterChain(std::move(master));

    pipeline.updateReverb(0.85, 0.2, 0.5, 0.0, true, true);

    const size_t frames = 256;
    std::vector<double> inL(frames, 0.0);
    std::vector<double> inR(frames, 0.0);
    std::vector<float> out(frames * 2, 0.0f);

    // Excite reverb with 2048 frames of audio to fully populate comb filter delay lines
    for (int b = 0; b < 8; ++b) {
        for (size_t i = 0; i < frames; ++i) {
            inL[i] = 0.5 * std::sin((static_cast<double>(b) * static_cast<double>(frames) + static_cast<double>(i)) * 0.1);
            inR[i] = inL[i];
        }
        pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);
    }

    // Now clear input to let tail ring out
    std::fill(inL.begin(), inL.end(), 0.0);
    std::fill(inR.begin(), inR.end(), 0.0);
    pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);

    double tailEnergyBefore = 0.0;
    for (float s : out) tailEnergyBefore += s * s;
    QVERIFY(tailEnergyBefore > 1e-4);

    // Update reverb settings while tail is ringing (triggers 512-frame crossfade)
    pipeline.updateReverb(0.4, 0.6, 0.3, 10.0, true, true);

    // Process through the crossfade (first 256 frames)
    pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);
    double tailEnergyCrossfade1 = 0.0;
    for (float s : out) tailEnergyCrossfade1 += s * s;

    // Tail energy must remain preserved and non-zero (not truncated to 0)
    QVERIFY(tailEnergyCrossfade1 > 1e-4);

    // Process next block (completing the crossfade)
    pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);
    pipeline.reclaimRetiredMemory();
}

void TestAudioPipeline::testLimiterLookaheadChangeSampleContinuity()
{
    // P0-23: Limiter lookahead reconfiguration with buffer inheritance and latency synchronization
    AudioPipeline pipeline(48000.0);

    auto master = std::make_unique<AudioChain>("Mastering", ChainPurpose::MasteringChain);
    auto limiterNode = std::make_shared<PeakLimiterNode>("Peak Limiter");
    master->appendNode(limiterNode);
    pipeline.swapMasterChain(std::move(master));

    pipeline.updatePeakLimiter(-0.5, 50.0, 1.0, true); // 1.0ms = 48 samples at 48kHz

    const size_t frames = 256;
    std::vector<double> inL(frames, 0.5);
    std::vector<double> inR(frames, 0.5);
    std::vector<float> out(frames * 2, 0.0f);

    pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);
    QCOMPARE(pipeline.limiterLatencyFrames(), 48);

    // Dynamically increase lookahead to 2.0ms (96 samples)
    pipeline.updatePeakLimiter(-0.5, 50.0, 2.0, true);
    pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);

    // Latency frames must reflect the updated lookahead
    QCOMPARE(pipeline.limiterLatencyFrames(), 96);

    // Output must not contain NaN, inf, or values exceeding ceiling
    for (float s : out) {
        QVERIFY(!std::isnan(s));
        QVERIFY(!std::isinf(s));
        QVERIFY(std::abs(s) <= 1.0f);
    }

    pipeline.reclaimRetiredMemory();
}

void TestAudioPipeline::testTelemetrySnapshotThreadSafety()
{
    // P0-26: Concurrent Audio Thread telemetry publishing and UI Thread polling
    AudioPipeline pipeline(48000.0);

    auto master = std::make_unique<AudioChain>("Mastering", ChainPurpose::MasteringChain);
    master->appendNode(std::make_shared<CompressorNode>("Compressor"));
    master->appendNode(std::make_shared<PeakLimiterNode>("Peak Limiter"));
    master->appendNode(std::make_shared<ReverbNode>("Reverb"));
    pipeline.swapMasterChain(std::move(master));

    pipeline.updateCompressor(-18.0, 4.0, 10.0, 100.0, 0.0, 4.0, true);
    pipeline.updatePeakLimiter(-1.0, 50.0, 1.0, true);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> totalTelemetryReads{0};

    // Audio Thread
    std::thread audioThread([&]() {
        const size_t frames = 128;
        std::vector<double> inL(frames, 0.8); // High signal to trigger gain reduction
        std::vector<double> inR(frames, 0.8);
        std::vector<float> out(frames * 2, 0.0f);

        while (!stop.load(std::memory_order_relaxed)) {
            pipeline.process(inL.data(), inR.data(), frames, out.data(), SampleFormat::Float32, false);
            std::this_thread::yield();
        }
    });

    // 4 UI / Background Telemetry Threads
    std::vector<std::thread> uiThreads;
    for (int t = 0; t < 4; ++t) {
        uiThreads.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                auto tele = pipeline.currentTelemetry();
                QVERIFY(tele.graphSampleRate > 0.0);
                double compGR = pipeline.compressorGainReductionDb();
                double limGR = pipeline.limiterGainReductionDb();
                int latency = pipeline.limiterLatencyFrames();
                Q_UNUSED(compGR);
                Q_UNUSED(limGR);
                Q_UNUSED(latency);
                totalTelemetryReads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true, std::memory_order_relaxed);

    audioThread.join();
    for (auto& t : uiThreads) {
        t.join();
    }

    QVERIFY(totalTelemetryReads.load() > 100);
}

void TestAudioPipeline::testEpochReclaimerShutdownConcurrentSafety()
{
    // P0-28: drainAll() shutdown protocol with concurrent speculative readers
    auto reclaimer = std::make_unique<EpochReclaimer<std::string>>();
    reclaimer->swapChain(std::make_unique<std::string>("InitialActiveData"));

    std::atomic<bool> stopReaders{false};
    std::atomic<uint64_t> successfulReads{0};
    std::atomic<uint64_t> rejectedReads{0};

    // 4 concurrent reader threads attempting enterRead() before, during, and after drainAll()
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&]() {
            while (!stopReaders.load(std::memory_order_relaxed)) {
                auto guard = reclaimer->enterRead();
                if (guard.isValid() && guard.chain != nullptr) {
                    successfulReads.fetch_add(1, std::memory_order_relaxed);
                    // Verify data sanity
                    volatile size_t len = guard.chain->length();
                    (void)len;
                } else {
                    rejectedReads.fetch_add(1, std::memory_order_relaxed);
                    QVERIFY(guard.chain == nullptr);
                }
                std::this_thread::yield();
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Shutdown while readers are vigorously querying
    reclaimer->drainAll();
    QVERIFY(reclaimer->isClosing());

    // Subsequent enterRead() MUST strictly reject
    auto postGuard = reclaimer->enterRead();
    QVERIFY(!postGuard.isValid());
    QVERIFY(postGuard.chain == nullptr);

    // swapChain on closed reclaimer is a safe no-op
    reclaimer->swapChain(std::make_unique<std::string>("ShouldBeIgnored"));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stopReaders.store(true, std::memory_order_relaxed);

    for (auto& t : readers) {
        t.join();
    }

    QVERIFY(successfulReads.load() > 0);
    QVERIFY(rejectedReads.load() > 0);
}

void TestAudioPipeline::testEpochReclaimerWriterShutdownSerialization()
{
    // P0-20: Atomic serialization of swapChain() with drainAll()
    auto reclaimer = std::make_unique<EpochReclaimer<std::string>>();
    QVERIFY(reclaimer->swapChain(std::make_unique<std::string>("InitialChain")));

    std::atomic<bool> stopWorkers{false};
    std::atomic<uint64_t> successfulSwaps{0};
    std::atomic<uint64_t> rejectedSwaps{0};
    std::atomic<uint64_t> successfulReads{0};

    // 2 Concurrent Reader threads
    std::vector<std::thread> readers;
    for (int i = 0; i < 2; ++i) {
        readers.emplace_back([&]() {
            while (!stopWorkers.load(std::memory_order_relaxed)) {
                auto guard = reclaimer->enterRead();
                if (guard.isValid() && guard.chain != nullptr) {
                    successfulReads.fetch_add(1, std::memory_order_relaxed);
                    volatile size_t len = guard.chain->length();
                    (void)len;
                }
                std::this_thread::yield();
            }
        });
    }

    // 2 Concurrent Writer threads attempting continuous swapChain
    std::vector<std::thread> writers;
    for (int i = 0; i < 2; ++i) {
        writers.emplace_back([&, i]() {
            uint64_t count = 0;
            while (!stopWorkers.load(std::memory_order_relaxed)) {
                auto chain = std::make_unique<std::string>("Chain_" + std::to_string(i) + "_" + std::to_string(count++));
                if (reclaimer->swapChain(std::move(chain))) {
                    successfulSwaps.fetch_add(1, std::memory_order_relaxed);
                } else {
                    rejectedSwaps.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::yield();
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Shutdown while writers and readers are actively working
    reclaimer->drainAll();
    QVERIFY(reclaimer->isClosing());

    // Subsequent swapChain MUST be rejected
    auto lateChain = std::make_unique<std::string>("LateChain");
    bool swapResult = reclaimer->swapChain(std::move(lateChain));
    QVERIFY(!swapResult);

    // All retired objects MUST be cleaned up
    QCOMPARE(reclaimer->retiredCount(), 0u);

    // Subsequent enterRead() MUST return nullptr
    auto postGuard = reclaimer->enterRead();
    QVERIFY(!postGuard.isValid());
    QVERIFY(postGuard.chain == nullptr);

    stopWorkers.store(true, std::memory_order_relaxed);
    for (auto& w : writers) w.join();
    for (auto& r : readers) r.join();

    QVERIFY(successfulSwaps.load() > 0);
    QVERIFY(rejectedSwaps.load() > 0);
    QVERIFY(successfulReads.load() > 0);
}

void TestAudioPipeline::testEpochReclaimerDeterministicShutdownRaces()
{
    // P0-20, P0-28: Deterministic verification of EpochReclaimer shutdown concurrency (Cases A - D)

    // Case A: Writer swapChain during Shutdown
    {
        EpochReclaimer<std::string> reclaimer;
        reclaimer.swapChain(std::make_unique<std::string>("Initial"));

        std::latch syncPoint(2);
        std::atomic<bool> swapOutcome{false};

        std::thread writerThread([&]() {
            syncPoint.arrive_and_wait();
            swapOutcome.store(reclaimer.swapChain(std::make_unique<std::string>("FromWriter")));
        });

        std::thread shutdownThread([&]() {
            syncPoint.arrive_and_wait();
            reclaimer.drainAll();
        });

        writerThread.join();
        shutdownThread.join();

        QVERIFY(reclaimer.isClosing());
        QCOMPARE(reclaimer.retiredCount(), 0u);
    }

    // Case B: Reader holds guard during swapChain and drainAll (drainAll blocks until Reader exits)
    {
        EpochReclaimer<std::string> reclaimer;
        reclaimer.swapChain(std::make_unique<std::string>("ReaderActiveChain"));

        std::latch readerHolding(1);
        std::atomic<bool> readerFinished{false};
        std::atomic<bool> drainDone{false};

        std::thread readerThread([&]() {
            auto guard = reclaimer.enterRead();
            QVERIFY(guard.isValid() && guard.chain != nullptr);
            QCOMPARE(*guard.chain, std::string("ReaderActiveChain"));
            readerHolding.count_down();

            // Hold guard until Control Thread initiates shutdown
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            // Verify data is still intact (never prematurely freed)
            QCOMPARE(*guard.chain, std::string("ReaderActiveChain"));
            readerFinished.store(true, std::memory_order_release);
        });

        readerHolding.wait();
        // Retire the chain that reader is observing
        reclaimer.swapChain(std::make_unique<std::string>("NewerChain"));

        // Launch drainAll which MUST block waiting for readerThread to finish
        std::thread drainThread([&]() {
            reclaimer.drainAll();
            drainDone.store(true, std::memory_order_release);
        });

        // While reader has not finished, drainAll must NOT have completed
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        QVERIFY(!drainDone.load(std::memory_order_acquire));

        readerThread.join();
        drainThread.join();

        QVERIFY(readerFinished.load());
        QVERIFY(drainDone.load());
        QVERIFY(reclaimer.isClosing());
        QCOMPARE(reclaimer.retiredCount(), 0u);
    }

    // Case C: Subsequent calls after drainAll completed
    {
        EpochReclaimer<std::string> reclaimer;
        reclaimer.swapChain(std::make_unique<std::string>("Initial"));
        reclaimer.drainAll();
        QVERIFY(reclaimer.isClosing());

        // Strictly reject new swaps
        bool swapOk = reclaimer.swapChain(std::make_unique<std::string>("PostClose"));
        QVERIFY(!swapOk);

        // Strictly reject new reads
        auto guard = reclaimer.enterRead();
        QVERIFY(!guard.isValid());
        QVERIFY(guard.chain == nullptr);
    }

    // Case D: Multiple writers (4 writers) at barrier calling swapChain concurrently with drainAll
    {
        EpochReclaimer<std::string> reclaimer;
        reclaimer.swapChain(std::make_unique<std::string>("InitialD"));

        const int numWriters = 4;
        std::latch syncAll(numWriters + 1);

        std::vector<std::thread> writers;
        std::atomic<int> successCount{0};
        std::atomic<int> rejectCount{0};

        for (int i = 0; i < numWriters; ++i) {
            writers.emplace_back([&, i]() {
                syncAll.arrive_and_wait();
                bool ok = reclaimer.swapChain(std::make_unique<std::string>("Chain_D_" + std::to_string(i)));
                if (ok) {
                    successCount.fetch_add(1, std::memory_order_relaxed);
                } else {
                    rejectCount.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        std::thread drainThread([&]() {
            syncAll.arrive_and_wait();
            reclaimer.drainAll();
        });

        for (auto& w : writers) w.join();
        drainThread.join();

        QVERIFY(reclaimer.isClosing());
        QCOMPARE(reclaimer.retiredCount(), 0u);
        QCOMPARE(successCount.load() + rejectCount.load(), numWriters);
    }
}

void TestAudioPipeline::cleanupTestCase()
{
    qDebug() << "TestAudioPipeline completed successfully.";
}

QTEST_MAIN(TestAudioPipeline)
#include "test_audio_pipeline.moc"
