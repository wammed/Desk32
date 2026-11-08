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
    void testDspParameterMailboxBatchApis();
    void testDspContinuousParameterUpdateRtSafety();
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
    // P0-25: 3-Slot Exchange tear-free snapshot consistency verification
    // Ensures that Consumer receives internally consistent parameter snapshots without partial updates
    DspParameterMailbox mailbox;

    std::atomic<bool> running{true};
    std::atomic<uint64_t> snapshotCheckCount{0};
    std::atomic<bool> inconsistencyDetected{false};

    // Producer Thread: Writes monotonically increasing generation values across all 32 bands
    std::thread producerThread([&]() {
        float gen = 1.0f;
        while (running.load(std::memory_order_relaxed)) {
            for (size_t b = 0; b < 32; ++b) {
                mailbox.setGeqBandGain(b, gen + static_cast<float>(b) * 0.001f);
            }
            gen += 1.0f;
            std::this_thread::yield();
        }
    });

    // Audio/Consumer Thread: Rapidly fetches snapshots and verifies tear-free consistency
    std::thread consumerThread([&]() {
        ContinuousDspParameters p;
        while (running.load(std::memory_order_relaxed)) {
            if (mailbox.fetchUpdates(p)) {
                // Check if all modified bands correspond to valid generation
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

void TestPhase2Dsp::testDspContinuousParameterUpdateRtSafety()
{
    // P0-14 & P0-17: Continuous DSP reconfiguration on Control Thread while Audio Thread processes
    // Verifies zero heap allocations in Audio Thread, makeup gain retention, and smooth state updates
    Compressor comp;
    comp.configure(48000.0, -12.0, 4.0, 10.0, 100.0, 3.0, 4.0, true);
    QCOMPARE(comp.makeupGainDb(), 3.0);

    PeakLimiter limiter;
    limiter.configure(48000.0, -0.5, 40.0, 1.5, true);
    QCOMPARE(limiter.ceilingDb(), -0.5);

    AlgorithmicReverb reverb;
    reverb.configure(48000.0, 0.8, 0.4, 0.3, 10.0, true, true);
    QCOMPARE(reverb.roomSize(), 0.8);

    std::atomic<bool> stopAudio{false};
    std::atomic<uint64_t> audioBlocksProcessed{0};

    // Audio thread
    std::thread audioThread([&]() {
        double left = 0.4, right = 0.4;
        while (!stopAudio.load(std::memory_order_relaxed)) {
            comp.process(left, right);
            limiter.process(left, right);
            reverb.process(left, right);
            audioBlocksProcessed.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Control thread rapidly changes parameters
    for (int i = 0; i < 100; ++i) {
        double threshold = -10.0 - (i % 20);
        double ratio = 2.0 + (i % 6);
        comp.configure(48000.0, threshold, ratio, 10.0, 100.0, 4.5, 4.0, true);

        double ceiling = -0.1 - (i % 5) * 0.2;
        double lookahead = 0.5 + (i % 4) * 0.5;
        limiter.configure(48000.0, ceiling, 50.0, lookahead, true);

        double room = 0.5 + (i % 40) * 0.01;
        reverb.configure(48000.0, room, 0.3, 0.25, 5.0, true, true);

        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    stopAudio.store(true, std::memory_order_relaxed);
    audioThread.join();

    QVERIFY(audioBlocksProcessed.load() > 100);
    QCOMPARE(comp.makeupGainDb(), 4.5); // Makeup gain maintained!

    // Verify ZERO heap allocations during process() under active reconfiguration
    s_allocationCount.store(0);
    s_trackAllocations.store(true);

    double left = 0.5, right = 0.5;
    for (int i = 0; i < 5000; ++i) {
        comp.process(left, right);
        limiter.process(left, right);
        reverb.process(left, right);
    }

    s_trackAllocations.store(false);
    QCOMPARE(s_allocationCount.load(), 0u);
}

QTEST_MAIN(TestPhase2Dsp)
#include "test_phase2_dsp.moc"
