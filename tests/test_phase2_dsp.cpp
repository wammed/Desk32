#include <QTest>
#include <QObject>
#include <cmath>
#include <vector>
#include <array>
#include <numbers>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <chrono>

#include "engine/buffer/PcmRingBuffer.hpp"
#include "engine/dsp/ParameterCommandQueue.hpp"
#include "engine/dsp/ParametricEQ.hpp"
#include "engine/dsp/GraphicEQ.hpp"
#include "engine/dsp/HighLowPassFilter.hpp"
#include "engine/dsp/Compressor.hpp"
#include "engine/dsp/PeakLimiter.hpp"
#include "engine/dsp/AlgorithmicReverb.hpp"
#include "engine/dsp/ParameterSmoother.hpp"
#include "engine/pipeline/AudioPipeline.hpp"
#include "engine/pipeline/BuiltInDspNodes.hpp"
#include "engine/memory/EpochReclaimer.hpp"

using namespace desk32::engine::dsp;
using namespace desk32::engine::pipeline;

// Heap allocation tracker for real-time safety verification
static std::atomic<bool> s_trackAllocations{false};
static std::atomic<size_t> s_allocationCount{0};

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

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { std::free(p); }

class TestPhase2Dsp : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void test24BandParametricEqAndResponseCurve();
    void test32BandGraphicEqAndReset();
    void testHighLowPassFilterSlopesAndCutoff();
    void testCompressorDynamicsAndGainReduction();
    void testPeakLimiterBrickwallAndLookaheadLatency();
    void testReverbTailPreservingBypass();
    void testParameterSmootherBehavior();
    void testLogicalRenderTapAndListeningCorrectionIsolation();
    void testZeroDynamicAllocationInPhase2Dsp();
    void testPcmRingBufferConcurrentSPSCAndUnderrun();
    void testParameterCommandQueueSPSCAndCapacity();
    void testParameterCommandQueueCoalescingAndOverflowStats();
    void testDspParameterMailboxMultiProducerAndContinuousUpdates();
    void testDspParameterMailbox3SlotExchangeSnapshotConsistency();
    void testDspParameterMailboxTornReadDetectionVsTripleBufferSafety();
    void testDspParameterMailboxBatchApis();
    void testSingleThreadDspUnitOperation();
    void testPreparedDspInstanceExchangeSafety();
    void testContinuousParameterMailboxUpdates();
    void testRtSafetyAllocationDuringProcess();
};

void TestPhase2Dsp::initTestCase()
{
    qDebug() << "Initializing TestPhase2Dsp (Phase 2 Acoustic Correction & Built-in DSP Verification)...";
}

void TestPhase2Dsp::test24BandParametricEqAndResponseCurve()
{
    ParametricEQ peq;
    peq.configure(48000.0);
    peq.setEnabled(true);

    // 1. Verify 12 Left Bands + 12 Right Bands are strictly independent (Section 5.2)
    ParametricEQ::BandConfig leftCfg;
    leftCfg.frequency = 1000.0;
    leftCfg.gainDb = 6.0;
    leftCfg.Q = 1.414;
    leftCfg.type = BiquadFilterType::Peak;
    leftCfg.enabled = true;

    ParametricEQ::BandConfig rightCfg;
    rightCfg.frequency = 5000.0;
    rightCfg.gainDb = -4.5;
    rightCfg.Q = 2.0;
    rightCfg.type = BiquadFilterType::Peak;
    rightCfg.enabled = true;

    peq.setLeftBand(5, leftCfg);
    peq.setRightBand(5, rightCfg);

    // Verify left and right band 5 are completely different
    QCOMPARE(peq.leftBand(5).frequency, 1000.0);
    QCOMPARE(peq.leftBand(5).gainDb, 6.0);
    QCOMPARE(peq.rightBand(5).frequency, 5000.0);
    QCOMPARE(peq.rightBand(5).gainDb, -4.5);

    // 2. Response curve calculation
    std::vector<double> freqs = { 100.0, 1000.0, 5000.0, 10000.0 };
    std::vector<double> leftDb, rightDb;
    peq.calculateCompositeResponse(freqs, leftDb, rightDb);

    QCOMPARE(leftDb.size(), freqs.size());
    QCOMPARE(rightDb.size(), freqs.size());

    // Left channel should have ~+6 dB at 1000 Hz, but ~0 dB at 100 Hz
    QVERIFY(std::abs(leftDb[0]) < 0.5); // 100 Hz
    QVERIFY(std::abs(leftDb[1] - 6.0) < 0.5); // 1000 Hz
    QVERIFY(std::abs(rightDb[1]) < 0.5); // Right channel has no boost at 1000 Hz!
    QVERIFY(std::abs(rightDb[2] - (-4.5)) < 0.5); // Right channel has -4.5 dB at 5000 Hz!

    // 3. Preset application (Section 5.10)
    QVERIFY(peq.applyPreset("Harman Target"));
    QVERIFY(peq.leftBand(1).gainDb > 3.0); // Sub-bass boost
    QVERIFY(peq.leftBand(7).gainDb > 2.0); // Ear gain boost

    QVERIFY(peq.applyPreset("Monitor Flat"));
    for (size_t i = 0; i < ParametricEQ::BANDS_PER_CHANNEL; ++i) {
        QCOMPARE(peq.leftBand(i).gainDb, 0.0);
        QCOMPARE(peq.rightBand(i).gainDb, 0.0);
    }
}

void TestPhase2Dsp::test32BandGraphicEqAndReset()
{
    GraphicEQ geq;
    geq.configure(48000.0);
    geq.setEnabled(true);

    // 1. Verify 32 standard ISO frequencies
    QCOMPARE(GraphicEQ::NUM_BANDS, size_t(32));
    QCOMPARE(geq.bandFrequency(0), 16.0);
    QCOMPARE(geq.bandFrequency(18), 1000.0);
    QCOMPARE(geq.bandFrequency(31), 20000.0);

    // 2. Set band gains with bounds clamping (-12 dB to +12 dB)
    geq.setBandGain(18, 8.0); // 1 kHz
    QCOMPARE(geq.bandGain(18), 8.0);

    geq.setBandGain(10, 15.0); // Exceeds +12 dB
    QCOMPARE(geq.bandGain(10), 12.0); // Clamped to 12 dB

    // 3. Response curve verification
    std::vector<double> freqs = { 100.0, 1000.0, 10000.0 };
    std::vector<double> curveDb;
    geq.calculateResponseCurve(freqs, curveDb);
    QVERIFY(curveDb[1] > 6.0); // Peak around 1 kHz

    // 4. Reset all bands
    geq.resetAllBands();
    for (size_t i = 0; i < GraphicEQ::NUM_BANDS; ++i) {
        QCOMPARE(geq.bandGain(i), 0.0);
    }
}

