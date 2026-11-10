#include <QTest>
#include <QObject>
#include <QSignalSpy>
#include <QTemporaryDir>
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
#include "core/EngineController.hpp"
#include "core/log/LogService.hpp"

using namespace desk32::engine::dsp;
using namespace desk32::engine::pipeline;

// Heap allocation tracker for real-time safety verification
// Scope of Interception (P1-4-AC-08, P1-4-R1):
// Intercepted C++ Dynamic Allocations:
//   - operator new(size_t)
//   - operator new(size_t, const std::nothrow_t&)
//   - operator new[](size_t)
//   - operator new[](size_t, const std::nothrow_t&)
//   - operator new(size_t, std::align_val_t)
//   - operator new[](size_t, std::align_val_t)
//   - operator new(size_t, std::align_val_t, const std::nothrow_t&)
//   - operator new[](size_t, std::align_val_t, const std::nothrow_t&)
// Outside Scope / Uninstrumented:
//   - Direct C-runtime std::malloc / calloc / realloc / free
//   - Uninstrumented external C library internal heap allocations
static std::atomic<bool> s_trackAllocations{false};
static std::atomic<size_t> s_allocationCount{0};
static thread_local bool t_isAudioThread{false};
static std::atomic<size_t> s_audioThreadAllocationCount{0};

// Specific category counters for Audio Thread
static std::atomic<size_t> s_audioScalarNewCount{0};
static std::atomic<size_t> s_audioArrayNewCount{0};
static std::atomic<size_t> s_audioAlignedNewCount{0};
static std::atomic<size_t> s_audioAlignedNothrowNewCount{0};

