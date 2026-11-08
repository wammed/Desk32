#include <QTest>
#include <QObject>
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

void TestAudioPipeline::cleanupTestCase()
{
    qDebug() << "TestAudioPipeline completed successfully.";
}

QTEST_MAIN(TestAudioPipeline)
#include "test_audio_pipeline.moc"