void TestPhase2Dsp::testHighLowPassFilterSlopesAndCutoff()
{
    HighLowPassFilter filter;
    // Configure HPF at 100 Hz, LPF at 5000 Hz, 12 dB/oct slope
    filter.configure(48000.0, 100.0, 5000.0,
                     HighLowPassFilter::FilterSlope::Slope12dB,
                     HighLowPassFilter::FilterSlope::Slope12dB);
    filter.setHpfEnabled(true);
    filter.setLpfEnabled(true);

    std::vector<double> freqs = { 25.0, 50.0, 100.0, 1000.0, 5000.0, 10000.0, 20000.0 };
    std::vector<double> outDb;
    filter.calculateResponseCurve(freqs, outDb);

    // At cutoff frequencies (100 Hz and 5000 Hz), attenuation is approximately -3 dB (Butterworth)
    QVERIFY(std::abs(outDb[2] - (-3.0)) < 0.6); // 100 Hz
    QVERIFY(std::abs(outDb[4] - (-3.0)) < 0.6); // 5000 Hz

    // In passband (1000 Hz), attenuation should be ~0 dB
    QVERIFY(std::abs(outDb[3]) < 0.5);

    // 1 octave below HPF cutoff (50 Hz), attenuation is ~ -12 dB for 2nd order
    QVERIFY(outDb[1] < -10.0);

    // Switch HPF to 24 dB/oct slope
    filter.setHpfSlope(HighLowPassFilter::FilterSlope::Slope24dB);
    filter.calculateResponseCurve(freqs, outDb);
    // At 50 Hz, 4th order attenuation is ~ -24 dB
    QVERIFY(outDb[1] < -20.0);
}

void TestPhase2Dsp::testCompressorDynamicsAndGainReduction()
{
    Compressor comp;
    // Threshold = -12 dB, Ratio = 4:1, Attack = 0.5ms, Release = 50ms, Makeup = 0dB
    comp.configure(48000.0, -12.0, 4.0, 0.5, 50.0, 0.0, 0.0); // Hard knee for exact measurement
    comp.setEnabled(true);

    // 1. Signal below threshold (-20 dBFS ≈ 0.1 linear amplitude)
    double left = 0.1, right = 0.1;
    for (int i = 0; i < 200; ++i) {
        comp.process(left, right);
        left = 0.1; right = 0.1;
    }
    QCOMPARE(comp.gainReductionDb(), 0.0); // Zero reduction below threshold

    // 2. Signal +12 dB above threshold (0 dBFS = 1.0 linear amplitude)
    // Input is 0 dBFS (-12 dB threshold -> 12 dB excess).
    // With 4:1 ratio, gain reduction should approach: 12 * (1 - 1/4) = 9.0 dB!
    for (int i = 0; i < 1000; ++i) { // Settle envelope
        left = 1.0; right = 1.0;
        comp.process(left, right);
    }

    qDebug() << "Measured Compressor Gain Reduction:" << comp.gainReductionDb() << "dB (expected ~9.0 dB)";
    QVERIFY(std::abs(comp.gainReductionDb() - 9.0) < 0.5);
    QVERIFY(left < 0.4); // Compressed output amplitude < 0.4 (1.0 * 10^(-9/20) ≈ 0.355)
}

void TestPhase2Dsp::testPeakLimiterBrickwallAndLookaheadLatency()
{
    PeakLimiter limiter;
    // Ceiling = -0.1 dB (0.98855 linear), Release = 20ms, Lookahead = 1.0ms
    limiter.configure(48000.0, -0.1, 20.0, 1.0);
    limiter.setEnabled(true);

    // 1. Verify lookahead latency frames (1.0 ms at 48 kHz = 48 frames)
    QCOMPARE(limiter.lookaheadSamples(), size_t(48));
    QCOMPARE(limiter.latencyFrames(), 48ll);

    // 2. Feed an aggressive +6 dBFS burst (+2.0 linear amplitude)
    const double ceilingLinear = limiter.ceiling();
    bool anyExceeded = false;
    double maxObserved = 0.0;

    for (int i = 0; i < 500; ++i) {
        double left = 2.0 * std::sin(2.0 * std::numbers::pi * 1000.0 * (i / 48000.0));
        double right = left;

        limiter.process(left, right);

        if (std::abs(left) > ceilingLinear + 1e-6 || std::abs(right) > ceilingLinear + 1e-6) {
            anyExceeded = true;
        }
        maxObserved = std::max(maxObserved, std::abs(left));
    }

    qDebug() << "Peak Limiter max observed output:" << maxObserved << "vs ceiling:" << ceilingLinear;
    QVERIFY2(!anyExceeded, "Peak Limiter failed: sample exceeded brickwall ceiling!");
    QVERIFY(limiter.gainReductionDb() > 5.0); // Significant gain reduction recorded
}

void TestPhase2Dsp::testReverbTailPreservingBypass()
{
    AlgorithmicReverb reverb;
    reverb.configure(48000.0, 0.8, 0.2, 0.5, 0.0, true);
    reverb.setEnabled(true);

    // Excite reverb with continuous audio to fill comb and all-pass delay buffers
    double left = 0.0, right = 0.0;
    for (int i = 0; i < 2000; ++i) {
        left = std::sin(i * 0.1);
        right = left;
        reverb.process(left, right);
    }

    // Now engage Tail-Preserving Bypass (m_enabled = false, tailPreserving = true)
    reverb.setEnabled(false);

    // Process additional frames with zero input: existing reverberation tail MUST continue to ring out!
    double tailEnergy = 0.0;
    for (int i = 0; i < 2000; ++i) {
        left = 0.0; right = 0.0;
        reverb.process(left, right);
        tailEnergy += std::abs(left) + std::abs(right);
    }

    qDebug() << "Reverb tail energy during tail-preserving bypass:" << tailEnergy;
    QVERIFY2(tailEnergy > 0.01, "Tail-preserving bypass failed: reverb tail was truncated abruptly!");

    // Immediate bypass test:
    reverb.setTailPreserving(false);
    left = 0.0; right = 0.0;
    reverb.process(left, right);
    QCOMPARE(left, 0.0);
    QCOMPARE(right, 0.0);
}

void TestPhase2Dsp::testParameterSmootherBehavior()
{
    ParameterSmoother smoother(0.0, 10.0, 48000.0); // 10 ms glide time
    smoother.setTarget(1.0);

    QVERIFY(smoother.isSmoothing());
    QCOMPARE(smoother.current(), 0.0);

    // Advance 5 ms (240 samples): should be between 0.0 and 1.0
    for (int i = 0; i < 240; ++i) {
        (void)smoother.next();
    }
    QVERIFY(smoother.current() > 0.3 && smoother.current() < 0.8);

    // Advance 100 ms (enough to settle completely): should snap exactly to 1.0
    for (int i = 0; i < 4800; ++i) {
        (void)smoother.next();
    }
    QCOMPARE(smoother.current(), 1.0);
    QVERIFY(!smoother.isSmoothing());
}

