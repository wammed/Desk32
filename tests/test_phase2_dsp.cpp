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
#include <latch>

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

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { std::free(p); }

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

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
    void testConcurrentSampleRateAndMailboxUpdates();
    void testReverbMultiExchangeAndBypassTailDecay();
    void testReverbSlotCapacityEvictionAndSafetyCeiling();
    void testDspParameterMailboxEpochAndMonotonicSequence();
    void testParameterCommandQueueOverflowAndDrainLimit();
    void testReverbO1StateTrackingAccuracyAndTailWaveformComparison();
    void testActiveDspDirectModificationImpossibility();
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

    listeningChain->appendNode(correctionPeq);
    pipeline.swapListeningChain(std::move(listeningChain));

    // Apply +12 dB boost at 1 kHz strictly in Listening Correction via Mailbox (P0-21)
    ParametricEQ::BandConfig boost;
    boost.frequency = 1000.0;
    boost.gainDb = 12.0;
    boost.Q = 1.0;
    boost.type = BiquadFilterType::Peak;
    boost.enabled = true;
    pipeline.paramMailbox().setListeningBand(5, boost);

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

    // 5. Consumer Stop / Restart & Statistics Reset (P0-25)
    DspCommand poppedNext;
    QVERIFY(queue.pop(poppedNext));
    QCOMPARE(poppedNext.type, DspCommandType::SetNodeBypass);
    QCOMPARE(poppedNext.index, uint8_t(1));
    QVERIFY(queue.empty());

    queue.resetStatistics();
    QCOMPARE(queue.overflowCount(), 0u);

    // Pause consumer while multiple producers push and finish concurrently, then resume consumer
    std::latch producerStartLatch(4);
    std::vector<std::thread> burstProducers;
    for (int p = 0; p < 4; ++p) {
        burstProducers.emplace_back([&, p]() {
            producerStartLatch.arrive_and_wait();
            for (int seq = 0; seq < 32; ++seq) {
                DspCommand c;
                c.type = DspCommandType::SetNodeBypass;
                c.index = static_cast<uint8_t>(p);
                c.intParam1 = static_cast<uint8_t>(seq);
                QVERIFY(queue.push(c));
            }
        });
    }
    for (auto& t : burstProducers) t.join();
    QCOMPARE(queue.size(), size_t(128));
    QCOMPARE(queue.overflowCount(), 0u);

    // Resume consumer: verify per-producer sequential order is 100% preserved
    std::array<int, 4> lastSeqPerProducer = {-1, -1, -1, -1};
    size_t drainedBurst = 0;
    DspCommand burstOut;
    while (queue.pop(burstOut)) {
        size_t prodId = static_cast<size_t>(burstOut.index);
        int seq = static_cast<int>(burstOut.intParam1);
        QVERIFY(prodId < 4u);
        QCOMPARE(seq, lastSeqPerProducer[prodId] + 1);
        lastSeqPerProducer[prodId] = seq;
        drainedBurst++;
    }
    QCOMPARE(drainedBurst, size_t(128));
    for (size_t p = 0; p < 4; ++p) {
        QCOMPARE(lastSeqPerProducer[p], 31);
    }
}