/// RAII Scope Guard to strictly delineate Audio Thread execution sections (P1-4 Section 5.3)
struct AudioThreadScopeGuard {
    AudioThreadScopeGuard() noexcept { t_isAudioThread = true; }
    ~AudioThreadScopeGuard() noexcept { t_isAudioThread = false; }
    AudioThreadScopeGuard(const AudioThreadScopeGuard&) = delete;
    AudioThreadScopeGuard& operator=(const AudioThreadScopeGuard&) = delete;
};

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(size_t size) {
    if (s_trackAllocations.load(std::memory_order_relaxed)) {
        if (t_isAudioThread) {
            s_audioScalarNewCount.fetch_add(1, std::memory_order_relaxed);
            s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
        }
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void* operator new(size_t size, const std::nothrow_t&) noexcept {
    if (s_trackAllocations.load(std::memory_order_relaxed)) {
        if (t_isAudioThread) {
            s_audioScalarNewCount.fetch_add(1, std::memory_order_relaxed);
            s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
        }
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void* operator new[](size_t size) {
    if (s_trackAllocations.load(std::memory_order_relaxed)) {
        if (t_isAudioThread) {
            s_audioArrayNewCount.fetch_add(1, std::memory_order_relaxed);
            s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
        }
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    if (s_trackAllocations.load(std::memory_order_relaxed)) {
        if (t_isAudioThread) {
            s_audioArrayNewCount.fetch_add(1, std::memory_order_relaxed);
            s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
        }
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void* operator new(size_t size, std::align_val_t al) {
    if (s_trackAllocations.load(std::memory_order_relaxed)) {
        if (t_isAudioThread) {
            s_audioAlignedNewCount.fetch_add(1, std::memory_order_relaxed);
            s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
        }
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
        if (t_isAudioThread) {
            s_audioAlignedNewCount.fetch_add(1, std::memory_order_relaxed);
            s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
        }
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* ptr = nullptr;
    if (posix_memalign(&ptr, static_cast<size_t>(al), size) != 0) {
        throw std::bad_alloc();
    }
    return ptr;
}

void* operator new(size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
    if (s_trackAllocations.load(std::memory_order_relaxed)) {
        if (t_isAudioThread) {
            s_audioAlignedNothrowNewCount.fetch_add(1, std::memory_order_relaxed);
            s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
        }
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* ptr = nullptr;
    if (posix_memalign(&ptr, static_cast<size_t>(al), size) != 0) {
        return nullptr;
    }
    return ptr;
}

void* operator new[](size_t size, std::align_val_t al, const std::nothrow_t&) noexcept {
    if (s_trackAllocations.load(std::memory_order_relaxed)) {
        if (t_isAudioThread) {
            s_audioAlignedNothrowNewCount.fetch_add(1, std::memory_order_relaxed);
            s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
        }
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* ptr = nullptr;
    if (posix_memalign(&ptr, static_cast<size_t>(al), size) != 0) {
        return nullptr;
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
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, size_t, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, size_t, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

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
    void testDspParametersReflectedAndAuditioned();
    void testIndividualDspNodeBypassVsGlobalBypassNonDestructive();
    void testMasterMuteSmoothingAndTelemetry();
    void testMasterMutePcmAttenuationIntegration();
    void testAudioThreadZeroAllocationDuringContinuousDspUpdates();
    void testAudioThreadAllocationTrackerOverloadsAndIntentionalDetection();
    void testDspParameterMailboxConsumedAppliedAndSupersededConsistency();
    void testDspNodeBypassParameterPreservation();
    void cleanupTestCase();

private:
    std::unique_ptr<QTemporaryDir> m_tempDir;
};

void TestPhase2Dsp::initTestCase()
{
    qDebug() << "Initializing TestPhase2Dsp (Phase 2 Acoustic Correction & Built-in DSP Verification)...";
    m_tempDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tempDir->isValid());
    QString dbPath = m_tempDir->path() + "/test_phase2_log.db";
    desk32::core::log::LogService::instance().start(dbPath);
}

void TestPhase2Dsp::cleanupTestCase()
{
    desk32::core::log::LogService::instance().stop();
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
    // 4096 frames at 192kHz deadline is 21.3 ms (21,333 us); native Release runs in < 2ms
#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_UNDEFINED__) && !defined(DESK32_SANITIZE_UNDEFINED) && defined(NDEBUG)
    QVERIFY(elapsedMicros < 10000);
#else
    QVERIFY(elapsedMicros < 21333);
#endif
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

void TestPhase2Dsp::testDspParametersReflectedAndAuditioned() {
    // 1. HighLowPassFilter response verification
    HighLowPassFilter filter;
    filter.configure(48000.0, 250.0, 7500.0,
                     HighLowPassFilter::FilterSlope::Slope24dB,
                     HighLowPassFilter::FilterSlope::Slope24dB);
    filter.setHpfEnabled(true);
    filter.setLpfEnabled(true);

    std::vector<double> freqs = { 50.0, 1000.0, 15000.0 };
    std::vector<double> outDb;
    filter.calculateResponseCurve(freqs, outDb);

    QCOMPARE(outDb.size(), size_t(3));
    QVERIFY(outDb[0] < -12.0); // 50 Hz attenuated by 24 dB/oct HPF at 250 Hz
    QVERIFY(std::abs(outDb[1]) < 1.0); // 1000 Hz passband
    QVERIFY(outDb[2] < -12.0); // 15000 Hz attenuated by 24 dB/oct LPF at 7500 Hz

    // 2. Compressor dynamics & gain reduction
    Compressor comp;
    comp.configure(48000.0, -24.0, 4.0, 5.0, 100.0, 0.0, 2.0, true);
    std::vector<double> cInL(512, 1.0), cInR(512, 1.0);
    for (size_t s = 0; s < 512; ++s) {
        comp.process(cInL[s], cInR[s]);
    }
    QVERIFY(comp.gainReductionDb() > 10.0);
    QVERIFY(cInL[511] < 0.6); // Signal compressed

    // 3. Peak Limiter brickwall ceiling
    PeakLimiter lim;
    lim.configure(48000.0, -1.5, 30.0, 2.5, true); // -1.5 dB ceiling = 0.8414
    std::vector<double> lInL(512, 1.5), lInR(512, 1.5);
    for (size_t s = 0; s < 512; ++s) {
        lim.process(lInL[s], lInR[s]);
    }
    for (size_t s = 120; s < 512; ++s) {
        QVERIFY(std::abs(lInL[s]) <= 0.842);
        QVERIFY(std::abs(lInR[s]) <= 0.842);
    }

    // 4. Algorithmic Reverb impulse and tail audition
    AlgorithmicReverb rev;
    rev.configure(48000.0, 0.8, 0.5, 0.5, 10.0, true, true);
    std::vector<double> rInL(256, 0.0), rInR(256, 0.0);
    rInL[0] = 1.0; rInR[0] = 1.0; // Impulse
    for (size_t s = 0; s < 256; ++s) {
        rev.process(rInL[s], rInR[s]);
    }
    QVERIFY(rev.totalInternalEnergy() > 0.0);
}

void TestPhase2Dsp::testIndividualDspNodeBypassVsGlobalBypassNonDestructive() {
    AudioChain chain("Test Master Chain", ChainPurpose::MasteringChain);
    auto filterNode = std::make_shared<HighLowPassNode>("Filter Node");
    auto compNode = std::make_shared<CompressorNode>("Comp Node");
    auto limNode = std::make_shared<PeakLimiterNode>("Limiter Node");
    auto revNode = std::make_shared<ReverbNode>("Reverb Node");

    chain.appendNode(filterNode);
    chain.appendNode(compNode);
    chain.appendNode(limNode);
    chain.appendNode(revNode);

    // Set individual node bypass states
    filterNode->setBypassed(false); // Active
    compNode->setBypassed(false);   // Active
    revNode->setBypassed(true);     // Bypassed
    limNode->setBypassed(false);    // Active

    QCOMPARE(filterNode->isBypassed(), false);
    QCOMPARE(compNode->isBypassed(), false);
    QCOMPARE(revNode->isBypassed(), true);
    QCOMPARE(limNode->isBypassed(), false);

    // Engage Global DSP Bypass on the chain (Section 10.2.3 & 5.9)
    chain.setBypassed(true);
    QCOMPARE(chain.isBypassed(), true);

    // Critical assertion: Global DSP Bypass must NOT modify or overwrite individual node bypass states!
    QCOMPARE(filterNode->isBypassed(), false);
    QCOMPARE(compNode->isBypassed(), false);
    QCOMPARE(revNode->isBypassed(), true);
    QCOMPARE(limNode->isBypassed(), false);

    // Process signal while globally bypassed: audio must pass through completely untouched
    std::vector<double> bufL(256, 0.777), bufR(256, -0.444);
    chain.process(bufL.data(), bufR.data(), 256);
    QCOMPARE(bufL[0], 0.777);
    QCOMPARE(bufR[0], -0.444);

    // Disengage Global DSP Bypass
    chain.setBypassed(false);
    QCOMPARE(chain.isBypassed(), false);

    // Verify all individual node bypass states are cleanly preserved
    QCOMPARE(filterNode->isBypassed(), false);
    QCOMPARE(compNode->isBypassed(), false);
    QCOMPARE(revNode->isBypassed(), true); // Remains bypassed!
    QCOMPARE(limNode->isBypassed(), false);
}

void TestPhase2Dsp::testMasterMuteSmoothingAndTelemetry() {
    // 1. ParameterSmoother behavior during Master Mute (smooth ramp down, click-free)
    ParameterSmoother smoother(1.0, 15.0);
    smoother.configure(48000.0, 15.0);
    smoother.reset(1.0);

    // Mute: ramp to 0.0
    smoother.setTarget(0.0);
    std::vector<double> sBuf(256, 1.0);
    smoother.process(sBuf.data(), 256);

    // Check monotonic decrease
    for (size_t i = 1; i < 256; ++i) {
        QVERIFY(sBuf[i] <= sBuf[i - 1]);
    }
    QVERIFY(sBuf[255] < 0.8); // Smoothly attenuated

    // 2. AudioPipeline Master Level, RMS & Clip Telemetry
    AudioPipeline pipeline(48000.0);

    std::vector<double> inL(256, 0.5), inR(256, 0.25);
    std::vector<float> outPcm(256 * 2);

    // Process normal block (within 0 dBFS headroom)
    pipeline.process(inL.data(), inR.data(), 256, outPcm.data(), desk32::engine::format::SampleFormat::Float32, false);

    auto telem = pipeline.currentTelemetry();
    QVERIFY(telem.masterPeakL > 0.49 && telem.masterPeakL < 0.51);
    QVERIFY(telem.masterPeakR > 0.24 && telem.masterPeakR < 0.26);
    QVERIFY(telem.masterRmsL > 0.49 && telem.masterRmsL < 0.51);
    QCOMPARE(telem.masterClipped, false);

    // Process hot signal that clips (>= 1.0)
    std::vector<double> hotL(256, 1.2), hotR(256, 0.8);
    pipeline.process(hotL.data(), hotR.data(), 256, outPcm.data(), desk32::engine::format::SampleFormat::Float32, false);

    auto telemHot = pipeline.currentTelemetry();
    QVERIFY(telemHot.masterPeakL >= 1.0);
    QCOMPARE(telemHot.masterClipped, true);

    // Verify zero allocations during telemetry and parameter changes
    s_trackAllocations.store(true, std::memory_order_relaxed);
    s_allocationCount.store(0, std::memory_order_relaxed);

    for (int i = 0; i < 1000; ++i) {
        pipeline.process(inL.data(), inR.data(), 256, outPcm.data(), desk32::engine::format::SampleFormat::Float32, false);
        auto t = pipeline.currentTelemetry();
        (void)t;
    }

    s_trackAllocations.store(false, std::memory_order_relaxed);
    QCOMPARE(s_allocationCount.load(), size_t(0));
}

void TestPhase2Dsp::testMasterMutePcmAttenuationIntegration() {
    // End-to-end integration test of Master Mute via EngineController APIs with actual AudioPipeline (P1-3)
    desk32::core::EngineController& controller = desk32::core::EngineController::instance();
    QVERIFY(controller.initialize(true));
    if (controller.audioOutput()) {
        controller.audioOutput()->stop();
        controller.audioOutput()->setOfflineSimulationMode(true);
    }

    // 1. Initial State: Unmuted, Master Gain = 1.0
    controller.setMasterGain(1.0);
    controller.setMasterMute(false);
    QCOMPARE(controller.masterGain(), 1.0);
    QCOMPARE(controller.isMasterMute(), false);

    // Setup synthetic 440Hz reference tone on Deck A
    desk32::engine::deck::TrackInfo synthInfo;
    synthInfo.path = "synthetic://sine_440";
    synthInfo.sampleRate = 48000.0;
    synthInfo.channels = 2;
    synthInfo.totalFrames = 480000; // 10 seconds of 440Hz sine

    auto* deckA = controller.deckManager()->deck('A');
    QVERIFY(deckA != nullptr);
    QVERIFY(deckA->loadTrack(synthInfo, 48000.0));
    controller.play("A");
    QCOMPARE(deckA->state(), desk32::engine::deck::TransportState::Playing);

    // Wait until background DecoderWorker has primed the ring buffer with decoded audio
    int waitCount = 0;
    while (deckA->worker().ringBuffer().availableRead() < 8192 && waitCount++ < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    QVERIFY(deckA->worker().ringBuffer().availableRead() >= 4096);

    const size_t blockSize = 128;
    std::vector<float> outPcm(blockSize * 2);

    // Prime the pipeline to steady-state playback (fill lookahead buffers and decoder ring buffers)
    double steadyMax = 0.0;
    for (int p = 0; p < 25; ++p) {
        controller.renderOfflineBlock(outPcm.data(), blockSize);
        for (float s : outPcm) {
            steadyMax = std::max(steadyMax, std::abs(static_cast<double>(s)));
        }
    }
    qDebug() << "steadyMax:" << steadyMax;
    QVERIFY(steadyMax > 0.1); // Confirmed active steady-state playback

    // 2. Engage Master Mute via EngineController::setMasterMute(true)
    QSignalSpy muteSpy(&controller, &desk32::core::EngineController::masterMuteChanged);
    controller.setMasterMute(true);
    QCOMPARE(controller.isMasterMute(), true);
    QCOMPARE(muteSpy.count(), 1);

    // Process blocks through EngineController until smooth decay and settling (20 chunks of 4*128 frames = 213 ms > 14 tau)
    double prevChunkMax = 2.0;
    for (int step = 0; step < 20; ++step) {
        double chunkMax = 0.0;
        for (int b = 0; b < 4; ++b) {
            controller.renderOfflineBlock(outPcm.data(), blockSize);
            double bMax = 0.0;
            for (float sample : outPcm) {
                bMax = std::max(bMax, std::abs(static_cast<double>(sample)));
            }
            chunkMax = std::max(chunkMax, bMax);

            // Check for click/pop: sample-to-sample difference should be bounded (< 0.05 per sample)
            for (size_t i = 2; i < blockSize * 2; i += 2) {
                double deltaL = std::abs(outPcm[i] - outPcm[i - 2]);
                double deltaR = std::abs(outPcm[i + 1] - outPcm[i - 1]);
                QVERIFY(deltaL < 0.05);
                QVERIFY(deltaR < 0.05);
            }
        }

        // Smooth monotonic decay across successive time windows
        QVERIFY(chunkMax <= prevChunkMax + 1e-5);
        prevChunkMax = chunkMax;
    }

    // After settling (>14 tau), ParameterSmoother inside EngineController snaps to 0.0: output must be strictly silent (< 1e-7, >140 dB attenuation)
    controller.renderOfflineBlock(outPcm.data(), blockSize);
    for (size_t i = 0; i < blockSize * 2; ++i) {
        QVERIFY(std::abs(outPcm[i]) < 1e-7f);
    }

    // 3. While muted, modify Master Gain via EngineController::setMasterGain(1.5)
    controller.setMasterGain(1.5);
    QCOMPARE(controller.masterGain(), 1.5);

    // Verify output remains strictly silent while mute is active
    controller.renderOfflineBlock(outPcm.data(), blockSize);
    for (size_t i = 0; i < blockSize * 2; ++i) {
        QVERIFY(std::abs(outPcm[i]) < 1e-7f);
    }

    // 4. Disengage Master Mute via EngineController::setMasterMute(false)
    controller.setMasterMute(false);
    QCOMPARE(controller.isMasterMute(), false);
    QCOMPARE(controller.masterGain(), 1.5); // Master Gain 1.5 was preserved across mute!

    // Render recovery blocks: smooth ramp up towards gain 1.5
    for (int b = 0; b < 60; ++b) {
        controller.renderOfflineBlock(outPcm.data(), blockSize);

        for (size_t i = 2; i < blockSize * 2; i += 2) {
            double deltaL = std::abs(outPcm[i] - outPcm[i - 2]);
            double deltaR = std::abs(outPcm[i + 1] - outPcm[i - 1]);
            QVERIFY(deltaL < 0.05);
            QVERIFY(deltaR < 0.05);
        }
    }

    // After full ramp up, measure peak across multiple blocks to capture the sine wave cycle
    double unmutedMax = 0.0;
    for (int b = 0; b < 10; ++b) {
        controller.renderOfflineBlock(outPcm.data(), blockSize);
        for (float sample : outPcm) {
            unmutedMax = std::max(unmutedMax, std::abs(static_cast<double>(sample)));
        }
    }
    QVERIFY(unmutedMax > steadyMax * 1.1); // Amplified by 1.5 gain

    // 5. Verify DSP Node & Global Bypass independence
    QCOMPARE(controller.globalDspBypass(), false);
    QCOMPARE(controller.isDspNodeBypassed("Filter"), true);
    QCOMPARE(controller.isDspNodeAppliedBypassed("Filter"), true);

    // Clean up
    controller.stop("A");
    controller.setMasterGain(1.0);
}

void TestPhase2Dsp::testAudioThreadZeroAllocationDuringContinuousDspUpdates() {
    // P1-4: Verify strictly ZERO C++ dynamic memory allocations (operator new / operator new[]) in Audio Thread
    // while Control Thread issues continuous high-frequency DSP parameter updates across all nodes.
    //
    // Note on Allocation Interception Scope (P1-4-AC-08):
    // Intercepted C++ Dynamic Allocations:
    //   - operator new(size_t)
    //   - operator new(size_t, const std::nothrow_t&)
    //   - operator new[](size_t)
    //   - operator new[](size_t, const std::nothrow_t&)
    //   - operator new(size_t, std::align_val_t)
    //   - operator new[](size_t, std::align_val_t)
    // Outside Scope / Uninstrumented:
    //   - Direct C-runtime std::malloc / calloc / realloc / free
    //   - Uninstrumented external C library internal heap allocations
    AudioPipeline pipeline(48000.0);

    // Build realistic Mastering and Listening chains
    auto masterChain = std::make_unique<AudioChain>("Master Chain", ChainPurpose::MasteringChain);
    masterChain->appendNode(std::make_shared<ParametricEqNode>("PEQ"));
    masterChain->appendNode(std::make_shared<GraphicEqNode>("GEQ"));
    masterChain->appendNode(std::make_shared<HighLowPassNode>("Filter"));
    masterChain->appendNode(std::make_shared<CompressorNode>("Compressor"));
    masterChain->appendNode(std::make_shared<ReverbNode>("Reverb"));
    masterChain->appendNode(std::make_shared<PeakLimiterNode>("Limiter"));
    pipeline.swapMasterChain(std::move(masterChain));

    auto listeningChain = std::make_unique<AudioChain>("Listening Chain", ChainPurpose::ListeningCorrectionChain);
    listeningChain->appendNode(std::make_shared<ParametricEqNode>("Listening Correction"));
    pipeline.swapListeningChain(std::move(listeningChain));

    std::atomic<bool> stopAudioThread{false};
    std::atomic<uint64_t> blocksProcessed{0};

    // Reset pipeline statistics and allocation trackers
    pipeline.resetStatistics();
    s_audioThreadAllocationCount.store(0, std::memory_order_relaxed);
    s_audioScalarNewCount.store(0, std::memory_order_relaxed);
    s_audioArrayNewCount.store(0, std::memory_order_relaxed);
    s_audioAlignedNewCount.store(0, std::memory_order_relaxed);
    s_audioAlignedNothrowNewCount.store(0, std::memory_order_relaxed);
    s_allocationCount.store(0, std::memory_order_relaxed);
    s_trackAllocations.store(true, std::memory_order_relaxed);

    const uint64_t initialMailboxCount = pipeline.paramMailbox().updateCount();

    // Spin up Audio Thread
    std::thread audioThread([&]() {
        const size_t frames = 256;
        std::array<double, frames> left;
        std::array<double, frames> right;
        std::array<float, frames * 2> out;
        left.fill(0.5);
        right.fill(-0.5);
        out.fill(0.0f);

        while (!stopAudioThread.load(std::memory_order_acquire)) {
            {
                AudioThreadScopeGuard guard;
                pipeline.process(left.data(), right.data(), frames, out.data(),
                                 desk32::engine::format::SampleFormat::Float32, false);
            }
            blocksProcessed.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    uint64_t commandUpdatesRequested = 0;
    uint64_t commandUpdatesEnqueued = 0;
    uint64_t commandUpdatesRejected = 0;

    // Control Thread: Issue 1,000 rapid updates across all DSP modules
    for (int i = 0; i < 1000; ++i) {
        // Continuous Mailbox updates: Filter and Listening preset
        pipeline.paramMailbox().setFilterParams(
            static_cast<float>(20.0 + (i % 200)),
            static_cast<float>(10000.0 - (i % 500)),
            1, 0, true, true
        );
        if (i % 10 == 0) {
            pipeline.paramMailbox().setListeningPreset("Monitor Flat");
        }

        // Discrete Command updates (Compressor, Limiter, Reverb, Filter bypass / params)
        desk32::engine::dsp::DspCommand cmd;
        if (i % 4 == 0) {
            cmd.type = desk32::engine::dsp::DspCommandType::SetNodeBypass;
            cmd.index = 2; // Filter
            cmd.boolFlag1 = (i % 8 == 0) ? 1 : 0;
        } else if (i % 4 == 1) {
            cmd.type = desk32::engine::dsp::DspCommandType::SetNodeBypass;
            cmd.index = 3; // Compressor
            cmd.boolFlag1 = (i % 8 == 1) ? 1 : 0;
        } else if (i % 4 == 2) {
            cmd.type = desk32::engine::dsp::DspCommandType::SetNodeBypass;
            cmd.index = 5; // Limiter
            cmd.boolFlag1 = (i % 8 == 2) ? 1 : 0;
        } else {
            cmd.type = desk32::engine::dsp::DspCommandType::SetGlobalBypass;
            cmd.boolFlag1 = (i % 16 == 3) ? 1 : 0;
        }

        commandUpdatesRequested++;
        if (pipeline.paramQueue().push(cmd)) {
            commandUpdatesEnqueued++;
        } else {
            commandUpdatesRejected++;
        }

        // Prepared instance updates on Control Thread
        if (i % 10 == 0) {
            pipeline.updateCompressor(-12.0 - (i % 10), 3.0, 10.0, 100.0, 2.0, 4.0, true);
            pipeline.updatePeakLimiter(-1.0 - (i % 3) * 0.5, 50.0, 3.0, true);
            pipeline.updateReverb(0.6 + (i % 4) * 0.1, 0.5, 0.2, 20.0, true, true);
        }

        if (i % 50 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    // P1-4-AC-01, P1-4-AC-02, P1-4-AC-03:
    // Guarantee processing of >= 1,000 Audio Processing Blocks with timeout detection
    constexpr uint64_t kRequiredAudioBlocks = 1000;
    constexpr auto kTimeout = std::chrono::seconds(5);
    const auto startTime = std::chrono::steady_clock::now();
    bool timedOut = false;

    while (blocksProcessed.load(std::memory_order_acquire) < kRequiredAudioBlocks) {
        if (std::chrono::steady_clock::now() - startTime > kTimeout) {
            timedOut = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    stopAudioThread.store(true, std::memory_order_release);
    audioThread.join();
    s_trackAllocations.store(false, std::memory_order_relaxed);

    // Collect structured metrics
    const uint64_t totalBlocks = blocksProcessed.load(std::memory_order_relaxed);
    const uint64_t mbPublished = pipeline.paramMailbox().updateCount() - initialMailboxCount;
    const uint64_t mbConsumed = pipeline.mailboxConsumed();
    const uint64_t mbApplied = pipeline.mailboxApplied();
    const uint64_t mbPending = pipeline.paramMailbox().pendingUpdatesCount();
    const uint64_t mbSuperseded = (mbPublished >= (mbConsumed + mbPending))
        ? (mbPublished - mbConsumed - mbPending)
        : 0;

    const uint64_t cmdEnqueued = commandUpdatesEnqueued;
    const uint64_t cmdRejected = commandUpdatesRejected;
    const uint64_t cmdConsumed = pipeline.commandsConsumed();
    const uint64_t cmdApplied = pipeline.commandsApplied();

    const uint64_t totalRequested = commandUpdatesRequested + mbPublished;
    const uint64_t totalAccepted = cmdEnqueued + mbPublished;
    const uint64_t totalRejected = cmdRejected;
    const uint64_t totalApplied = cmdApplied + mbApplied;

    const size_t audioNewAllocations = s_audioScalarNewCount.load(std::memory_order_relaxed);
    const size_t audioArrayNewAllocations = s_audioArrayNewCount.load(std::memory_order_relaxed);
    const size_t audioAlignedAllocations = s_audioAlignedNewCount.load(std::memory_order_relaxed);
    const size_t audioAlignedNothrowAllocations = s_audioAlignedNothrowNewCount.load(std::memory_order_relaxed);
    const size_t totalAudioAllocations = s_audioThreadAllocationCount.load(std::memory_order_relaxed);

    const bool testPassed = !timedOut && (totalBlocks >= kRequiredAudioBlocks) && (totalAudioAllocations == 0);

    // Diagnostic Output (Section 6.1)
    std::printf("\n===================================================\n"
                "Audio Thread Zero Allocation Test\n\n"
                "Sample Rate: 48000 Hz\n"
                "Block Size: 256\n"
                "Required Blocks: %llu\n"
                "Processed Blocks: %llu\n\n"
                "Updates Requested: %llu\n"
                "Updates Accepted: %llu\n"
                "Updates Rejected: %llu\n"
                "Updates Applied: %llu\n\n"
                "Mailbox Published: %llu\n"
                "Mailbox Consumed: %llu\n"
                "Mailbox Applied: %llu\n"
                "Mailbox Superseded: %llu\n"
                "Mailbox Pending: %llu\n\n"
                "Command Enqueued: %llu\n"
                "Command Consumed: %llu\n"
                "Command Applied: %llu\n\n"
                "C++ operator new allocations: %zu\n"
                "C++ operator new[] allocations: %zu\n"
                "C++ aligned allocations: %zu\n"
                "C++ aligned nothrow allocations: %zu\n\n"
                "Result: %s\n"
                "===================================================\n",
                static_cast<unsigned long long>(kRequiredAudioBlocks),
                static_cast<unsigned long long>(totalBlocks),
                static_cast<unsigned long long>(totalRequested),
                static_cast<unsigned long long>(totalAccepted),
                static_cast<unsigned long long>(totalRejected),
                static_cast<unsigned long long>(totalApplied),
                static_cast<unsigned long long>(mbPublished),
                static_cast<unsigned long long>(mbConsumed),
                static_cast<unsigned long long>(mbApplied),
                static_cast<unsigned long long>(mbSuperseded),
                static_cast<unsigned long long>(mbPending),
                static_cast<unsigned long long>(cmdEnqueued),
                static_cast<unsigned long long>(cmdConsumed),
                static_cast<unsigned long long>(cmdApplied),
                audioNewAllocations,
                audioArrayNewAllocations,
                audioAlignedAllocations,
                audioAlignedNothrowAllocations,
                testPassed ? "PASS" : "FAIL");
    std::fflush(stdout);

    if (timedOut) {
        QFAIL(qPrintable(QString("Timeout waiting for audio blocks: processed %1 / %2 required blocks")
            .arg(totalBlocks).arg(kRequiredAudioBlocks)));
    }

    // Acceptance Criteria Assertions
    // P1-4-AC-01, P1-4-AC-02: Guaranteed >= 1,000 blocks processed
    QVERIFY2(totalBlocks >= kRequiredAudioBlocks, "Processed blocks must be >= 1000");

    // P1-4-AC-04: Separation of Requested, Accepted, Rejected
    QCOMPARE(cmdEnqueued + cmdRejected, commandUpdatesRequested);
    QCOMPARE(totalAccepted + totalRejected, totalRequested);

    // P1-4-AC-05: Queue push success != Applied. Consumed commands are applied.
    QCOMPARE(cmdConsumed, cmdEnqueued);
    QCOMPARE(cmdApplied, cmdConsumed);

    // P1-4-R2 & P1-4-R3: Mailbox Published, Consumed, Applied, Superseded, Pending consistency
    QVERIFY(mbConsumed <= mbPublished);
    QVERIFY(mbApplied <= mbConsumed);
    QCOMPARE(mbPublished, mbConsumed + mbSuperseded + mbPending);
    QVERIFY(mbPublished > 0);
    QVERIFY(mbConsumed > 0);
    QVERIFY(mbApplied > 0);

    // P1-4-AC-08, P1-4-AC-09, P1-4-R1: C++ allocations strictly ZERO across all forms
    QCOMPARE(totalAudioAllocations, size_t(0));
    QCOMPARE(audioNewAllocations, size_t(0));
    QCOMPARE(audioArrayNewAllocations, size_t(0));
    QCOMPARE(audioAlignedAllocations, size_t(0));
    QCOMPARE(audioAlignedNothrowAllocations, size_t(0));

    // Test Queue Full condition: fill queue to capacity (512 items) and verify process() doesn't block or allocate
    s_audioThreadAllocationCount.store(0, std::memory_order_relaxed);
    s_audioScalarNewCount.store(0, std::memory_order_relaxed);
    s_audioArrayNewCount.store(0, std::memory_order_relaxed);
    s_audioAlignedNewCount.store(0, std::memory_order_relaxed);
    s_audioAlignedNothrowNewCount.store(0, std::memory_order_relaxed);
    s_trackAllocations.store(true, std::memory_order_relaxed);

    for (size_t i = 0; i < 512; ++i) {
        desk32::engine::dsp::DspCommand fullCmd;
        fullCmd.type = desk32::engine::dsp::DspCommandType::SetNodeBypass;
        fullCmd.index = 0;
        pipeline.paramQueue().push(fullCmd);
    }

    const size_t frames = 256;
    std::array<double, frames> left;
    std::array<double, frames> right;
    std::array<float, frames * 2> out;
    left.fill(0.1);
    right.fill(0.1);
    out.fill(0.0f);

    {
        AudioThreadScopeGuard guard;
        pipeline.process(left.data(), right.data(), frames, out.data(),
                         desk32::engine::format::SampleFormat::Float32, false);
    }

    s_trackAllocations.store(false, std::memory_order_relaxed);
    QCOMPARE(s_audioThreadAllocationCount.load(), size_t(0));
}

void TestPhase2Dsp::testAudioThreadAllocationTrackerOverloadsAndIntentionalDetection() {
    // P1-4-R1: Verify that all C++ dynamic allocation forms are tracked,
    // including normal scalar/array new, nothrow scalar/array new,
    // aligned scalar/array new, and aligned+nothrow scalar/array new.
    // Also verify that intentional allocations on Audio Thread are detected,
    // while allocations outside Audio Thread are NOT counted toward Audio Thread allocations.

    s_trackAllocations.store(false, std::memory_order_relaxed);
    s_audioThreadAllocationCount.store(0, std::memory_order_relaxed);
    s_audioScalarNewCount.store(0, std::memory_order_relaxed);
    s_audioArrayNewCount.store(0, std::memory_order_relaxed);
    s_audioAlignedNewCount.store(0, std::memory_order_relaxed);
    s_audioAlignedNothrowNewCount.store(0, std::memory_order_relaxed);
    s_allocationCount.store(0, std::memory_order_relaxed);

    s_trackAllocations.store(true, std::memory_order_relaxed);

    // 1. Outside Audio Thread allocations: must increment s_allocationCount but NOT s_audioThreadAllocationCount
    {
        int* p1 = new int(42);
        asm volatile("" : "+r"(p1) : : "memory");
        int* p2 = new int[5]{1, 2, 3, 4, 5};
        asm volatile("" : "+r"(p2) : : "memory");
        int* p3 = new (std::nothrow) int(99);
        asm volatile("" : "+r"(p3) : : "memory");
        int* p4 = new (std::nothrow) int[8];
        asm volatile("" : "+r"(p4) : : "memory");
        auto* p5 = static_cast<int*>(operator new(sizeof(int), std::align_val_t{64}));
        asm volatile("" : "+r"(p5) : : "memory");
        auto* p6 = static_cast<int*>(operator new[](sizeof(int) * 4, std::align_val_t{64}));
        asm volatile("" : "+r"(p6) : : "memory");
        auto* p7 = static_cast<int*>(operator new(sizeof(int), std::align_val_t{64}, std::nothrow));
        asm volatile("" : "+r"(p7) : : "memory");
        auto* p8 = static_cast<int*>(operator new[](sizeof(int) * 4, std::align_val_t{64}, std::nothrow));
        asm volatile("" : "+r"(p8) : : "memory");

        QVERIFY(p1 && p2 && p3 && p4 && p5 && p6 && p7 && p8);

        delete p1;
        delete[] p2;
        delete p3;
        delete[] p4;
        operator delete(p5, std::align_val_t{64});
        operator delete[](p6, std::align_val_t{64});
        operator delete(p7, std::align_val_t{64}, std::nothrow);
        operator delete[](p8, std::align_val_t{64}, std::nothrow);
    }

    QVERIFY(s_allocationCount.load() >= 8);
    QCOMPARE(s_audioThreadAllocationCount.load(), size_t(0));
    QCOMPARE(s_audioScalarNewCount.load(), size_t(0));
    QCOMPARE(s_audioArrayNewCount.load(), size_t(0));
    QCOMPARE(s_audioAlignedNewCount.load(), size_t(0));
    QCOMPARE(s_audioAlignedNothrowNewCount.load(), size_t(0));

    // 2. Audio Thread intentional allocations: must be detected and increment category counters
    {
        AudioThreadScopeGuard guard;

        // P1-4-R1-AC-01: Aligned + Nothrow scalar new
        auto* pAlignedNothrowScalar = static_cast<int*>(operator new(sizeof(int), std::align_val_t{64}, std::nothrow));
        asm volatile("" : "+r"(pAlignedNothrowScalar) : : "memory");
        QVERIFY(pAlignedNothrowScalar != nullptr);
        QCOMPARE(reinterpret_cast<uintptr_t>(pAlignedNothrowScalar) % 64, uintptr_t(0));
        QCOMPARE(s_audioAlignedNothrowNewCount.load(), size_t(1));

        // P1-4-R1-AC-02: Aligned + Nothrow array new[]
        auto* pAlignedNothrowArray = static_cast<int*>(operator new[](sizeof(int) * 16, std::align_val_t{64}, std::nothrow));
        asm volatile("" : "+r"(pAlignedNothrowArray) : : "memory");
        QVERIFY(pAlignedNothrowArray != nullptr);
        QCOMPARE(reinterpret_cast<uintptr_t>(pAlignedNothrowArray) % 64, uintptr_t(0));
        QCOMPARE(s_audioAlignedNothrowNewCount.load(), size_t(2));

        // Aligned normal scalar & array
        auto* pAlignedScalar = static_cast<int*>(operator new(sizeof(int), std::align_val_t{64}));
        asm volatile("" : "+r"(pAlignedScalar) : : "memory");
        auto* pAlignedArray = static_cast<int*>(operator new[](sizeof(int) * 8, std::align_val_t{64}));
        asm volatile("" : "+r"(pAlignedArray) : : "memory");
        QCOMPARE(s_audioAlignedNewCount.load(), size_t(2));

        // Normal scalar & array
        int* pScalar = new int(123);
        asm volatile("" : "+r"(pScalar) : : "memory");
        int* pArray = new int[4]{1, 2, 3, 4};
        asm volatile("" : "+r"(pArray) : : "memory");
        QCOMPARE(s_audioScalarNewCount.load(), size_t(1));
        QCOMPARE(s_audioArrayNewCount.load(), size_t(1));

        // Cleanup
        operator delete(pAlignedNothrowScalar, std::align_val_t{64}, std::nothrow);
        operator delete[](pAlignedNothrowArray, std::align_val_t{64}, std::nothrow);
        operator delete(pAlignedScalar, std::align_val_t{64});
        operator delete[](pAlignedArray, std::align_val_t{64});
        delete pScalar;
        delete[] pArray;
    }

    // P1-4-R1-AC-03: Total intentional allocations detected on Audio Thread
    QCOMPARE(s_audioThreadAllocationCount.load(), size_t(6));
    s_trackAllocations.store(false, std::memory_order_relaxed);
}

void TestPhase2Dsp::testDspParameterMailboxConsumedAppliedAndSupersededConsistency() {
    // P1-4-R2 & P1-4-R3: Verify accurate separation of Published, Consumed, Applied, Superseded,
    // and Pending/Unconsumed updates, along with unapplied diagnostic tracking.
    AudioPipeline pipeline(48000.0);
    pipeline.resetStatistics();

    // 1. Initial State: all counts zero
    QCOMPARE(pipeline.paramMailbox().updateCount(), uint64_t(0));
    QCOMPARE(pipeline.mailboxConsumed(), uint64_t(0));
    QCOMPARE(pipeline.mailboxApplied(), uint64_t(0));
    QCOMPARE(pipeline.paramMailbox().pendingUpdatesCount(), uint64_t(0));

    // 2. Publish 1 update without processing (Pending state verification)
    pipeline.paramMailbox().setFilterParams(100.0f, 5000.0f, 1, 0, true, true);
    QCOMPARE(pipeline.paramMailbox().updateCount(), uint64_t(1));
    QCOMPARE(pipeline.paramMailbox().pendingUpdatesCount(), uint64_t(1)); // Exactly 1 pending

    // 3. Publish 3 more updates without processing -> previous 3 are superseded, 1 is pending
    pipeline.paramMailbox().setFilterParams(120.0f, 4800.0f, 1, 0, true, true);
    pipeline.paramMailbox().setFilterParams(140.0f, 4600.0f, 1, 0, true, true);
    pipeline.paramMailbox().setFilterParams(160.0f, 4400.0f, 1, 0, true, true);
    QCOMPARE(pipeline.paramMailbox().updateCount(), uint64_t(4));
    QCOMPARE(pipeline.paramMailbox().pendingUpdatesCount(), uint64_t(1));

    // P1-4-R2-AC-02 & AC-03: Superseded vs Pending distinction before consumption
    // Published = 4, Consumed = 0, Pending = 1 => Superseded = 4 - 0 - 1 = 3
    uint64_t pub = pipeline.paramMailbox().updateCount();
    uint64_t con = pipeline.mailboxConsumed();
    uint64_t pen = pipeline.paramMailbox().pendingUpdatesCount();
    uint64_t sup = (pub >= con + pen) ? (pub - con - pen) : 0;
    QCOMPARE(sup, uint64_t(3));
    QCOMPARE(pen, uint64_t(1));
    QCOMPARE(pub, con + sup + pen);

    // 4. Audio Thread processes 1 block with no master/listening chain attached
    // -> Update is consumed, but DSP nodes are not present, so Applied remains 0!
    const size_t frames = 256;
    std::array<double, frames> left{};
    std::array<double, frames> right{};
    std::array<float, frames * 2> out{};

    pipeline.process(left.data(), right.data(), frames, out.data(),
                     desk32::engine::format::SampleFormat::Float32, false);

    // Now: Published = 4, Consumed = 1, Applied = 0, Pending = 0, Superseded = 3
    pub = pipeline.paramMailbox().updateCount();
    con = pipeline.mailboxConsumed();
    uint64_t app = pipeline.mailboxApplied();
    pen = pipeline.paramMailbox().pendingUpdatesCount();
    sup = (pub >= con + pen) ? (pub - con - pen) : 0;

    QCOMPARE(con, uint64_t(1));
    QCOMPARE(pen, uint64_t(0));
    QCOMPARE(sup, uint64_t(3));
    QCOMPARE(pub, con + sup + pen);

    // P1-4-R3-AC-01, AC-02, AC-03:
    // Consumed <= Published, Applied <= Consumed, unapplied detected when target node not present
    QVERIFY(con <= pub);
    QVERIFY(app <= con);
    QCOMPARE(app, uint64_t(0)); // Unapplied because node is not present in empty pipeline
    uint64_t unapplied = con - app;
    QCOMPARE(unapplied, uint64_t(1)); // Diagnosed: 1 update consumed but unapplied

    // 5. Attach master chain with FilterNode and publish update with modified mask
    auto masterChain = std::make_unique<AudioChain>("Master", ChainPurpose::MasteringChain);
    masterChain->appendNode(std::make_shared<HighLowPassNode>("Filter"));
    pipeline.swapMasterChain(std::move(masterChain));

    pipeline.paramMailbox().setFilterParams(200.0f, 4000.0f, 1, 0, true, true);
    pipeline.process(left.data(), right.data(), frames, out.data(),
                     desk32::engine::format::SampleFormat::Float32, false);

    // Now: Published = 5, Consumed = 2, Applied = 1 (this new one was applied!), Pending = 0, Superseded = 3
    pub = pipeline.paramMailbox().updateCount();
    con = pipeline.mailboxConsumed();
    app = pipeline.mailboxApplied();
    pen = pipeline.paramMailbox().pendingUpdatesCount();
    sup = (pub >= con + pen) ? (pub - con - pen) : 0;

    QCOMPARE(pub, uint64_t(5));
    QCOMPARE(con, uint64_t(2));
    QCOMPARE(app, uint64_t(1));
    QCOMPARE(pen, uint64_t(0));
    QCOMPARE(sup, uint64_t(3));
    QCOMPARE(pub, con + sup + pen);
    QVERIFY(con <= pub);
    QVERIFY(app <= con);
}

void TestPhase2Dsp::testDspNodeBypassParameterPreservation() {
    // P1-2: Verify that toggling node bypass does NOT destroy or reset parameter values
    HighLowPassNode filterNode("Test Filter");
    filterNode.configure(48000.0, 120.0, 8000.0,
                         HighLowPassFilter::FilterSlope::Slope24dB,
                         HighLowPassFilter::FilterSlope::Slope12dB);

    CompressorNode compNode("Test Comp");
    compNode.configure(48000.0);
    compNode.prepareInstance(48000.0, -18.0, 4.0, 15.0, 120.0, 3.5, 6.0, true);

    PeakLimiterNode limNode("Test Limiter");
    limNode.configure(48000.0);
    limNode.prepareInstance(48000.0, -1.5, 35.0, 4.0, true);

    ReverbNode revNode("Test Reverb");
    revNode.configure(48000.0);
    revNode.prepareInstance(48000.0, 0.75, 0.45, 0.35, 25.0, true, true);

    // Process single block to swap prepared instances into active
    double dL = 0.0, dR = 0.0;
    compNode.process(&dL, &dR, 1);
    limNode.process(&dL, &dR, 1);
    revNode.process(&dL, &dR, 1);

    // Bypass all
    filterNode.setBypassed(true);
    compNode.setBypassed(true);
    limNode.setBypassed(true);
    revNode.setBypassed(true);

    QCOMPARE(filterNode.isBypassed(), true);
    QCOMPARE(compNode.isBypassed(), true);
    QCOMPARE(limNode.isBypassed(), true);
    QCOMPARE(revNode.isBypassed(), true);

    // Unbypass all
    filterNode.setBypassed(false);
    compNode.setBypassed(false);
    limNode.setBypassed(false);
    revNode.setBypassed(false);

    QCOMPARE(filterNode.isBypassed(), false);
    QCOMPARE(compNode.isBypassed(), false);
    QCOMPARE(limNode.isBypassed(), false);
    QCOMPARE(revNode.isBypassed(), false);

    // Verify all parameters are intact
    QCOMPARE(filterNode.dsp().hpfFrequency(), 120.0);
    QCOMPARE(filterNode.dsp().lpfFrequency(), 8000.0);
    QCOMPARE(filterNode.dsp().isHpfEnabled(), true);
    QCOMPARE(filterNode.dsp().isLpfEnabled(), true);

    QCOMPARE(compNode.thresholdDb(), -18.0);
    QCOMPARE(compNode.ratio(), 4.0);
    QCOMPARE(compNode.attackMs(), 15.0);
    QCOMPARE(compNode.releaseMs(), 120.0);
    QCOMPARE(compNode.makeupGainDb(), 3.5);
    QCOMPARE(compNode.kneeDb(), 6.0);

    QCOMPARE(limNode.ceilingDb(), -1.5);
    QCOMPARE(limNode.releaseMs(), 35.0);
    QCOMPARE(limNode.lookaheadMs(), 4.0);

    QCOMPARE(revNode.roomSize(), 0.75);
    QCOMPARE(revNode.damping(), 0.45);
    QCOMPARE(revNode.wetLevel(), 0.35);
    QCOMPARE(revNode.predelayMs(), 25.0);
    QCOMPARE(revNode.isTailPreserving(), true);
}

QTEST_GUILESS_MAIN(TestPhase2Dsp)
#include "test_phase2_dsp.moc"