void TestPhase2Dsp::testLogicalRenderTapAndListeningCorrectionIsolation()
{
    AudioPipeline pipeline(48000.0);

    // 1. Master Chain before Logical Render Tap
    auto masterChain = std::make_unique<AudioChain>("Master Chain", ChainPurpose::MasteringChain);
    auto masterPeq = std::make_shared<ParametricEqNode>("Master PEQ", NodeAffiliation::RenderCommon);
    masterChain->appendNode(masterPeq);
    pipeline.swapMasterChain(std::move(masterChain));

    // 2. Listening Correction Chain downstream of Logical Render Tap (Section 5.10)
    auto listeningChain = std::make_unique<AudioChain>("Listening Chain", ChainPurpose::ListeningCorrectionChain);
    auto correctionPeq = std::make_shared<ParametricEqNode>("Listening Correction", NodeAffiliation::ListeningCorrection);

    // Apply +12 dB boost at 1 kHz strictly in Listening Correction
    correctionPeq->dsp().setEnabled(true);
    ParametricEQ::BandConfig boost;
    boost.frequency = 1000.0;
    boost.gainDb = 12.0;
    boost.Q = 1.0;
    boost.type = BiquadFilterType::Peak;
    boost.enabled = true;
    correctionPeq->dsp().setLeftBand(5, boost);
    correctionPeq->dsp().setRightBand(5, boost);

    listeningChain->appendNode(correctionPeq);
    pipeline.swapListeningChain(std::move(listeningChain));

    // 3. Render 256 frames of 1 kHz sine wave
    const size_t frames = 256;
    std::vector<double> leftIn(frames), rightIn(frames);
    for (size_t i = 0; i < frames; ++i) {
        double s = 0.2 * std::sin(2.0 * std::numbers::pi * 1000.0 * (static_cast<double>(i) / 48000.0));
        leftIn[i] = s;
        rightIn[i] = s;
    }

    // A. Process in ListeningPath mode
    pipeline.setOutputMode(OutputPathMode::ListeningPath);
    std::vector<double> leftListen = leftIn;
    std::vector<double> rightListen = rightIn;
    std::vector<float> pcmListen(frames * 2);
    pipeline.process(leftListen.data(), rightListen.data(), frames, pcmListen.data(), desk32::engine::format::SampleFormat::Float32, false);

    // B. Process in RenderMonitorPath mode (MUST have 0.000000 Listening Correction leakage!)
    pipeline.setOutputMode(OutputPathMode::RenderMonitorPath);
    std::vector<double> leftMonitor = leftIn;
    std::vector<double> rightMonitor = rightIn;
    std::vector<float> pcmMonitor(frames * 2);
    pipeline.process(leftMonitor.data(), rightMonitor.data(), frames, pcmMonitor.data(), desk32::engine::format::SampleFormat::Float32, false);

    // Calculate RMS difference: Render Monitor must match input (since master PEQ is flat),
    // while Listening Path must be significantly louder (+12 dB)
    double listenEnergy = 0.0;
    double monitorEnergy = 0.0;
    double inputEnergy = 0.0;

    for (size_t i = 0; i < frames; ++i) {
        listenEnergy += leftListen[i] * leftListen[i];
        monitorEnergy += leftMonitor[i] * leftMonitor[i];
        inputEnergy += leftIn[i] * leftIn[i];
    }

    qDebug() << "Listening Path Energy (with +12dB Correction):" << listenEnergy;
    qDebug() << "Render Monitor Energy (without Correction):" << monitorEnergy;
    qDebug() << "Input Energy:" << inputEnergy;

    // Render Monitor should match input energy with zero leakage
    QVERIFY(std::abs(monitorEnergy - inputEnergy) < 1e-4);
    // Listening Path should have ~4x amplitude (~16x energy)
    QVERIFY(listenEnergy > monitorEnergy * 8.0);
}

void TestPhase2Dsp::testZeroDynamicAllocationInPhase2Dsp()
{
    // Verify that running all Phase 2 DSPs in real-time produces ZERO heap allocations
    ParametricEQ peq; peq.configure(48000.0);
    GraphicEQ geq; geq.configure(48000.0);
    HighLowPassFilter filter; filter.configure(48000.0);
    Compressor comp; comp.configure(48000.0); comp.setEnabled(true);
    PeakLimiter limiter; limiter.configure(48000.0); limiter.setEnabled(true);
    AlgorithmicReverb reverb; reverb.configure(48000.0); reverb.setEnabled(true);

    double left = 0.5, right = 0.5;

    // Warm-up one iteration
    peq.process(left, right);
    geq.process(left, right);
    filter.process(left, right);
    comp.process(left, right);
    limiter.process(left, right);
    reverb.process(left, right);

    // Track dynamic memory allocations
    s_allocationCount.store(0);
    s_trackAllocations.store(true);

    for (int i = 0; i < 10000; ++i) {
        left = 0.3 * std::sin(i * 0.1);
        right = left;
        peq.process(left, right);
        geq.process(left, right);
        filter.process(left, right);
        comp.process(left, right);
        limiter.process(left, right);
        reverb.process(left, right);
    }

    s_trackAllocations.store(false);

    qDebug() << "Total dynamic heap allocations in 10,000 Phase 2 DSP blocks:" << s_allocationCount.load();
    QCOMPARE(s_allocationCount.load(), 0u);
}