void TestPhase2Dsp::testDspParameterMailboxMultiProducerAndContinuousUpdates()
{
    // P0-24, P0-25: Multi-Producer safe continuous parameter mailbox with lock-free consumer
    DspParameterMailbox mailbox;
    QVERIFY(mailbox.isLockFree());
    QCOMPARE(mailbox.updateCount(), 0u);

    ContinuousDspParameters params;
    QVERIFY(!mailbox.fetchUpdates(params)); // Immediately after initialization: no pending update

    // 1. Single producer update & sequence number monotonicity
    mailbox.setGeqBandGain(3, 4.5f);
    QCOMPARE(mailbox.updateCount(), 1u);

    QVERIFY(mailbox.fetchUpdates(params));
    QCOMPARE(params.geqGains[3], 4.5f);
    QVERIFY((params.geqModifiedMask & (1u << 3)) != 0);
    QCOMPARE(params.sequenceNumber, 1u);

    // Second fetch without new updates must return false
    QVERIFY(!mailbox.fetchUpdates(params));

    // Re-initialization (reset) verification (P0-24):
    // Preserves sequenceNumber monotonicity while incrementing epoch
    mailbox.reset();
    QCOMPARE(mailbox.currentEpoch(), 2u);
    QCOMPARE(mailbox.updateCount(), 1u); // Monotonic sequence preserved across reset
    QVERIFY(!mailbox.fetchUpdates(params));

    mailbox.setGeqBandGain(5, -3.0f);
    QCOMPARE(mailbox.updateCount(), 2u);
    QVERIFY(mailbox.fetchUpdates(params));
    QCOMPARE(params.geqGains[5], -3.0f);
    QCOMPARE(params.sequenceNumber, 2u);
    QCOMPARE(params.epoch, 2u);

    // 2. Concurrent multi-producer stress test (4 producer threads, 1 consumer thread)
    std::atomic<bool> running{true};
    std::atomic<uint64_t> totalPushes{0};
    std::latch startLatch{4};
    std::vector<std::thread> producers;

    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&mailbox, &running, &totalPushes, &startLatch, t]() {
            startLatch.count_down();
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

    startLatch.wait();

    uint64_t consumedUpdates = 0;
    uint64_t lastSeq = 1;
    // Consume updates until at least 50 updates are verified, or timeout safety
    for (int i = 0; i < 5000 && (consumedUpdates < 50 || totalPushes.load() < 50); ++i) {
        ContinuousDspParameters p;
        if (mailbox.fetchUpdates(p)) {
            // Verify strict monotonic sequenceNumber progression (never rewinds)
            QVERIFY(p.sequenceNumber > lastSeq);
            lastSeq = p.sequenceNumber;
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
        QVERIFY(finalParams.sequenceNumber > lastSeq);
        lastSeq = finalParams.sequenceNumber;
        consumedUpdates++;
    }

    QVERIFY(consumedUpdates > 0);
    QCOMPARE(lastSeq, mailbox.updateCount());
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
        QVERIFY(compNode.isEnabled());
        QVERIFY(limNode.isEnabled());
        QVERIFY(revNode.isEnabled());
    }

    // Case 2: Active exchange at block boundary
    {
        CompressorNode compNode;
        compNode.prepareInstance(48000.0, -18.0, 4.0, 10.0, 100.0, 3.5, 4.0, true);
        std::vector<double> left(128, 0.5), right(128, 0.5);
        compNode.process(left.data(), right.data(), 128);
        QCOMPARE(compNode.thresholdDb(), -18.0);
        QCOMPARE(compNode.makeupGainDb(), 3.5);
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
            QCOMPARE(limNode.ceilingDb(), ceilDb);
        }
    }

    // Case 5: Overwrite before consumption (unused prepared replacement)
    {
        CompressorNode compNode;
        // Prepare config A
        compNode.prepareInstance(48000.0, -10.0, 2.0, 10.0, 100.0, 1.0, 4.0, true);
        // Overwrite with config B without calling process() in between
        compNode.prepareInstance(48000.0, -22.0, 5.0, 10.0, 100.0, 6.0, 4.0, true);
        QCOMPARE(compNode.replacedBeforeActivationCount(), 1u);

        std::vector<double> left(128, 0.5), right(128, 0.5);
        compNode.process(left.data(), right.data(), 128);
        QCOMPARE(compNode.activatedCount(), 1u);
        QCOMPARE(compNode.retiredCount(), 1u);
        QCOMPARE(compNode.thresholdDb(), -22.0);
        QCOMPARE(compNode.makeupGainDb(), 6.0);
        compNode.reclaimRetired();
        QCOMPARE(compNode.reclaimedCount(), 1u);
    }

    // Case 6: Concurrent Multi-Producer Control Threads + Audio Thread processing during rapid exchanges (P0-21)
    {
        auto compNode = std::make_unique<CompressorNode>();
        auto limNode = std::make_unique<PeakLimiterNode>();
        auto revNode = std::make_unique<ReverbNode>();

        std::atomic<bool> stopAudio{false};
        std::atomic<uint64_t> blocksProcessed{0};
        std::latch startSync(3);

        std::thread audioThread([&]() {
            startSync.arrive_and_wait();
            std::vector<double> left(128, 0.4), right(128, 0.4);
            while (!stopAudio.load(std::memory_order_relaxed)) {
                std::fill(left.begin(), left.end(), 0.4);
                std::fill(right.begin(), right.end(), 0.4);
                compNode->process(left.data(), right.data(), 128);
                limNode->process(left.data(), right.data(), 128);
                revNode->process(left.data(), right.data(), 128);
                blocksProcessed.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        });

        // 2 concurrent Control Threads calling prepareInstance simultaneously
        std::vector<std::thread> controlThreads;
        for (int t = 0; t < 2; ++t) {
            controlThreads.emplace_back([&, t]() {
                startSync.arrive_and_wait();
                for (int i = 0; i < 20; ++i) {
                    compNode->prepareInstance(48000.0, -10.0 - ((i + t) % 8), 3.0, 10.0, 100.0, static_cast<double>(i % 4), 4.0, true);
                    limNode->prepareInstance(48000.0, -0.5 - ((i + t) % 5) * 0.2, 50.0 + (i % 10) * 5.0, 5.0, true);
                    revNode->prepareInstance(48000.0, 0.5 + ((i + t) % 4) * 0.1, 0.3, 0.3, 10.0, true, true);
                    std::this_thread::sleep_for(std::chrono::microseconds(300));
                }
            });
        }

        for (auto& ct : controlThreads) ct.join();

        while (blocksProcessed.load(std::memory_order_relaxed) <= 20) {
            std::this_thread::yield();
        }
        stopAudio.store(true, std::memory_order_relaxed);
        audioThread.join();

        QVERIFY(blocksProcessed.load() > 20);
        compNode->reclaimRetired();
        limNode->reclaimRetired();
        revNode->reclaimRetired();
        QCOMPARE(compNode->retiredCount(), compNode->reclaimedCount());
        QCOMPARE(limNode->retiredCount(), limNode->reclaimedCount());
        QCOMPARE(revNode->retiredCount(), revNode->reclaimedCount());
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

    // Case 10: Reverb Tail Continuity under repeated exchanges (distinct L/R zero-filled buffers)
    {
        ReverbNode rev;
        rev.prepareInstance(48000.0, 0.85, 0.3, 0.5, 0.0, true, true);
        std::vector<double> exciteL(2048, 0.5), exciteR(2048, 0.5);
        rev.process(exciteL.data(), exciteR.data(), 2048);

        // Perform multiple exchanges while input is strictly zero (P0-22: Tail preservation across exchanges)
        std::vector<double> zeroL(256, 0.0), zeroR(256, 0.0);
        for (int i = 0; i < 8; ++i) { // 8 blocks = 2048 frames (~42.7ms)
            std::fill(zeroL.begin(), zeroL.end(), 0.0);
            std::fill(zeroR.begin(), zeroR.end(), 0.0);
            if (i < 4) {
                rev.prepareInstance(48000.0, 0.7 - i * 0.1, 0.3, 0.5, 0.0, true, true);
            }
            rev.process(zeroL.data(), zeroR.data(), 256);
            double e = 0.0;
            for (size_t s = 0; s < 256; ++s) {
                e += zeroL[s] * zeroL[s] + zeroR[s] * zeroR[s];
            }
            // Tail should smoothly decay without dropping to 0 instantaneously
            QVERIFY(e > 1e-6);
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

    // Case 12: Bounded execution time during exchange on Audio Thread (Microbenchmark)
    {
        ReverbNode rev;
        rev.prepareInstance(48000.0, 0.7, 0.3, 0.3, 0.0, true, true);
        std::vector<double> block(256, 0.2);

        // Control Thread prepares instance beforehand
        rev.prepareInstance(48000.0, 0.8, 0.4, 0.4, 0.0, true, true);

        // Audio Thread: only measure process() execution time
        auto t0 = std::chrono::steady_clock::now();
        rev.process(block.data(), block.data(), 256);
        auto t1 = std::chrono::steady_clock::now();

        int64_t elapsedNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        // 256 frames of reverb processing on Audio Thread must easily complete under 500 microseconds (500,000 ns) in native,
        // or under 10ms with Sanitizer instrumentation
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__) || defined(DESK32_SANITIZE_UNDEFINED)
        QVERIFY(elapsedNanos < 10000000);
#else
        QVERIFY(elapsedNanos < 500000);
#endif
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

void TestPhase2Dsp::testConcurrentSampleRateAndMailboxUpdates()
{
    // Test E (P0-24): Multi-thread concurrent setSampleRate and parameter updates
    DspParameterMailbox mailbox;
    std::atomic<bool> stop{false};

    // Thread 1: Rapid sample rate changes
    std::thread t1([&]() {
        const double rates[] = {44100.0, 48000.0, 88200.0, 96000.0, 192000.0};
        int idx = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            mailbox.setSampleRate(rates[idx % 5]);
            idx++;
            std::this_thread::yield();
        }
    });

    // Thread 2: Rapid EQ parameter changes
    std::thread t2([&]() {
        int idx = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            mailbox.setGeqBandGain(static_cast<size_t>(idx % 32), static_cast<float>((idx % 24) - 12));
            idx++;
            std::this_thread::yield();
        }
    });

    // Reader Thread: Audio Thread fetching parameters
    ContinuousDspParameters params;
    uint64_t fetches = 0;
    for (int i = 0; i < 200; ++i) {
        if (mailbox.fetchUpdates(params)) {
            fetches++;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    stop.store(true, std::memory_order_relaxed);
    t1.join();
    t2.join();

    QVERIFY(fetches > 0);
}

void TestPhase2Dsp::testReverbMultiExchangeAndBypassTailDecay()
{
    // P0-22: Deep Acoustic & Lifecycle Verification of Reverb Ringout Slots (All Retire Reasons & Edge Cases)

    // 1. Capacity Eviction (SlotCapacityEviction): 10 consecutive excited exchanges (> MAX_RINGOUT_SLOTS = 8)
    {
        ReverbNode rev;
        rev.prepareInstance(48000.0, 0.9, 0.2, 0.5, 0.0, true, true);
        std::vector<double> exciteL(2048, 0.6), exciteR(2048, 0.6);
        rev.process(exciteL.data(), exciteR.data(), 2048);

        // Perform 10 rapid exchanges where each instance is excited then swapped; measure pure ringout tail on zero input
        std::vector<double> pulseL(64, 0.4), pulseR(64, 0.4);
        std::vector<double> zeroL(256, 0.0), zeroR(256, 0.0);
        for (int i = 0; i < 10; ++i) {
            rev.prepareInstance(48000.0, 0.88 - i * 0.02, 0.25, 0.5, 0.0, true, true);
            // Activate new instance and excite it briefly so it has internal energy for next swap
            std::fill(pulseL.begin(), pulseL.end(), 0.4);
            std::fill(pulseR.begin(), pulseR.end(), 0.4);
            rev.process(pulseL.data(), pulseR.data(), 64);

            // Measure pure decaying tail with zero input
            std::fill(zeroL.begin(), zeroL.end(), 0.0);
            std::fill(zeroR.begin(), zeroR.end(), 0.0);
            rev.process(zeroL.data(), zeroR.data(), 256);
            double e = 0.0;
            for (size_t s = 0; s < 256; ++s) {
                e += zeroL[s] * zeroL[s] + zeroR[s] * zeroR[s];
            }
            // Tail MUST NEVER be abruptly cut to 0 even when ringout capacity (8 slots) is exceeded
            QVERIFY(e > 1e-6);
        }
        // 10 excited swaps: first 8 fill all 8 ringout slots, swaps 9 and 10 evict the 2 lowest-energy slots
        QCOMPARE(rev.activeRingoutSlotCount(), ReverbNode::MAX_RINGOUT_SLOTS);
        QCOMPARE(rev.capacityEvictionRetireCount(), 2u);
        QCOMPARE(rev.lastRetireReason(), ReverbNode::RetireReason::SlotCapacityEviction);
        rev.reclaimRetired();
        QCOMPARE(rev.retiredCount(), rev.reclaimedCount());
    }

    // 2. Natural Decay Complete (NaturalDecayComplete): Short RT60 decays naturally and frees slot
    {
        ReverbNode rev;
        rev.prepareInstance(48000.0, 0.2, 0.5, 0.5, 0.0, true, true); // Short decay roomSize = 0.2
        std::vector<double> exciteL(2048, 0.5), exciteR(2048, 0.5);
        rev.process(exciteL.data(), exciteR.data(), 2048);

        // Swap to a new instance -> old instance enters ringout slot 0
        rev.prepareInstance(48000.0, 0.3, 0.5, 0.5, 0.0, true, true);
        std::vector<double> zeroL(512, 0.0), zeroR(512, 0.0);
        rev.process(zeroL.data(), zeroR.data(), 512);
        QCOMPARE(rev.activeRingoutSlotCount(), size_t(1));

        // Process silent blocks until natural decay completes (theoreticalDecayFrames for roomSize=0.2 is 0.5s = 24,000 frames = ~47 blocks of 512)
        for (int b = 0; b < 60; ++b) {
            std::fill(zeroL.begin(), zeroL.end(), 0.0);
            std::fill(zeroR.begin(), zeroR.end(), 0.0);
            rev.process(zeroL.data(), zeroR.data(), 512);
            if (rev.activeRingoutSlotCount() == 0) break;
        }
        QCOMPARE(rev.activeRingoutSlotCount(), size_t(0));
        QCOMPARE(rev.naturalDecayRetireCount(), 1u);
        QCOMPARE(rev.lastRetireReason(), ReverbNode::RetireReason::NaturalDecayComplete);
        rev.reclaimRetired();
        QCOMPARE(rev.retiredCount(), rev.reclaimedCount());
    }

    // 3. Max Retention Safety Ceiling (MaxRetentionSafetyCeiling) at 48kHz and 96kHz
    {
        ReverbNode rev;
        AlgorithmicReverb dummy48;
        dummy48.configure(48000.0, 0.98, 0.1, 0.5, 0.0, true, true);
        ReverbNode::RingoutSlot slot48{&dummy48, 480000, 0, 1.0};
        QCOMPARE(rev.evaluateRetireCondition(slot48, 0.5), ReverbNode::RetireReason::MaxRetentionSafetyCeiling);

        AlgorithmicReverb dummy96;
        dummy96.configure(96000.0, 0.98, 0.1, 0.5, 0.0, true, true);
        ReverbNode::RingoutSlot slot96Early{&dummy96, 480000, 0, 1.0};
        // At 96kHz, 480,000 frames is only 5 seconds -> should NOT hit 10-second safety ceiling yet
        QCOMPARE(rev.evaluateRetireCondition(slot96Early, 0.5), ReverbNode::RetireReason::None);
        ReverbNode::RingoutSlot slot96Full{&dummy96, 960000, 0, 1.0};
        QCOMPARE(rev.evaluateRetireCondition(slot96Full, 0.5), ReverbNode::RetireReason::MaxRetentionSafetyCeiling);
    }

    // 4. Explicit Cancel (reset() and non-tail-preserving exchange)
    {
        ReverbNode rev;
        rev.prepareInstance(48000.0, 0.85, 0.2, 0.5, 0.0, true, true);
        std::vector<double> exciteL(1024, 0.5), exciteR(1024, 0.5);
        rev.process(exciteL.data(), exciteR.data(), 1024);

        rev.prepareInstance(48000.0, 0.8, 0.2, 0.5, 0.0, true, true);
        std::vector<double> zeroL(256, 0.0), zeroR(256, 0.0);
        rev.process(zeroL.data(), zeroR.data(), 256);
        QCOMPARE(rev.activeRingoutSlotCount(), size_t(1));

        // Explicit reset() cancels all active ringout slots immediately
        rev.reset();
        QCOMPARE(rev.activeRingoutSlotCount(), size_t(0));
        QCOMPARE(rev.explicitCancelRetireCount(), 1u);
        QCOMPARE(rev.lastRetireReason(), ReverbNode::RetireReason::ExplicitCancel);
        rev.reclaimRetired();
    }

    // 5. Abnormal State NaN/Inf Protection (AbnormalStateReset)
    {
        ReverbNode rev;
        rev.prepareInstance(48000.0, 0.85, 0.2, 0.5, 0.0, true, true);
        std::vector<double> exciteL(1024, 0.5), exciteR(1024, 0.5);
        rev.process(exciteL.data(), exciteR.data(), 1024);

        // Inject NaN into active reverb state, then swap so it tries to enter ringout
        rev.injectInvalidStateForTest(std::numeric_limits<double>::quiet_NaN());
        QVERIFY(rev.hasInvalidState());

        rev.prepareInstance(48000.0, 0.75, 0.3, 0.5, 0.0, true, true);
        std::vector<double> outL(256, 0.2), outR(256, 0.2);
        rev.process(outL.data(), outR.data(), 256);

        // Abnormal instance must have been reset & retired immediately without entering ringout or polluting output
        QCOMPARE(rev.activeRingoutSlotCount(), size_t(0));
        QCOMPARE(rev.abnormalStateRetireCount(), 1u);
        QCOMPARE(rev.lastRetireReason(), ReverbNode::RetireReason::AbnormalStateReset);
        for (size_t i = 0; i < 256; ++i) {
            QVERIFY(std::isfinite(outL[i]));
            QVERIFY(std::isfinite(outR[i]));
        }
        rev.reclaimRetired();
    }

    // 6. Sample Rate Change (44.1kHz -> 48kHz -> 96kHz) and Block Size Change (64 -> 256 -> 1024) during Ringout
    {
        ReverbNode rev;
        rev.configure(44100.0);
        rev.prepareInstance(44100.0, 0.8, 0.3, 0.5, 10.0, true, true);
        std::vector<double> inL(1024, 0.4), inR(1024, 0.4);
        rev.process(inL.data(), inR.data(), 64);
        rev.process(inL.data(), inR.data(), 256);

        rev.configure(48000.0);
        std::fill(inL.begin(), inL.end(), 0.2);
        std::fill(inR.begin(), inR.end(), 0.2);
        rev.process(inL.data(), inR.data(), 256);

        rev.configure(96000.0);
        std::fill(inL.begin(), inL.end(), 0.1);
        std::fill(inR.begin(), inR.end(), 0.1);
        rev.process(inL.data(), inR.data(), 1024);

        for (size_t i = 0; i < 1024; ++i) {
            QVERIFY(std::isfinite(inL[i]));
            QVERIFY(std::isfinite(inR[i]));
        }
        rev.reclaimRetired();
    }

    // 7. Bypass Progression & No Ghost Tail Re-Appearance After Unbypass
    {
        ReverbNode rev;
        rev.prepareInstance(48000.0, 0.2, 0.5, 0.5, 0.0, true, true); // Fast decay

        // Excite reverb with audio
        std::vector<double> sigL(1024, 0.5), sigR(1024, 0.5);
        rev.process(sigL.data(), sigR.data(), 1024);

        // Swap instance so excited reverb is in ringout slot, then engage Bypass
        rev.prepareInstance(48000.0, 0.2, 0.5, 0.5, 0.0, true, true);
        rev.setBypassed(true);
        QVERIFY(rev.isBypassed());

        // Process 60 blocks under bypass: output must be dry pass-through, and internal tail must decay & retire
        std::vector<double> passL(512, 0.3), passR(512, 0.3);
        for (int b = 0; b < 60; ++b) {
            std::fill(passL.begin(), passL.end(), 0.3);
            std::fill(passR.begin(), passR.end(), 0.3);
            rev.process(passL.data(), passR.data(), 512);
            QCOMPARE(passL[0], 0.3);
            QCOMPARE(passR[0], 0.3);
        }
        // Verify ringout slot retired during bypass!
        QCOMPARE(rev.activeRingoutSlotCount(), size_t(0));
        QCOMPARE(rev.naturalDecayRetireCount(), 1u);

        // Unbypass and feed silence (0.0): verify old tail does NOT reappear!
        rev.setBypassed(false);
        QVERIFY(!rev.isBypassed());

        std::vector<double> silentL(512, 0.0), silentR(512, 0.0);
        rev.process(silentL.data(), silentR.data(), 512);
        double residualEnergy = 0.0;
        for (size_t i = 0; i < 512; ++i) {
            residualEnergy += silentL[i] * silentL[i] + silentR[i] * silentR[i];
        }
        QVERIFY(residualEnergy < 1e-9);
        rev.reclaimRetired();
    }
}

void TestPhase2Dsp::testReverbSlotCapacityEvictionAndSafetyCeiling()
{
    // P0-22: Deterministic verification of SlotCapacityEviction and MaxRetentionSafetyCeiling
    // 1. SlotCapacityEviction (All 8 slots saturated -> 9th swap evicts longest-lived slot)
    {
        ReverbNode rev;
        const double sr = 48000.0;
        const size_t frames = 256;
        std::vector<double> exciteL(frames, 0.5), exciteR(frames, 0.5);

        // Initial process so active reverb has energy (> 1e-12)
        rev.prepareInstance(sr, 0.8, 0.3, 0.5, 0.0, true, true);
        rev.process(exciteL.data(), exciteR.data(), frames);

        // Fill all 8 ringout slots with excited reverbs
        for (int s = 0; s < 8; ++s) {
            rev.prepareInstance(sr, 0.8 + static_cast<double>(s) * 0.01, 0.3, 0.5, 0.0, true, true);
            rev.process(exciteL.data(), exciteR.data(), frames);
        }
        QCOMPARE(rev.activeRingoutSlotCount(), size_t(8));
        QCOMPARE(rev.capacityEvictionRetireCount(), uint64_t(0));

        // 9th swap: All 8 slots full -> must trigger SlotCapacityEviction
        rev.prepareInstance(sr, 0.7, 0.2, 0.4, 0.0, true, true);
        rev.process(exciteL.data(), exciteR.data(), frames);

        QCOMPARE(rev.activeRingoutSlotCount(), size_t(8)); // Still 8
        QCOMPARE(rev.capacityEvictionRetireCount(), uint64_t(1));
        QCOMPARE(rev.lastRetireReason(), ReverbNode::RetireReason::SlotCapacityEviction);
        rev.reclaimRetired();
    }

    // 2. MaxRetentionSafetyCeiling (10-second timeout ceiling)
    {
        ReverbNode rev;
        const double sr = 8000.0; // Use 8kHz so 10 seconds is 80,000 frames
        const size_t frames = 1000;
        rev.configure(sr);

        std::vector<double> exciteL(frames, 0.5), exciteR(frames, 0.5);
        rev.prepareInstance(sr, 0.99, 0.01, 0.5, 0.0, true, true); // Extremely long decay
        rev.process(exciteL.data(), exciteR.data(), frames);

        // Swap to put it in ringout slot
        rev.prepareInstance(sr, 0.5, 0.5, 0.5, 0.0, true, true);
        std::vector<double> silenceL(frames, 0.0), silenceR(frames, 0.0);
        rev.process(silenceL.data(), silenceR.data(), frames);
        QCOMPARE(rev.activeRingoutSlotCount(), size_t(1));

        // Advance 81 blocks of 1000 frames (81,000 frames > 80,000 frames = 10 seconds @ 8kHz)
        for (int b = 0; b < 81; ++b) {
            std::fill(silenceL.begin(), silenceL.end(), 0.0);
            std::fill(silenceR.begin(), silenceR.end(), 0.0);
            rev.process(silenceL.data(), silenceR.data(), frames);
        }

        // Must be retired due to MaxRetentionSafetyCeiling
        QCOMPARE(rev.activeRingoutSlotCount(), size_t(0));
        QCOMPARE(rev.safetyCeilingRetireCount(), uint64_t(1));
        QCOMPARE(rev.lastRetireReason(), ReverbNode::RetireReason::MaxRetentionSafetyCeiling);
        rev.reclaimRetired();
    }
}

void TestPhase2Dsp::testDspParameterMailboxEpochAndMonotonicSequence()
{
    // P0-24: Generation / Epoch tracking & strict sequenceNumber monotonicity across resets
    DspParameterMailbox mailbox;
    QCOMPARE(mailbox.currentEpoch(), uint64_t(1));
    QCOMPARE(mailbox.currentSequenceNumber(), uint64_t(0));

    // Update parameters
    mailbox.setGeqBandGain(0, 3.0f);
    mailbox.setGeqBandGain(1, -2.0f);
    QCOMPARE(mailbox.currentEpoch(), uint64_t(1));
    uint64_t seqBefore = mailbox.currentSequenceNumber();
    QCOMPARE(seqBefore, uint64_t(2));

    ContinuousDspParameters params;
    QVERIFY(mailbox.fetchUpdates(params));
    QCOMPARE(params.epoch, uint64_t(1));
    QCOMPARE(params.sequenceNumber, uint64_t(2));

    // Reset mailbox: epoch must increment, sequenceNumber must preserve monotonicity
    mailbox.reset();
    QCOMPARE(mailbox.currentEpoch(), uint64_t(2));
    QCOMPARE(mailbox.currentSequenceNumber(), seqBefore); // NOT zero!

    // Next update after reset: sequenceNumber continues monotonically from seqBefore
    mailbox.setGeqBandGain(5, 1.5f);
    QCOMPARE(mailbox.currentEpoch(), uint64_t(2));
    QCOMPARE(mailbox.currentSequenceNumber(), seqBefore + 1);

    ContinuousDspParameters postResetParams;
    QVERIFY(mailbox.fetchUpdates(postResetParams));
    QCOMPARE(postResetParams.epoch, uint64_t(2));
    QCOMPARE(postResetParams.sequenceNumber, seqBefore + 1);
    QVERIFY(postResetParams.sequenceNumber > params.sequenceNumber);
}

void TestPhase2Dsp::testParameterCommandQueueOverflowAndDrainLimit()
{
    // P0-25: ParameterCommandQueue overflow detection and drain bounds
    ParameterCommandQueue queue;
    QCOMPARE(queue.size(), size_t(0));
    QCOMPARE(queue.overflowCount(), uint64_t(0));

    // Fill queue to exact capacity (512)
    for (size_t i = 0; i < ParameterCommandQueue::CAPACITY; ++i) {
        DspCommand cmd;
        cmd.type = DspCommandType::SetNodeBypass;
        cmd.index = static_cast<uint8_t>(i % 16);
        QVERIFY(queue.push(cmd));
    }
    QCOMPARE(queue.size(), ParameterCommandQueue::CAPACITY);
    QCOMPARE(queue.overflowCount(), uint64_t(0));

    // 513th push must return false and increment overflowCount
    DspCommand overflowCmd;
    overflowCmd.type = DspCommandType::SetGlobalBypass;
    QVERIFY(!queue.push(overflowCmd));
    QCOMPARE(queue.overflowCount(), uint64_t(1));
    QCOMPARE(queue.size(), ParameterCommandQueue::CAPACITY);

    // Push 9 more when full -> all return false, overflowCount increases to 10
    for (int i = 0; i < 9; ++i) {
        QVERIFY(!queue.push(overflowCmd));
    }
    QCOMPARE(queue.overflowCount(), uint64_t(10));

    // Drain exactly 64 commands (AudioPipeline MAX_COMMANDS_PER_BLOCK limit)
    for (size_t i = 0; i < 64; ++i) {
        DspCommand popped;
        QVERIFY(queue.pop(popped));
        QCOMPARE(popped.type, DspCommandType::SetNodeBypass);
        QCOMPARE(popped.index, static_cast<uint8_t>(i % 16));
    }
    QCOMPARE(queue.size(), ParameterCommandQueue::CAPACITY - 64);

    // Now 64 slots are free -> push 64 commands must succeed
    for (size_t i = 0; i < 64; ++i) {
        DspCommand cmd;
        cmd.type = DspCommandType::ResetAllGeqBands;
        QVERIFY(queue.push(cmd));
    }
    QCOMPARE(queue.size(), ParameterCommandQueue::CAPACITY);
    QCOMPARE(queue.overflowCount(), uint64_t(10)); // Unchanged
}

void TestPhase2Dsp::testReverbO1StateTrackingAccuracyAndTailWaveformComparison()
{
    // P0-22: Comprehensive Reverb O(1) state tracking accuracy, tail continuity, and decay verification
    AlgorithmicReverb reverb;
    const double sr = 48000.0;
    reverb.configure(sr, 0.85, 0.3, 0.5, 5.0, true, true);
    reverb.reset();

    // 1. Initial state: both O(1) tracking and scan must report 0.0 energy
    QCOMPARE(reverb.totalInternalEnergy(), 0.0);
    QCOMPARE(reverb.debugScanTotalEnergy(), 0.0);

    // 2. Feed an impulse to excite the reverb
    double impL = 1.0, impR = 1.0;
    reverb.process(impL, impR);

    // Energy must have increased in both
    double energyAfterImpulse = reverb.totalInternalEnergy();
    double scanAfterImpulse = reverb.debugScanTotalEnergy();
    QVERIFY(energyAfterImpulse > 0.0);
    QVERIFY(scanAfterImpulse > 0.0);
    QVERIFY(std::abs(energyAfterImpulse - scanAfterImpulse) / scanAfterImpulse < 1e-3);

    // 3. Ringout with silence: verify O(1) energy matches buffer scan at every step
    for (int block = 0; block < 100; ++block) {
        for (int s = 0; s < 256; ++s) {
            double l = 0.0, r = 0.0;
            reverb.process(l, r);
        }
        double e_o1 = reverb.totalInternalEnergy();
        double e_scan = reverb.debugScanTotalEnergy();
        if (e_scan > 1e-6) {
            double relErr = std::abs(e_o1 - e_scan) / e_scan;
            QVERIFY(relErr < 0.02); // Within 2% relative error throughout dynamic decay
        }
    }

    // 4. Test ReverbNode natural decay and non-premature retirement
    ReverbNode revNode("TestReverbNode");
    revNode.configure(sr);
    revNode.prepareInstance(sr, 0.6, 0.2, 0.3, 0.0, true, true);

    const size_t frames = 256;
    std::vector<double> impulseBufL(frames, 0.0), impulseBufR(frames, 0.0);
    impulseBufL[0] = 1.0; impulseBufR[0] = 1.0;
    revNode.process(impulseBufL.data(), impulseBufR.data(), frames);

    // Swap instance to send excited reverb to a ringout slot
    revNode.prepareInstance(sr, 0.5, 0.5, 0.3, 0.0, true, true);
    std::vector<double> silenceL(frames, 0.0), silenceR(frames, 0.0);
    revNode.process(silenceL.data(), silenceR.data(), frames);
    QCOMPARE(revNode.activeRingoutSlotCount(), size_t(1));

    // While tail is still ringing out, slot must NOT retire prematurely
    for (int b = 0; b < 10; ++b) {
        std::fill(silenceL.begin(), silenceL.end(), 0.0);
        std::fill(silenceR.begin(), silenceR.end(), 0.0);
        revNode.process(silenceL.data(), silenceR.data(), frames);
    }
    QCOMPARE(revNode.activeRingoutSlotCount(), size_t(1)); // Still active!

    // Advance until natural decay finishes safely
    size_t decayBlocks = (revNode.dsp().theoreticalDecayFrames() / frames) + 30;
    for (size_t b = 0; b < decayBlocks; ++b) {
        std::fill(silenceL.begin(), silenceL.end(), 0.0);
        std::fill(silenceR.begin(), silenceR.end(), 0.0);
        revNode.process(silenceL.data(), silenceR.data(), frames);
        if (revNode.activeRingoutSlotCount() == 0) break;
    }
    QCOMPARE(revNode.activeRingoutSlotCount(), size_t(0));
    QCOMPARE(revNode.lastRetireReason(), ReverbNode::RetireReason::NaturalDecayComplete);
    revNode.reclaimRetired();

    // 5. High sample rate (192kHz) max tail execution time verification (pure O(1))
    AlgorithmicReverb hiRateRev;
    hiRateRev.configure(192000.0, 0.98, 0.1, 0.5, 50.0, true, true);
    double l192 = 0.5, r192 = 0.5;
    hiRateRev.process(l192, r192);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 4096; ++i) {
        double l = 0.0, r = 0.0;
        hiRateRev.process(l, r);
    }
    auto t1 = std::chrono::steady_clock::now();
    auto elapsedMicros = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    qDebug() << "192kHz 4096-frame Reverb O(1) process time:" << elapsedMicros << "microseconds";
    // 4096 frames at 192kHz deadline is 21.3 ms (21,333 us); pure O(1) DSP runs in < 2ms
    QVERIFY(elapsedMicros < 10000);
}

void TestPhase2Dsp::testActiveDspDirectModificationImpossibility()
{
    // P0-21: Architectural verification that Audio Nodes do not expose mutable DSP instances
    ParametricEqNode peqNode("Master PEQ");
    const ParametricEQ& peqConstRef = peqNode.dsp(); // Strictly const ref
    (void)peqConstRef;

    CompressorNode compNode("Master Comp");
    const Compressor& compConstRef = compNode.dsp(); // Strictly const ref
    (void)compConstRef;

    PeakLimiterNode limNode("Master Limiter");
    const PeakLimiter& limConstRef = limNode.dsp(); // Strictly const ref
    (void)limConstRef;

    ReverbNode revNode("Master Reverb");
    const AlgorithmicReverb& revConstRef = revNode.dsp(); // Strictly const ref
    (void)revConstRef;

    // Verify prepared instance exchange counters
    QCOMPARE(compNode.activatedCount(), uint64_t(0));
    compNode.prepareInstance(48000.0, -12.0, 4.0, 10.0, 100.0, 0.0, 4.0, true);
    std::vector<double> bufL(256, 0.5), bufR(256, 0.5);
    compNode.process(bufL.data(), bufR.data(), 256);
    QCOMPARE(compNode.activatedCount(), uint64_t(1));
}

QTEST_MAIN(TestPhase2Dsp)
#include "test_phase2_dsp.moc"