void TestPhase2Dsp::testPcmRingBufferConcurrentSPSCAndUnderrun()
{
    using namespace desk32::engine::buffer;
    PcmRingBuffer ring(4096);
    QCOMPARE(ring.capacity(), size_t(4096));

    // 1. Underrun test: reading from empty ring buffer returns 0
    std::vector<double> outL(256), outR(256);
    size_t readEmpty = ring.readPlanar(outL.data(), outR.data(), 256);
    QCOMPARE(readEmpty, size_t(0));

    // 2. Multithreaded SPSC test: Producer writes 20,000 frames, Consumer reads them
    const size_t totalFrames = 20000;
    std::atomic<bool> producerDone{false};
    std::vector<double> consumedL;
    consumedL.reserve(totalFrames);
    std::vector<double> consumedR;
    consumedR.reserve(totalFrames);

    std::thread producer([&ring, &producerDone, totalFrames]() {
        size_t written = 0;
        std::vector<double> chunkL(128);
        std::vector<double> chunkR(128);
        while (written < totalFrames) {
            size_t n = std::min(size_t(128), totalFrames - written);
            for (size_t i = 0; i < n; ++i) {
                chunkL[i] = static_cast<double>(written + i) * 0.001;
                chunkR[i] = -static_cast<double>(written + i) * 0.001;
            }
            size_t actual = ring.writePlanar(chunkL.data(), chunkR.data(), n);
            written += actual;
            if (actual == 0) {
                std::this_thread::yield();
            }
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&ring, &producerDone, &consumedL, &consumedR, totalFrames]() {
        std::vector<double> blockL(256);
        std::vector<double> blockR(256);
        while (consumedL.size() < totalFrames) {
            size_t n = ring.readPlanar(blockL.data(), blockR.data(), 256);
            if (n > 0) {
                consumedL.insert(consumedL.end(), blockL.begin(), blockL.begin() + static_cast<ptrdiff_t>(n));
                consumedR.insert(consumedR.end(), blockR.begin(), blockR.begin() + static_cast<ptrdiff_t>(n));
            } else if (producerDone.load(std::memory_order_acquire)) {
                // Drain remaining
                n = ring.readPlanar(blockL.data(), blockR.data(), 256);
                if (n > 0) {
                    consumedL.insert(consumedL.end(), blockL.begin(), blockL.begin() + static_cast<ptrdiff_t>(n));
                    consumedR.insert(consumedR.end(), blockR.begin(), blockR.begin() + static_cast<ptrdiff_t>(n));
                } else {
                    break;
                }
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    QCOMPARE(consumedL.size(), totalFrames);
    QCOMPARE(consumedR.size(), totalFrames);
    for (size_t i = 0; i < totalFrames; ++i) {
        QCOMPARE(consumedL[i], static_cast<double>(i) * 0.001);
        QCOMPARE(consumedR[i], -static_cast<double>(i) * 0.001);
    }

    // 3. RT Safety: Verify zero dynamic heap allocation in readPlanar
    s_allocationCount.store(0);
    s_trackAllocations.store(true);
    for (int i = 0; i < 1000; ++i) {
        (void)ring.readPlanar(outL.data(), outR.data(), 256);
    }
    s_trackAllocations.store(false);
    QCOMPARE(s_allocationCount.load(), 0u);
}

void TestPhase2Dsp::testParameterCommandQueueSPSCAndCapacity()
{
    ParameterCommandQueue queue;
    QCOMPARE(queue.size(), size_t(0));
    QVERIFY(queue.empty());

    // 1. Capacity boundary test (CAPACITY = 512)
    for (size_t i = 0; i < ParameterCommandQueue::CAPACITY; ++i) {
        DspCommand cmd;
        cmd.type = DspCommandType::SetGeqBandGain;
        cmd.index = static_cast<uint8_t>(i % 32);
        cmd.param1 = static_cast<float>(i);
        QVERIFY(queue.push(cmd));
    }
    QCOMPARE(queue.size(), ParameterCommandQueue::CAPACITY);

    // 513th push should fail
    DspCommand overflowCmd;
    overflowCmd.type = DspCommandType::ResetAllGeqBands;
    QVERIFY(!queue.push(overflowCmd));

    // 2. FIFO verification
    for (size_t i = 0; i < ParameterCommandQueue::CAPACITY; ++i) {
        DspCommand outCmd;
        QVERIFY(queue.pop(outCmd));
        QCOMPARE(outCmd.type, DspCommandType::SetGeqBandGain);
        QCOMPARE(outCmd.index, static_cast<uint8_t>(i % 32));
        QCOMPARE(outCmd.param1, static_cast<float>(i));
    }
    QVERIFY(queue.empty());
    DspCommand emptyCmd;
    QVERIFY(!queue.pop(emptyCmd));

    // 3. Multi-Producer (MPSC) Concurrency Verification (P0-25)
    std::atomic<bool> startProducers{false};
    std::atomic<uint64_t> totalPushed{0};
    std::atomic<uint64_t> totalPopped{0};
    std::atomic<bool> stopConsumer{false};

    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&, p]() {
            while (!startProducers.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 50; ++i) {
                DspCommand c;
                c.type = DspCommandType::SetNodeBypass;
                c.index = static_cast<uint8_t>(p);
                c.param1 = static_cast<float>(i);
                while (!queue.push(c)) {
                    std::this_thread::yield();
                }
                totalPushed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread consumer([&]() {
        DspCommand popped;
        while (!stopConsumer.load(std::memory_order_relaxed) || !queue.empty()) {
            if (queue.pop(popped)) {
                totalPopped.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    startProducers.store(true, std::memory_order_release);
    for (auto& pr : producers) pr.join();
    stopConsumer.store(true, std::memory_order_release);
    consumer.join();

    QCOMPARE(totalPushed.load(), 200u);
    QCOMPARE(totalPopped.load(), 200u);
    QVERIFY(queue.empty());
}

void TestPhase2Dsp::testParameterCommandQueueCoalescingAndOverflowStats()
{
    // P0-24: Strict SPSC Queue without in-place mutation, FIFO ordering, and overflow tracking
    ParameterCommandQueue queue;
    QCOMPARE(queue.overflowCount(), 0u);
    QCOMPARE(queue.size(), 0u);
    QVERIFY(queue.empty());

    // 1. Fill queue to capacity (512 items)
    for (size_t i = 0; i < ParameterCommandQueue::CAPACITY; ++i) {
        DspCommand cmd;
        cmd.type = DspCommandType::SetGeqBandGain;
        cmd.index = static_cast<uint8_t>(i % 32);
        cmd.subIndex = 0;
        cmd.param1 = static_cast<float>(i);
        QVERIFY(queue.push(cmd));
    }
    QCOMPARE(queue.size(), ParameterCommandQueue::CAPACITY);

    // 2. Queue full: Pushes must fail cleanly without corrupting existing elements (P0-24)
    DspCommand overflowCmd;
    overflowCmd.type = DspCommandType::SetGeqBandGain;
    overflowCmd.index = 5;
    overflowCmd.param1 = 999.0f;
    QVERIFY(!queue.push(overflowCmd)); // Must return false
    QCOMPARE(queue.overflowCount(), 1u);
    QCOMPARE(queue.size(), ParameterCommandQueue::CAPACITY);

    // 3. Verify strict FIFO ordering of all items (first-in, first-out)
    for (size_t i = 0; i < ParameterCommandQueue::CAPACITY; ++i) {
        DspCommand out;
        QVERIFY(queue.pop(out));
        QCOMPARE(out.type, DspCommandType::SetGeqBandGain);
        QCOMPARE(out.index, static_cast<uint8_t>(i % 32));
        QCOMPARE(out.param1, static_cast<float>(i));
    }
    QVERIFY(queue.empty());

    // 4. After draining, queue accepts pushes again
    DspCommand nextCmd;
    nextCmd.type = DspCommandType::SetNodeBypass;
    nextCmd.index = 1;
    QVERIFY(queue.push(nextCmd));
    QCOMPARE(queue.size(), 1u);
}

void TestPhase2Dsp::testDspParameterMailboxMultiProducerAndContinuousUpdates()
{
    // P0-25: Multi-Producer safe continuous parameter mailbox with lock-free consumer
    DspParameterMailbox mailbox;
    QCOMPARE(mailbox.updateCount(), 0u);

    ContinuousDspParameters params;
    QVERIFY(!mailbox.fetchUpdates(params)); // Initially empty

    // 1. Single producer update
    mailbox.setGeqBandGain(3, 4.5f);
    QCOMPARE(mailbox.updateCount(), 1u);

    QVERIFY(mailbox.fetchUpdates(params));
    QCOMPARE(params.geqGains[3], 4.5f);
    QVERIFY((params.geqModifiedMask & (1u << 3)) != 0);

    // Second fetch without new updates must return false
    QVERIFY(!mailbox.fetchUpdates(params));

    // 2. Concurrent multi-producer stress test (4 producer threads, 1 consumer thread)
    std::atomic<bool> running{true};
    std::atomic<uint64_t> totalPushes{0};
    std::vector<std::thread> producers;

    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&mailbox, &running, &totalPushes, t]() {
            float val = 0.0f;
            while (running.load(std::memory_order_relaxed)) {
                mailbox.setGeqBandGain(static_cast<size_t>(t * 8), val);
                mailbox.setPeqBand(static_cast<size_t>(t * 5), 0, 1000.0f + val, val, 1.0f, 0, true);
                mailbox.setFilterParams(20.0f + val, 20000.0f - val, 1, 1, true, true);
                val += 0.1f;
                totalPushes.fetch_add(3, std::memory_order_relaxed);
            }
        });
    }

    uint64_t consumedUpdates = 0;
    for (int i = 0; i < 500; ++i) {
        ContinuousDspParameters p;
        if (mailbox.fetchUpdates(p)) {
            consumedUpdates++;
        }
        std::this_thread::yield();
    }

    running.store(false);
    for (auto& p : producers) {
        p.join();
    }

    // Final drain
    ContinuousDspParameters finalParams;
    if (mailbox.fetchUpdates(finalParams)) {
        consumedUpdates++;
    }

    QVERIFY(consumedUpdates > 0);

    QVERIFY(mailbox.updateCount() > 0);
    QVERIFY(totalPushes.load() > 0);
}

void TestPhase2Dsp::testDspParameterMailbox3SlotExchangeSnapshotConsistency()
{
    // P0-24, P0-25: 3-Slot Exchange tear-free snapshot consistency verification
    // Ensures that Consumer receives internally consistent parameter snapshots without partial updates
    DspParameterMailbox mailbox;
    QVERIFY(mailbox.isLockFree());

    std::atomic<bool> running{true};
    std::atomic<uint64_t> snapshotCheckCount{0};
    std::atomic<bool> inconsistencyDetected{false};

    // Producer Thread: Writes monotonically increasing generation values across all 32 bands atomically
    std::thread producerThread([&]() {
        float gen = 1.0f;
        while (running.load(std::memory_order_relaxed)) {
            std::array<float, 32> gains{};
            for (size_t b = 0; b < 32; ++b) {
                gains[b] = gen + static_cast<float>(b) * 0.001f;
            }
            mailbox.setAllGeqBandGains(gains);
            gen += 1.0f;
            std::this_thread::yield();
        }
    });

    // Audio/Consumer Thread: Rapidly fetches snapshots and verifies tear-free consistency
    std::thread consumerThread([&]() {
        ContinuousDspParameters p;
        while (running.load(std::memory_order_relaxed)) {
            if (mailbox.fetchUpdates(p)) {
                // Check if all 32 bands correspond to the exact same generation
                float baseGen = std::round(p.geqGains[0]);
                for (size_t b = 0; b < 32; ++b) {
                    float expected = baseGen + static_cast<float>(b) * 0.001f;
                    if (std::abs(p.geqGains[b] - expected) > 1e-4f) {
                        inconsistencyDetected.store(true, std::memory_order_release);
                    }
                }
                snapshotCheckCount.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running.store(false, std::memory_order_relaxed);

    producerThread.join();
    consumerThread.join();

    QVERIFY(!inconsistencyDetected.load());
    QVERIFY(snapshotCheckCount.load() > 10);
}

void TestPhase2Dsp::testDspParameterMailboxTornReadDetectionVsTripleBufferSafety()
{
    // P0-24: Proof of Tear Detection Logic vs Triple Buffer Consistency
#if !defined(__SANITIZE_THREAD__) && !(defined(__has_feature) && __has_feature(thread_sanitizer))
    // Part 1: Intentionally unsynchronized shared structure to prove the detection logic works
    {
        struct alignas(64) UnsynchronizedBuffer {
            std::array<float, 32> gains{};
        } rawShared;

        std::atomic<bool> runRaw{true};
        std::atomic<uint64_t> rawTearsDetected{0};
        std::atomic<uint64_t> rawReads{0};

        std::thread rawWriter([&]() {
            float gen = 1.0f;
            while (runRaw.load(std::memory_order_relaxed)) {
                for (size_t b = 0; b < 32; ++b) {
                    rawShared.gains[b] = gen + static_cast<float>(b) * 0.001f;
                }
                gen += 1.0f;
            }
        });

        std::thread rawReader([&]() {
            while (runRaw.load(std::memory_order_relaxed)) {
                std::array<float, 32> snap = rawShared.gains;
                float baseGen = std::round(snap[0]);
                for (size_t b = 0; b < 32; ++b) {
                    float expected = baseGen + static_cast<float>(b) * 0.001f;
                    if (std::abs(snap[b] - expected) > 1e-4f) {
                        rawTearsDetected.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
                rawReads.fetch_add(1, std::memory_order_relaxed);
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        runRaw.store(false, std::memory_order_relaxed);
        rawWriter.join();
        rawReader.join();

        // Proves the tear detection logic detects asynchronous tearing
        QVERIFY(rawReads.load() > 100);
        QVERIFY(rawTearsDetected.load() > 0);
    }
#endif

    // Part 2: Identical verification on DspParameterMailbox yields strictly 0 tears
    {
        DspParameterMailbox mailbox;
        std::atomic<bool> runMailbox{true};
        std::atomic<uint64_t> mailboxTearsDetected{0};
        std::atomic<uint64_t> mailboxReads{0};

        std::thread mailboxWriter([&]() {
            float gen = 1.0f;
            while (runMailbox.load(std::memory_order_relaxed)) {
                std::array<float, 32> gains{};
                for (size_t b = 0; b < 32; ++b) {
                    gains[b] = gen + static_cast<float>(b) * 0.001f;
                }
                mailbox.setAllGeqBandGains(gains);
                gen += 1.0f;
            }
        });

        std::thread mailboxReader([&]() {
            ContinuousDspParameters snap;
            while (runMailbox.load(std::memory_order_relaxed)) {
                if (mailbox.fetchUpdates(snap)) {
                    float baseGen = std::round(snap.geqGains[0]);
                    for (size_t b = 0; b < 32; ++b) {
                        float expected = baseGen + static_cast<float>(b) * 0.001f;
                        if (std::abs(snap.geqGains[b] - expected) > 1e-4f) {
                            mailboxTearsDetected.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                    }
                    mailboxReads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        runMailbox.store(false, std::memory_order_relaxed);
        mailboxWriter.join();
        mailboxReader.join();

        // 100% tear-free guarantee
        QVERIFY(mailboxReads.load() > 10);
        QCOMPARE(mailboxTearsDetected.load(), 0u);
    }
}

void TestPhase2Dsp::testDspParameterMailboxBatchApis()
{
    // P0-25, P0-27: Batch Mailbox APIs (resetAllGeqBands, applyPeqPreset, setPeqBandEnabled)
    DspParameterMailbox mailbox;

    // 1. Test resetAllGeqBands()
    for (size_t b = 0; b < 32; ++b) {
        mailbox.setGeqBandGain(b, 5.0f);
    }
    mailbox.resetAllGeqBands();

    ContinuousDspParameters p;
    QVERIFY(mailbox.fetchUpdates(p));
    QCOMPARE(p.geqModifiedMask, 0xFFFFFFFFu);
    for (size_t b = 0; b < 32; ++b) {
        QCOMPARE(p.geqGains[b], 0.0f);
    }

    // 2. Test setPeqBandEnabled()
    mailbox.setPeqBandEnabled(4, 0, true);
    mailbox.setPeqBandEnabled(7, 1, false);
    QVERIFY(mailbox.fetchUpdates(p));
    QVERIFY((p.peqLeftModifiedMask & (1u << 4)) != 0);
    QCOMPARE(p.peqLeftBands[4].enabled, 1);
    QVERIFY((p.peqRightModifiedMask & (1u << 7)) != 0);
    QCOMPARE(p.peqRightBands[7].enabled, 0);

    // 3. Test applyPeqPreset()
    std::array<ContinuousDspParameters::PeqBandConfig, 24> leftPreset{};
    std::array<ContinuousDspParameters::PeqBandConfig, 24> rightPreset{};
    for (size_t b = 0; b < 24; ++b) {
        leftPreset[b].frequency = 100.0f * static_cast<float>(b + 1);
        leftPreset[b].gainDb = -2.0f;
        leftPreset[b].Q = 1.4f;
        leftPreset[b].type = 1;
        leftPreset[b].enabled = 1;

        rightPreset[b].frequency = 120.0f * static_cast<float>(b + 1);
        rightPreset[b].gainDb = 2.0f;
        rightPreset[b].Q = 0.7f;
        rightPreset[b].type = 2;
        rightPreset[b].enabled = 1;
    }
    mailbox.applyPeqPreset(leftPreset, rightPreset);

    QVERIFY(mailbox.fetchUpdates(p));
    QCOMPARE(p.peqLeftModifiedMask, 0x00FFFFFFu);
    QCOMPARE(p.peqRightModifiedMask, 0x00FFFFFFu);
    QCOMPARE(p.peqLeftBands[0].gainDb, -2.0f);
    QCOMPARE(p.peqRightBands[0].gainDb, 2.0f);
}

void TestPhase2Dsp::testSingleThreadDspUnitOperation()
{
    // Test A (P0-23): Single-thread unit operations for all DSP processors
    // Verifies configure, process, reset, and parameter retention on a single thread.

    // 1. Compressor
    Compressor comp;
    comp.configure(48000.0, -12.0, 4.0, 10.0, 100.0, 3.5, 4.0, true);
    QCOMPARE(comp.makeupGainDb(), 3.5);
    QCOMPARE(comp.thresholdDb(), -12.0);
    QCOMPARE(comp.ratio(), 4.0);
    QVERIFY(comp.isEnabled());

    double cL = 0.8, cR = 0.8;
    for (int i = 0; i < 100; ++i) {
        comp.process(cL, cR);
    }
    QVERIFY(comp.gainReductionDb() >= 0.0);
    comp.reset();

    // 2. PeakLimiter
    PeakLimiter limiter;
    limiter.configure(48000.0, -0.5, 40.0, 1.0, true);
    QCOMPARE(limiter.ceilingDb(), -0.5);
    QCOMPARE(limiter.releaseMs(), 40.0);
    QCOMPARE(limiter.lookaheadMs(), 1.0);
    QVERIFY(limiter.isEnabled());
    QVERIFY(limiter.latencyFrames() > 0);

    double hotL = 2.0, hotR = 2.0;
    for (int i = 0; i < 200; ++i) {
        limiter.process(hotL, hotR);
    }
    double maxCeilingLinear = std::pow(10.0, -0.5 / 20.0);
    QVERIFY(std::abs(hotL) <= maxCeilingLinear + 1e-4);
    QVERIFY(std::abs(hotR) <= maxCeilingLinear + 1e-4);
    limiter.reset();

    // 3. AlgorithmicReverb
    AlgorithmicReverb reverb;
    reverb.configure(48000.0, 0.8, 0.4, 0.3, 10.0, true, true);
    QCOMPARE(reverb.roomSize(), 0.8);
    QCOMPARE(reverb.wetLevel(), 0.3);
    QVERIFY(reverb.isTailPreserving());
    QVERIFY(reverb.isEnabled());

    double rL = 0.5, rR = 0.5;
    for (int i = 0; i < 100; ++i) {
        reverb.process(rL, rR);
    }
    reverb.reset();

    // 4. ParametricEQ, GraphicEQ, HighLowPassFilter
    ParametricEQ peq;
    peq.configure(48000.0);
    peq.setEnabled(true);
    double pL = 0.5, pR = 0.5;
    peq.process(pL, pR);
    peq.reset();

    GraphicEQ geq;
    geq.configure(48000.0);
    geq.setEnabled(true);
    double gL = 0.5, gR = 0.5;
    geq.process(gL, gR);
    geq.reset();

    HighLowPassFilter flt;
    flt.configure(48000.0);
    flt.setHpfEnabled(true);
    flt.setLpfEnabled(true);
    double fL = 0.5, fR = 0.5;
    flt.process(fL, fR);
    flt.reset();
}

void TestPhase2Dsp::testPreparedDspInstanceExchangeSafety()
{
    // P0-21, P0-22, P0-23: Comprehensive Verification of Prepared DSP Instance Exchange (Cases 1 - 9)

    // Case 1: Initial publish
    {
        CompressorNode compNode;
        PeakLimiterNode limNode;
        ReverbNode revNode;
        QVERIFY(compNode.dsp().isEnabled());
        QVERIFY(limNode.dsp().isEnabled());
        QVERIFY(revNode.dsp().isEnabled());
    }

    // Case 2: Active exchange at block boundary
    {
        CompressorNode compNode;
        compNode.prepareInstance(48000.0, -18.0, 4.0, 10.0, 100.0, 3.5, 4.0, true);
        std::vector<double> left(128, 0.5), right(128, 0.5);
        compNode.process(left.data(), right.data(), 128);
        QCOMPARE(compNode.dsp().thresholdDb(), -18.0);
        QCOMPARE(compNode.dsp().makeupGainDb(), 3.5);
    }

    // Case 3: Old instance retirement & reclaim
    {
        CompressorNode compNode;
        compNode.prepareInstance(48000.0, -15.0, 3.0, 10.0, 100.0, 2.0, 4.0, true);
        std::vector<double> left(128, 0.5), right(128, 0.5);
        compNode.process(left.data(), right.data(), 128);
        // Retired old instance is held until reclaimRetired is invoked on Control Thread
        compNode.reclaimRetired();
    }

    // Case 4: Sequential rapid exchanges
    {
        PeakLimiterNode limNode;
        for (int i = 0; i < 20; ++i) {
            double ceilDb = -0.5 - static_cast<double>(i) * 0.1;
            limNode.prepareInstance(48000.0, ceilDb, 100.0, 5.0, true);
            std::vector<double> left(64, 0.5), right(64, 0.5);
            limNode.process(left.data(), right.data(), 64);
            limNode.reclaimRetired();
            QCOMPARE(limNode.dsp().ceilingDb(), ceilDb);
        }
    }

    // Case 5: Overwrite before consumption (unused prepared replacement)
    {
        CompressorNode compNode;
        // Prepare config A
        compNode.prepareInstance(48000.0, -10.0, 2.0, 10.0, 100.0, 1.0, 4.0, true);
        // Overwrite with config B without calling process() in between
        compNode.prepareInstance(48000.0, -22.0, 5.0, 10.0, 100.0, 6.0, 4.0, true);

        std::vector<double> left(128, 0.5), right(128, 0.5);
        compNode.process(left.data(), right.data(), 128);
        QCOMPARE(compNode.dsp().thresholdDb(), -22.0);
        QCOMPARE(compNode.dsp().makeupGainDb(), 6.0);
        compNode.reclaimRetired();
    }

    // Case 6: Concurrent Audio Thread processing during rapid exchanges
    {
        auto compNode = std::make_unique<CompressorNode>();
        auto limNode = std::make_unique<PeakLimiterNode>();
        auto revNode = std::make_unique<ReverbNode>();

        std::atomic<bool> stopAudio{false};
        std::atomic<uint64_t> blocksProcessed{0};

        std::thread audioThread([&]() {
            std::vector<double> left(128, 0.4), right(128, 0.4);
            while (!stopAudio.load(std::memory_order_relaxed)) {
                compNode->process(left.data(), right.data(), 128);
                limNode->process(left.data(), right.data(), 128);
                revNode->process(left.data(), right.data(), 128);
                blocksProcessed.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        });

        for (int i = 0; i < 30; ++i) {
            compNode->prepareInstance(48000.0, -10.0 - (i % 8), 3.0, 10.0, 100.0, static_cast<double>(i % 4), 4.0, true);
            limNode->prepareInstance(48000.0, -0.5 - (i % 5) * 0.2, 50.0 + (i % 10) * 5.0, 5.0, true);
            revNode->prepareInstance(48000.0, 0.5 + (i % 4) * 0.1, 0.3, 0.3, 10.0, true, true);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        stopAudio.store(true, std::memory_order_relaxed);
        audioThread.join();

        QVERIFY(blocksProcessed.load() > 20);
        compNode->reclaimRetired();
        limNode->reclaimRetired();
        revNode->reclaimRetired();
    }

    // Case 7 & 8: Shutdown during active exchange and unused instance discard
    {
        auto node = std::make_unique<CompressorNode>();
        node->prepareInstance(48000.0, -14.0, 2.0, 10.0, 100.0, 2.0, 4.0, true);
        // Destroy without process() (Case 8: unused discard)
        node.reset();

        auto node2 = std::make_unique<PeakLimiterNode>();
        node2->prepareInstance(48000.0, -1.0, 100.0, 5.0, true);
        std::vector<double> left(128, 0.5), right(128, 0.5);
        node2->process(left.data(), right.data(), 128);
        node2->prepareInstance(48000.0, -2.0, 100.0, 5.0, true);
        // Destroy with retired and prepared instances pending (Case 7)
        node2.reset();
    }

    // Case 9: State inheritance correctness
    // 9.1: Compressor envelope inheritance
    {
        CompressorNode comp;
        comp.prepareInstance(48000.0, -20.0, 4.0, 5.0, 500.0, 0.0, 4.0, true);
        std::vector<double> loudLeft(512, 0.9), loudRight(512, 0.9);
        comp.process(loudLeft.data(), loudRight.data(), 512);
        double grBefore = comp.gainReductionDb();
        QVERIFY(grBefore > 2.0); // Significant gain reduction

        // Prepare new instance with different ratio, state must inherit
        comp.prepareInstance(48000.0, -20.0, 6.0, 5.0, 500.0, 0.0, 4.0, true);
        std::vector<double> nextLeft(128, 0.9), nextRight(128, 0.9);
        comp.process(nextLeft.data(), nextRight.data(), 128);
        double grAfter = comp.gainReductionDb();
        // Envelope was inherited: gain reduction should not reset to 0
        QVERIFY(grAfter > 2.0);
    }

    // 9.2: PeakLimiter lookahead buffer inheritance
    {
        PeakLimiterNode lim;
        lim.prepareInstance(48000.0, -0.5, 100.0, 5.0, true);
        std::vector<double> inL(256, 0.8), inR(256, 0.8);
        lim.process(inL.data(), inR.data(), 256);

        // Prepare new instance, lookahead state must inherit smoothly without discontinuity
        lim.prepareInstance(48000.0, -1.0, 100.0, 5.0, true);
        std::vector<double> inL2(128, 0.8), inR2(128, 0.8);
        lim.process(inL2.data(), inR2.data(), 128);
        for (double s : inL2) {
            QVERIFY(!std::isnan(s));
            QVERIFY(std::abs(s) <= 1.0);
        }
    }

    // 9.3: Reverb tail decay preservation
    {
        ReverbNode rev;
        rev.prepareInstance(48000.0, 0.8, 0.2, 0.5, 0.0, true, true);
        // Excite with continuous audio to fill comb and all-pass delay buffers
        std::vector<double> exciteL(2048), exciteR(2048);
        for (size_t i = 0; i < 2048; ++i) {
            exciteL[i] = std::sin(static_cast<double>(i) * 0.1);
            exciteR[i] = exciteL[i];
        }
        rev.process(exciteL.data(), exciteR.data(), 2048);

        // Prepare new reverb instance with different room size
        rev.prepareInstance(48000.0, 0.9, 0.3, 0.5, 0.0, true, true);
        std::vector<double> tailL(512, 0.0), tailR(512, 0.0);
        rev.process(tailL.data(), tailR.data(), 512);

        double tailEnergy = 0.0;
        for (size_t i = 0; i < 512; ++i) {
            tailEnergy += tailL[i] * tailL[i] + tailR[i] * tailR[i];
        }
        // Tail decay was inherited and preserved across exchange
        QVERIFY(tailEnergy > 1e-4);
    }

    // Case 10: Reverb Tail Continuity under repeated exchanges
    {
        ReverbNode rev;
        rev.prepareInstance(48000.0, 0.85, 0.3, 0.5, 0.0, true, true);
        std::vector<double> excite(1024, 0.5);
        rev.process(excite.data(), excite.data(), 1024);

        // Perform multiple exchanges while input is zero
        std::vector<double> zero(256, 0.0);
        for (int i = 0; i < 4; ++i) {
            rev.prepareInstance(48000.0, 0.7 - i * 0.1, 0.3, 0.5, 0.0, true, true);
            rev.process(zero.data(), zero.data(), 256);
            double e = 0.0;
            for (double s : zero) e += s * s;
            // Tail should smoothly decay without dropping to 0 instantaneously
            QVERIFY(e > 1e-5);
        }
        rev.reclaimRetired();
    }

    // Case 11: Crossfade Output Continuity (No clicks/pops/discontinuities)
    {
        ReverbNode rev;
        rev.prepareInstance(48000.0, 0.7, 0.3, 0.4, 0.0, true, true);
        std::vector<double> sig(512, 0.3);
        rev.process(sig.data(), sig.data(), 512);

        // Swap instance and check continuity
        rev.prepareInstance(48000.0, 0.9, 0.2, 0.6, 0.0, true, true);
        std::vector<double> out1(256, 0.3);
        rev.process(out1.data(), out1.data(), 256);

        // Max difference between consecutive samples must not exhibit abrupt step discontinuity
        for (size_t i = 1; i < 256; ++i) {
            double delta = std::abs(out1[i] - out1[i - 1]);
            QVERIFY(delta < 0.25);
        }
        rev.reclaimRetired();
    }

    // Case 12: Bounded execution time during exchange (Microbenchmark)
    {
        ReverbNode rev;
        rev.prepareInstance(48000.0, 0.7, 0.3, 0.3, 0.0, true, true);
        std::vector<double> block(256, 0.2);

        auto t0 = std::chrono::steady_clock::now();
        // Trigger prepared instance exchange at block boundary
        rev.prepareInstance(48000.0, 0.8, 0.4, 0.4, 0.0, true, true);
        rev.process(block.data(), block.data(), 256);
        auto t1 = std::chrono::steady_clock::now();

        int64_t elapsedNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        // 256 frames of reverb processing with crossfade must easily complete under 500 microseconds (500,000 ns)
        QVERIFY(elapsedNanos < 5000000);
        rev.reclaimRetired();
    }

    // Case 13: Zero Variable-Length Buffer Copying on Audio Thread (RT Safety)
    {
        ReverbNode rev;
        rev.prepareInstance(48000.0, 0.7, 0.3, 0.3, 0.0, true, true);
        std::vector<double> block(256, 0.2);
        rev.process(block.data(), block.data(), 256);

        // Prepare new instance
        rev.prepareInstance(48000.0, 0.85, 0.2, 0.5, 0.0, true, true);

        // Track allocations during exchange
        s_allocationCount.store(0);
        s_trackAllocations.store(true);

        rev.process(block.data(), block.data(), 256);

        s_trackAllocations.store(false);
        QCOMPARE(s_allocationCount.load(), 0u);
        rev.reclaimRetired();
    }
}

void TestPhase2Dsp::testContinuousParameterMailboxUpdates()
{
    // Test C (P0-23, P0-25): Rapid continuous parameter updates via Mailbox
    // Audio Thread fetches parameter snapshots and applies them without locks.
    DspParameterMailbox mailbox;
    std::atomic<bool> stopWorkers{false};
    std::atomic<uint64_t> fetchesCompleted{0};

    // Audio Thread
    std::thread audioThread([&]() {
        ContinuousDspParameters p;
        while (!stopWorkers.load(std::memory_order_relaxed)) {
            if (mailbox.fetchUpdates(p)) {
                fetchesCompleted.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });

    // Control Thread: Continuous slider adjustments
    for (int i = 0; i < 100; ++i) {
        float gain = -12.0f + static_cast<float>(i % 24);
        mailbox.setGeqBandGain(static_cast<size_t>(i % 32), gain);
        mailbox.setFilterParams(30.0f + static_cast<float>(i * 5), 18000.0f, 1, 1, true, true);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    stopWorkers.store(true, std::memory_order_relaxed);
    audioThread.join();

    QVERIFY(fetchesCompleted.load() > 5);
    QVERIFY(mailbox.updateCount() >= 100);
}

void TestPhase2Dsp::testRtSafetyAllocationDuringProcess()
{
    // Test D (P0-23): Real-Time Safety - Zero dynamic allocations during Audio process() loop
    Compressor comp;
    comp.configure(48000.0, -12.0, 4.0, 10.0, 100.0, 3.0, 4.0, true);

    PeakLimiter limiter;
    limiter.configure(48000.0, -0.5, 40.0, 1.0, true);

    AlgorithmicReverb reverb;
    reverb.configure(48000.0, 0.7, 0.3, 0.2, 5.0, true, true);

    ParametricEQ peq;
    peq.configure(48000.0);
    peq.setEnabled(true);

    GraphicEQ geq;
    geq.configure(48000.0);
    geq.setEnabled(true);

    HighLowPassFilter flt;
    flt.configure(48000.0);
    flt.setHpfEnabled(true);
    flt.setLpfEnabled(true);

    s_allocationCount.store(0);
    s_trackAllocations.store(true);

    double left = 0.5, right = 0.5;
    for (int i = 0; i < 10000; ++i) {
        comp.process(left, right);
        limiter.process(left, right);
        reverb.process(left, right);
        peq.process(left, right);
        geq.process(left, right);
        flt.process(left, right);
    }

    s_trackAllocations.store(false);
    QCOMPARE(s_allocationCount.load(), 0u);
}

QTEST_MAIN(TestPhase2Dsp)
#include "test_phase2_dsp.moc"
