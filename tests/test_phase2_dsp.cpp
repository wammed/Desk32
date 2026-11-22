#include <QJsonArray>
#include <QObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <latch>
#include <numbers>
#include <thread>
#include <vector>

#include "core/EngineController.hpp"
#include "core/log/LogErrorCodes.hpp"
#include "core/log/LogService.hpp"
#include "engine/buffer/PcmRingBuffer.hpp"
#include "engine/dsp/AlgorithmicReverb.hpp"
#include "engine/dsp/Compressor.hpp"
#include "engine/dsp/GraphicEQ.hpp"
#include "engine/dsp/HighLowPassFilter.hpp"
#include "engine/dsp/ParameterCommandQueue.hpp"
#include "engine/dsp/ParameterSmoother.hpp"
#include "engine/dsp/ParametricEQ.hpp"
#include "engine/dsp/PeakLimiter.hpp"
#include "engine/memory/EpochReclaimer.hpp"
#include "engine/pipeline/AudioPipeline.hpp"
#include "engine/pipeline/BuiltInDspNodes.hpp"

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

/// RAII Scope Guard to strictly delineate Audio Thread execution sections (P1-4
/// Section 5.3)
struct AudioThreadScopeGuard {
  AudioThreadScopeGuard() noexcept { t_isAudioThread = true; }
  ~AudioThreadScopeGuard() noexcept { t_isAudioThread = false; }
  AudioThreadScopeGuard(const AudioThreadScopeGuard &) = delete;
  AudioThreadScopeGuard &operator=(const AudioThreadScopeGuard &) = delete;
};

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void *operator new(size_t size) {
  if (s_trackAllocations.load(std::memory_order_relaxed)) {
    if (t_isAudioThread) {
      s_audioScalarNewCount.fetch_add(1, std::memory_order_relaxed);
      s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    s_allocationCount.fetch_add(1, std::memory_order_relaxed);
  }
  return std::malloc(size);
}

void *operator new(size_t size, const std::nothrow_t &) noexcept {
  if (s_trackAllocations.load(std::memory_order_relaxed)) {
    if (t_isAudioThread) {
      s_audioScalarNewCount.fetch_add(1, std::memory_order_relaxed);
      s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    s_allocationCount.fetch_add(1, std::memory_order_relaxed);
  }
  return std::malloc(size);
}

void *operator new[](size_t size) {
  if (s_trackAllocations.load(std::memory_order_relaxed)) {
    if (t_isAudioThread) {
      s_audioArrayNewCount.fetch_add(1, std::memory_order_relaxed);
      s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    s_allocationCount.fetch_add(1, std::memory_order_relaxed);
  }
  return std::malloc(size);
}

void *operator new[](size_t size, const std::nothrow_t &) noexcept {
  if (s_trackAllocations.load(std::memory_order_relaxed)) {
    if (t_isAudioThread) {
      s_audioArrayNewCount.fetch_add(1, std::memory_order_relaxed);
      s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    s_allocationCount.fetch_add(1, std::memory_order_relaxed);
  }
  return std::malloc(size);
}

void *operator new(size_t size, std::align_val_t al) {
  if (s_trackAllocations.load(std::memory_order_relaxed)) {
    if (t_isAudioThread) {
      s_audioAlignedNewCount.fetch_add(1, std::memory_order_relaxed);
      s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    s_allocationCount.fetch_add(1, std::memory_order_relaxed);
  }
  void *ptr = nullptr;
  if (posix_memalign(&ptr, static_cast<size_t>(al), size) != 0) {
    throw std::bad_alloc();
  }
  return ptr;
}

void *operator new[](size_t size, std::align_val_t al) {
  if (s_trackAllocations.load(std::memory_order_relaxed)) {
    if (t_isAudioThread) {
      s_audioAlignedNewCount.fetch_add(1, std::memory_order_relaxed);
      s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    s_allocationCount.fetch_add(1, std::memory_order_relaxed);
  }
  void *ptr = nullptr;
  if (posix_memalign(&ptr, static_cast<size_t>(al), size) != 0) {
    throw std::bad_alloc();
  }
  return ptr;
}

void *operator new(size_t size, std::align_val_t al,
                   const std::nothrow_t &) noexcept {
  if (s_trackAllocations.load(std::memory_order_relaxed)) {
    if (t_isAudioThread) {
      s_audioAlignedNothrowNewCount.fetch_add(1, std::memory_order_relaxed);
      s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    s_allocationCount.fetch_add(1, std::memory_order_relaxed);
  }
  void *ptr = nullptr;
  if (posix_memalign(&ptr, static_cast<size_t>(al), size) != 0) {
    return nullptr;
  }
  return ptr;
}

void *operator new[](size_t size, std::align_val_t al,
                     const std::nothrow_t &) noexcept {
  if (s_trackAllocations.load(std::memory_order_relaxed)) {
    if (t_isAudioThread) {
      s_audioAlignedNothrowNewCount.fetch_add(1, std::memory_order_relaxed);
      s_audioThreadAllocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    s_allocationCount.fetch_add(1, std::memory_order_relaxed);
  }
  void *ptr = nullptr;
  if (posix_memalign(&ptr, static_cast<size_t>(al), size) != 0) {
    return nullptr;
  }
  return ptr;
}

void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void *p, size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete(void *p, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  std::free(p);
}
void operator delete[](void *p, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  std::free(p);
}
void operator delete(void *p, size_t, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  std::free(p);
}
void operator delete[](void *p, size_t, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  std::free(p);
}

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
  void testSeekObservabilityLogging();
  void testSessionAndFolderObservabilityLogging();
  void testAutoAdvanceLoggingAndNoDoubleAdvance();
  void testAutoPauseOtherDeckOnPlay();
  void testFolderSessionEndLoggingWithReasons();
  void testHpfLpfClickFreeEnableDisableCrossfade();
  void testMasterGainPostTapIsolation();
  void testPhase2GapFixesBitPerfectGeqListeningMetersAndSmoothing();
  void testPhase4AuditDspReverbContractsOverflowAndSmoothing();
  void testReverbGainStagingCombCountAndStability();
  void testDspInputOutputTrimsPresetRulesAndPreamp();
  void cleanupTestCase();

private:
  std::unique_ptr<QTemporaryDir> m_tempDir;
};

void TestPhase2Dsp::initTestCase() {
  qDebug() << "Initializing TestPhase2Dsp (Phase 2 Acoustic Correction & "
              "Built-in DSP Verification)...";
  m_tempDir = std::make_unique<QTemporaryDir>();
  QVERIFY(m_tempDir->isValid());
  QString dbPath = m_tempDir->path() + "/test_phase2_log.db";
  desk32::core::log::LogService::instance().start(dbPath);
}

void TestPhase2Dsp::cleanupTestCase() {
  desk32::core::log::LogService::instance().stop();
}

void TestPhase2Dsp::test24BandParametricEqAndResponseCurve() {
  ParametricEQ peq;
  peq.configure(48000.0);
  peq.setEnabled(true);

  QCOMPARE(ParametricEQ::BANDS_PER_CHANNEL, size_t(16));

  // 1. Verify Left Bands + Right Bands are strictly independent (Section 5.6:
  // 16 bands/ch)
  ParametricEQ::BandConfig highShelf;
  highShelf.frequency = 18000.0;
  highShelf.gainDb = 3.0;
  highShelf.Q = 0.707;
  highShelf.type = BiquadFilterType::HighShelf;
  highShelf.enabled = true;
  peq.setLeftBand(15, highShelf);
  QCOMPARE(peq.leftBand(15).frequency, 18000.0);
  QCOMPARE(peq.rightBand(15).gainDb, 0.0);

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
  std::vector<double> freqs = {100.0, 1000.0, 5000.0, 10000.0};
  std::vector<double> leftDb, rightDb;
  peq.calculateCompositeResponse(freqs, leftDb, rightDb);

  QCOMPARE(leftDb.size(), freqs.size());
  QCOMPARE(rightDb.size(), freqs.size());

  // Left channel should have ~+6 dB at 1000 Hz, but ~0 dB at 100 Hz
  QVERIFY(std::abs(leftDb[0]) < 0.5);       // 100 Hz
  QVERIFY(std::abs(leftDb[1] - 6.0) < 0.5); // 1000 Hz
  QVERIFY(std::abs(rightDb[1]) < 0.5); // Right channel has no boost at 1000 Hz!
  QVERIFY(std::abs(rightDb[2] - (-4.5)) <
          0.5); // Right channel has -4.5 dB at 5000 Hz!

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

void TestPhase2Dsp::test32BandGraphicEqAndReset() {
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

  geq.setBandGain(10, 15.0);        // Exceeds +12 dB
  QCOMPARE(geq.bandGain(10), 12.0); // Clamped to 12 dB

  // 3. Response curve verification
  std::vector<double> freqs = {100.0, 1000.0, 10000.0};
  std::vector<double> curveDb;
  geq.calculateResponseCurve(freqs, curveDb);
  QVERIFY(curveDb[1] > 6.0); // Peak around 1 kHz

  // 4. Reset all bands
  geq.resetAllBands();
  for (size_t i = 0; i < GraphicEQ::NUM_BANDS; ++i) {
    QCOMPARE(geq.bandGain(i), 0.0);
  }
}

void TestPhase2Dsp::testHighLowPassFilterSlopesAndCutoff() {
  HighLowPassFilter filter;
  // Configure HPF at 100 Hz, LPF at 5000 Hz, 12 dB/oct slope
  filter.configure(48000.0, 100.0, 5000.0,
                   HighLowPassFilter::FilterSlope::Slope12dB,
                   HighLowPassFilter::FilterSlope::Slope12dB);
  filter.setHpfEnabled(true);
  filter.setLpfEnabled(true);

  std::vector<double> freqs = {25.0,   50.0,    100.0,  1000.0,
                               5000.0, 10000.0, 20000.0};
  std::vector<double> outDb;
  filter.calculateResponseCurve(freqs, outDb);

  // At cutoff frequencies (100 Hz and 5000 Hz), attenuation is approximately -3
  // dB (Butterworth)
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

void TestPhase2Dsp::testCompressorDynamicsAndGainReduction() {
  Compressor comp;
  // Threshold = -12 dB, Ratio = 4:1, Attack = 0.5ms, Release = 50ms, Makeup =
  // 0dB
  comp.configure(48000.0, -12.0, 4.0, 0.5, 50.0, 0.0,
                 0.0); // Hard knee for exact measurement
  comp.setEnabled(true);

  // 1. Signal below threshold (-20 dBFS ≈ 0.1 linear amplitude)
  double left = 0.1, right = 0.1;
  for (int i = 0; i < 200; ++i) {
    comp.process(left, right);
    left = 0.1;
    right = 0.1;
  }
  QCOMPARE(comp.gainReductionDb(), 0.0); // Zero reduction below threshold

  // 2. Signal +12 dB above threshold (0 dBFS = 1.0 linear amplitude)
  // Input is 0 dBFS (-12 dB threshold -> 12 dB excess).
  // With 4:1 ratio, gain reduction should approach: 12 * (1 - 1/4) = 9.0 dB!
  for (int i = 0; i < 1000; ++i) { // Settle envelope
    left = 1.0;
    right = 1.0;
    comp.process(left, right);
  }

  qDebug() << "Measured Compressor Gain Reduction:" << comp.gainReductionDb()
           << "dB (expected ~9.0 dB)";
  QVERIFY(std::abs(comp.gainReductionDb() - 9.0) < 0.5);
  QVERIFY(left <
          0.4); // Compressed output amplitude < 0.4 (1.0 * 10^(-9/20) ≈ 0.355)
}

void TestPhase2Dsp::testPeakLimiterBrickwallAndLookaheadLatency() {
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
    double left =
        2.0 * std::sin(2.0 * std::numbers::pi * 1000.0 * (i / 48000.0));
    double right = left;

    limiter.process(left, right);

    if (std::abs(left) > ceilingLinear + 1e-6 ||
        std::abs(right) > ceilingLinear + 1e-6) {
      anyExceeded = true;
    }
    maxObserved = std::max(maxObserved, std::abs(left));
  }

  qDebug() << "Peak Limiter max observed output:" << maxObserved
           << "vs ceiling:" << ceilingLinear;
  QVERIFY2(!anyExceeded,
           "Peak Limiter failed: sample exceeded brickwall ceiling!");
  QVERIFY(limiter.gainReductionDb() >
          5.0); // Significant gain reduction recorded
}

void TestPhase2Dsp::testReverbTailPreservingBypass() {
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

  // Now engage Tail-Preserving Bypass (m_enabled = false, tailPreserving =
  // true)
  reverb.setEnabled(false);

  // Process additional frames with zero input: existing reverberation tail MUST
  // continue to ring out!
  double tailEnergy = 0.0;
  for (int i = 0; i < 2000; ++i) {
    left = 0.0;
    right = 0.0;
    reverb.process(left, right);
    tailEnergy += std::abs(left) + std::abs(right);
  }

  qDebug() << "Reverb tail energy during tail-preserving bypass:" << tailEnergy;
  QVERIFY2(
      tailEnergy > 0.01,
      "Tail-preserving bypass failed: reverb tail was truncated abruptly!");

  // Immediate bypass test:
  reverb.setTailPreserving(false);
  left = 0.0;
  right = 0.0;
  reverb.process(left, right);
  QCOMPARE(left, 0.0);
  QCOMPARE(right, 0.0);
}

void TestPhase2Dsp::testParameterSmootherBehavior() {
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

void TestPhase2Dsp::testLogicalRenderTapAndListeningCorrectionIsolation() {
  AudioPipeline pipeline(48000.0);

  // 1. Master Chain before Logical Render Tap
  auto masterChain = std::make_unique<AudioChain>("Master Chain",
                                                  ChainPurpose::MasteringChain);
  auto masterPeq = std::make_shared<ParametricEqNode>(
      "Master PEQ", NodeAffiliation::RenderCommon);
  masterChain->appendNode(masterPeq);
  pipeline.swapMasterChain(std::move(masterChain));

  // 2. Listening Correction Chain downstream of Logical Render Tap
  // (Section 5.10)
  auto listeningChain = std::make_unique<AudioChain>(
      "Listening Chain", ChainPurpose::ListeningCorrectionChain);
  auto correctionPeq = std::make_shared<ParametricEqNode>(
      "Listening Correction", NodeAffiliation::ListeningCorrection);

  listeningChain->appendNode(correctionPeq);
  pipeline.swapListeningChain(std::move(listeningChain));

  // Apply +12 dB boost at 1 kHz strictly in Listening Correction via Mailbox
  // (P0-21)
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
    double s = 0.2 * std::sin(2.0 * std::numbers::pi * 1000.0 *
                              (static_cast<double>(i) / 48000.0));
    leftIn[i] = s;
    rightIn[i] = s;
  }

  // A. Process in ListeningPath mode
  pipeline.setOutputMode(OutputPathMode::ListeningPath);
  std::vector<double> leftListen = leftIn;
  std::vector<double> rightListen = rightIn;
  std::vector<float> pcmListen(frames * 2);
  pipeline.process(leftListen.data(), rightListen.data(), frames,
                   pcmListen.data(),
                   desk32::engine::format::SampleFormat::Float32, false);

  // B. Process in RenderMonitorPath mode (MUST have 0.000000 Listening
  // Correction leakage!)
  pipeline.setOutputMode(OutputPathMode::RenderMonitorPath);
  std::vector<double> leftMonitor = leftIn;
  std::vector<double> rightMonitor = rightIn;
  std::vector<float> pcmMonitor(frames * 2);
  pipeline.process(leftMonitor.data(), rightMonitor.data(), frames,
                   pcmMonitor.data(),
                   desk32::engine::format::SampleFormat::Float32, false);

  // Calculate RMS difference: Render Monitor must match input (since master PEQ
  // is flat), while Listening Path must be significantly louder (+12 dB)
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

void TestPhase2Dsp::testZeroDynamicAllocationInPhase2Dsp() {
  // Verify that running all Phase 2 DSPs in real-time produces ZERO heap
  // allocations
  ParametricEQ peq;
  peq.configure(48000.0);
  GraphicEQ geq;
  geq.configure(48000.0);
  HighLowPassFilter filter;
  filter.configure(48000.0);
  Compressor comp;
  comp.configure(48000.0);
  comp.setEnabled(true);
  PeakLimiter limiter;
  limiter.configure(48000.0);
  limiter.setEnabled(true);
  AlgorithmicReverb reverb;
  reverb.configure(48000.0);
  reverb.setEnabled(true);

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

  qDebug() << "Total dynamic heap allocations in 10,000 Phase 2 DSP blocks:"
           << s_allocationCount.load();
  QCOMPARE(s_allocationCount.load(), 0u);
}

void TestPhase2Dsp::testPcmRingBufferConcurrentSPSCAndUnderrun() {
  using namespace desk32::engine::buffer;
  PcmRingBuffer ring(4096);
  QCOMPARE(ring.capacity(), size_t(4096));

  // 1. Underrun test: reading from empty ring buffer returns 0
  std::vector<double> outL(256), outR(256);
  size_t readEmpty = ring.readPlanar(outL.data(), outR.data(), 256);
  QCOMPARE(readEmpty, size_t(0));

  // 2. Multithreaded SPSC test: Producer writes 20,000 frames, Consumer reads
  // them
  constexpr size_t totalFrames = 20000;
  std::atomic<bool> producerDone{false};
  std::vector<double> consumedL;
  consumedL.reserve(totalFrames);
  std::vector<double> consumedR;
  consumedR.reserve(totalFrames);

  std::thread producer([&ring, &producerDone]() {
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

  std::thread consumer([&ring, &producerDone, &consumedL, &consumedR]() {
    std::vector<double> blockL(256);
    std::vector<double> blockR(256);
    while (consumedL.size() < totalFrames) {
      size_t n = ring.readPlanar(blockL.data(), blockR.data(), 256);
      if (n > 0) {
        consumedL.insert(consumedL.end(), blockL.begin(),
                         blockL.begin() + static_cast<ptrdiff_t>(n));
        consumedR.insert(consumedR.end(), blockR.begin(),
                         blockR.begin() + static_cast<ptrdiff_t>(n));
      } else if (producerDone.load(std::memory_order_acquire)) {
        // Drain remaining
        n = ring.readPlanar(blockL.data(), blockR.data(), 256);
        if (n > 0) {
          consumedL.insert(consumedL.end(), blockL.begin(),
                           blockL.begin() + static_cast<ptrdiff_t>(n));
          consumedR.insert(consumedR.end(), blockR.begin(),
                           blockR.begin() + static_cast<ptrdiff_t>(n));
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

void TestPhase2Dsp::testParameterCommandQueueSPSCAndCapacity() {
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
  for (auto &pr : producers)
    pr.join();
  stopConsumer.store(true, std::memory_order_release);
  consumer.join();

  QCOMPARE(totalPushed.load(), 200u);
  QCOMPARE(totalPopped.load(), 200u);
  QVERIFY(queue.empty());
}

void TestPhase2Dsp::testParameterCommandQueueCoalescingAndOverflowStats() {
  // P0-24: Strict SPSC Queue without in-place mutation, FIFO ordering, and
  // overflow tracking
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

  // 2. Queue full: Pushes must fail cleanly without corrupting existing
  // elements (P0-24)
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

  // Pause consumer while multiple producers push and finish concurrently, then
  // resume consumer
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
  for (auto &t : burstProducers)
    t.join();
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

void TestPhase2Dsp::testDspParameterMailboxMultiProducerAndContinuousUpdates() {
  // P0-24, P0-25: Multi-Producer safe continuous parameter mailbox with
  // lock-free consumer
  DspParameterMailbox mailbox;
  QVERIFY(mailbox.isLockFree());
  QCOMPARE(mailbox.updateCount(), 0u);

  ContinuousDspParameters params;
  QVERIFY(!mailbox.fetchUpdates(
      params)); // Immediately after initialization: no pending update

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
  QCOMPARE(mailbox.updateCount(),
           1u); // Monotonic sequence preserved across reset
  QVERIFY(!mailbox.fetchUpdates(params));

  mailbox.setGeqBandGain(5, -3.0f);
  QCOMPARE(mailbox.updateCount(), 2u);
  QVERIFY(mailbox.fetchUpdates(params));
  QCOMPARE(params.geqGains[5], -3.0f);
  QCOMPARE(params.sequenceNumber, 2u);
  QCOMPARE(params.epoch, 2u);

  // 2. Concurrent multi-producer stress test (4 producer threads, 1 consumer
  // thread)
  std::atomic<bool> running{true};
  std::atomic<uint64_t> totalPushes{0};
  std::latch startLatch{4};
  std::vector<std::thread> producers;

  for (int t = 0; t < 4; ++t) {
    producers.emplace_back([&mailbox, &running, &totalPushes, &startLatch,
                            t]() {
      startLatch.count_down();
      float val = 0.0f;
      while (running.load(std::memory_order_relaxed)) {
        mailbox.setGeqBandGain(static_cast<size_t>(t * 8), val);
        mailbox.setPeqBand(static_cast<size_t>(t * 5), 0, 1000.0f + val, val,
                           1.0f, 0, true);
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
  for (int i = 0; i < 5000 && (consumedUpdates < 50 || totalPushes.load() < 50);
       ++i) {
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
  for (auto &p : producers) {
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

void TestPhase2Dsp::testDspParameterMailbox3SlotExchangeSnapshotConsistency() {
  // P0-24, P0-25: 3-Slot Exchange tear-free snapshot consistency verification
  // Ensures that Consumer receives internally consistent parameter snapshots
  // without partial updates
  DspParameterMailbox mailbox;
  QVERIFY(mailbox.isLockFree());

  std::atomic<bool> running{true};
  std::atomic<uint64_t> snapshotCheckCount{0};
  std::atomic<bool> inconsistencyDetected{false};

  // Producer Thread: Writes monotonically increasing generation values across
  // all 32 bands atomically
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

  // Audio/Consumer Thread: Rapidly fetches snapshots and verifies tear-free
  // consistency
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

void TestPhase2Dsp::
    testDspParameterMailboxTornReadDetectionVsTripleBufferSafety() {
  // P0-24: Proof of Tear Detection Logic vs Triple Buffer Consistency
#if !defined(__SANITIZE_THREAD__) &&                                           \
    !(defined(__has_feature) && __has_feature(thread_sanitizer))
  // Part 1: Intentionally unsynchronized shared structure to prove the
  // detection logic works
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

  // Part 2: Identical verification on DspParameterMailbox yields strictly 0
  // tears
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

void TestPhase2Dsp::testDspParameterMailboxBatchApis() {
  // P0-25, P0-27: Batch Mailbox APIs (resetAllGeqBands, applyPeqPreset,
  // setPeqBandEnabled)
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

void TestPhase2Dsp::testSingleThreadDspUnitOperation() {
  // Test A (P0-23): Single-thread unit operations for all DSP processors
  // Verifies configure, process, reset, and parameter retention on a single
  // thread.

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

void TestPhase2Dsp::testPreparedDspInstanceExchangeSafety() {
  // P0-21, P0-22, P0-23: Comprehensive Verification of Prepared DSP Instance
  // Exchange (Cases 1 - 9)

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
    // Retired old instance is held until reclaimRetired is invoked on Control
    // Thread
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

  // Case 6: Concurrent Multi-Producer Control Threads + Audio Thread processing
  // during rapid exchanges (P0-21)
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
          compNode->prepareInstance(48000.0, -10.0 - ((i + t) % 8), 3.0, 10.0,
                                    100.0, static_cast<double>(i % 4), 4.0,
                                    true);
          limNode->prepareInstance(48000.0, -0.5 - ((i + t) % 5) * 0.2,
                                   50.0 + (i % 10) * 5.0, 5.0, true);
          revNode->prepareInstance(48000.0, 0.5 + ((i + t) % 4) * 0.1, 0.3, 0.3,
                                   10.0, true, true);
          std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
      });
    }

    for (auto &ct : controlThreads)
      ct.join();

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

    // Prepare new instance, lookahead state must inherit smoothly without
    // discontinuity
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

  // Case 10: Reverb Tail Continuity under repeated exchanges (distinct L/R
  // zero-filled buffers)
  {
    ReverbNode rev;
    rev.prepareInstance(48000.0, 0.85, 0.3, 0.5, 0.0, true, true);
    std::vector<double> exciteL(2048, 0.5), exciteR(2048, 0.5);
    rev.process(exciteL.data(), exciteR.data(), 2048);

    // Perform multiple exchanges while input is strictly zero (P0-22: Tail
    // preservation across exchanges)
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

    // Max difference between consecutive samples must not exhibit abrupt step
    // discontinuity
    for (size_t i = 1; i < 256; ++i) {
      double delta = std::abs(out1[i] - out1[i - 1]);
      QVERIFY(delta < 0.25);
    }
    rev.reclaimRetired();
  }

  // Case 12: Bounded execution time during exchange on Audio Thread
  // (Microbenchmark)
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

    int64_t elapsedNanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    // 256 frames of reverb processing on Audio Thread must easily complete
    // under 500 microseconds (500,000 ns) in native, or under 10ms with
    // Sanitizer instrumentation
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__) ||           \
    defined(__SANITIZE_UNDEFINED__) || defined(DESK32_SANITIZE_UNDEFINED)
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

void TestPhase2Dsp::testContinuousParameterMailboxUpdates() {
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
    mailbox.setFilterParams(30.0f + static_cast<float>(i * 5), 18000.0f, 1, 1,
                            true, true);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  stopWorkers.store(true, std::memory_order_relaxed);
  audioThread.join();

  QVERIFY(fetchesCompleted.load() > 5);
  QVERIFY(mailbox.updateCount() >= 100);
}

void TestPhase2Dsp::testRtSafetyAllocationDuringProcess() {
  // Test D (P0-23): Real-Time Safety - Zero dynamic allocations during Audio
  // process() loop
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

void TestPhase2Dsp::testConcurrentSampleRateAndMailboxUpdates() {
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
      mailbox.setGeqBandGain(static_cast<size_t>(idx % 32),
                             static_cast<float>((idx % 24) - 12));
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

void TestPhase2Dsp::testReverbMultiExchangeAndBypassTailDecay() {
  // P0-22: Deep Acoustic & Lifecycle Verification of Reverb Ringout Slots (All
  // Retire Reasons & Edge Cases)

  // 1. Capacity Eviction (SlotCapacityEviction): 10 consecutive excited
  // exchanges (> MAX_RINGOUT_SLOTS = 8)
  {
    ReverbNode rev;
    rev.prepareInstance(48000.0, 0.9, 0.2, 0.5, 0.0, true, true);
    std::vector<double> exciteL(2048, 0.6), exciteR(2048, 0.6);
    rev.process(exciteL.data(), exciteR.data(), 2048);

    // Perform 10 rapid exchanges where each instance is excited then swapped;
    // measure pure ringout tail on zero input
    std::vector<double> pulseL(64, 0.4), pulseR(64, 0.4);
    std::vector<double> zeroL(256, 0.0), zeroR(256, 0.0);
    for (int i = 0; i < 10; ++i) {
      rev.prepareInstance(48000.0, 0.88 - i * 0.02, 0.25, 0.5, 0.0, true, true);
      // Activate new instance and excite it briefly so it has internal energy
      // for next swap
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
      // Tail MUST NEVER be abruptly cut to 0 even when ringout capacity (8
      // slots) is exceeded
      QVERIFY(e > 1e-6);
    }
    // 10 excited swaps: first 8 fill all 8 ringout slots, swaps 9 and 10 evict
    // the 2 lowest-energy slots
    QCOMPARE(rev.activeRingoutSlotCount(), ReverbNode::MAX_RINGOUT_SLOTS);
    QCOMPARE(rev.capacityEvictionRetireCount(), 2u);
    QCOMPARE(rev.lastRetireReason(),
             ReverbNode::RetireReason::SlotCapacityEviction);
    rev.reclaimRetired();
    QCOMPARE(rev.retiredCount(), rev.reclaimedCount());
  }

  // 2. Natural Decay Complete (NaturalDecayComplete): Short RT60 decays
  // naturally and frees slot
  {
    ReverbNode rev;
    rev.prepareInstance(48000.0, 0.2, 0.5, 0.5, 0.0, true,
                        true); // Short decay roomSize = 0.2
    std::vector<double> exciteL(2048, 0.5), exciteR(2048, 0.5);
    rev.process(exciteL.data(), exciteR.data(), 2048);

    // Swap to a new instance -> old instance enters ringout slot 0
    rev.prepareInstance(48000.0, 0.3, 0.5, 0.5, 0.0, true, true);
    std::vector<double> zeroL(512, 0.0), zeroR(512, 0.0);
    rev.process(zeroL.data(), zeroR.data(), 512);
    QCOMPARE(rev.activeRingoutSlotCount(), size_t(1));

    // Process silent blocks until natural decay completes
    // (theoreticalDecayFrames for roomSize=0.2 is 0.5s = 24,000 frames = ~47
    // blocks of 512)
    for (int b = 0; b < 60; ++b) {
      std::fill(zeroL.begin(), zeroL.end(), 0.0);
      std::fill(zeroR.begin(), zeroR.end(), 0.0);
      rev.process(zeroL.data(), zeroR.data(), 512);
      if (rev.activeRingoutSlotCount() == 0)
        break;
    }
    QCOMPARE(rev.activeRingoutSlotCount(), size_t(0));
    QCOMPARE(rev.naturalDecayRetireCount(), 1u);
    QCOMPARE(rev.lastRetireReason(),
             ReverbNode::RetireReason::NaturalDecayComplete);
    rev.reclaimRetired();
    QCOMPARE(rev.retiredCount(), rev.reclaimedCount());
  }

  // 3. Max Retention Safety Ceiling (MaxRetentionSafetyCeiling) at 48kHz and
  // 96kHz
  {
    ReverbNode rev;
    AlgorithmicReverb dummy48;
    dummy48.configure(48000.0, 0.98, 0.1, 0.5, 0.0, true, true);
    ReverbNode::RingoutSlot slot48{&dummy48, 480000, 0, 1.0};
    QCOMPARE(rev.evaluateRetireCondition(slot48, 0.5),
             ReverbNode::RetireReason::MaxRetentionSafetyCeiling);

    AlgorithmicReverb dummy96;
    dummy96.configure(96000.0, 0.98, 0.1, 0.5, 0.0, true, true);
    ReverbNode::RingoutSlot slot96Early{&dummy96, 480000, 0, 1.0};
    // At 96kHz, 480,000 frames is only 5 seconds -> should NOT hit 10-second
    // safety ceiling yet
    QCOMPARE(rev.evaluateRetireCondition(slot96Early, 0.5),
             ReverbNode::RetireReason::None);
    ReverbNode::RingoutSlot slot96Full{&dummy96, 960000, 0, 1.0};
    QCOMPARE(rev.evaluateRetireCondition(slot96Full, 0.5),
             ReverbNode::RetireReason::MaxRetentionSafetyCeiling);
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

    // Inject NaN into active reverb state, then swap so it tries to enter
    // ringout
    rev.injectInvalidStateForTest(std::numeric_limits<double>::quiet_NaN());
    QVERIFY(rev.hasInvalidState());

    rev.prepareInstance(48000.0, 0.75, 0.3, 0.5, 0.0, true, true);
    std::vector<double> outL(256, 0.2), outR(256, 0.2);
    rev.process(outL.data(), outR.data(), 256);

    // Abnormal instance must have been reset & retired immediately without
    // entering ringout or polluting output
    QCOMPARE(rev.activeRingoutSlotCount(), size_t(0));
    QCOMPARE(rev.abnormalStateRetireCount(), 1u);
    QCOMPARE(rev.lastRetireReason(),
             ReverbNode::RetireReason::AbnormalStateReset);
    for (size_t i = 0; i < 256; ++i) {
      QVERIFY(std::isfinite(outL[i]));
      QVERIFY(std::isfinite(outR[i]));
    }
    rev.reclaimRetired();
  }

  // 6. Sample Rate Change (44.1kHz -> 48kHz -> 96kHz) and Block Size Change (64
  // -> 256 -> 1024) during Ringout
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

    // Process 60 blocks under bypass: output must be dry pass-through, and
    // internal tail must decay & retire
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

void TestPhase2Dsp::testReverbSlotCapacityEvictionAndSafetyCeiling() {
  // P0-22: Deterministic verification of SlotCapacityEviction and
  // MaxRetentionSafetyCeiling
  // 1. SlotCapacityEviction (All 8 slots saturated -> 9th swap evicts
  // longest-lived slot)
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
      rev.prepareInstance(sr, 0.8 + static_cast<double>(s) * 0.01, 0.3, 0.5,
                          0.0, true, true);
      rev.process(exciteL.data(), exciteR.data(), frames);
    }
    QCOMPARE(rev.activeRingoutSlotCount(), size_t(8));
    QCOMPARE(rev.capacityEvictionRetireCount(), uint64_t(0));

    // 9th swap: All 8 slots full -> must trigger SlotCapacityEviction
    rev.prepareInstance(sr, 0.7, 0.2, 0.4, 0.0, true, true);
    rev.process(exciteL.data(), exciteR.data(), frames);

    QCOMPARE(rev.activeRingoutSlotCount(), size_t(8)); // Still 8
    QCOMPARE(rev.capacityEvictionRetireCount(), uint64_t(1));
    QCOMPARE(rev.lastRetireReason(),
             ReverbNode::RetireReason::SlotCapacityEviction);
    rev.reclaimRetired();
  }

  // 2. MaxRetentionSafetyCeiling (10-second timeout ceiling)
  {
    ReverbNode rev;
    const double sr = 8000.0; // Use 8kHz so 10 seconds is 80,000 frames
    const size_t frames = 1000;
    rev.configure(sr);

    std::vector<double> exciteL(frames, 0.5), exciteR(frames, 0.5);
    rev.prepareInstance(sr, 0.99, 0.01, 0.5, 0.0, true,
                        true); // Extremely long decay
    rev.process(exciteL.data(), exciteR.data(), frames);

    // Swap to put it in ringout slot
    rev.prepareInstance(sr, 0.5, 0.5, 0.5, 0.0, true, true);
    std::vector<double> silenceL(frames, 0.0), silenceR(frames, 0.0);
    rev.process(silenceL.data(), silenceR.data(), frames);
    QCOMPARE(rev.activeRingoutSlotCount(), size_t(1));

    // Advance 81 blocks of 1000 frames (81,000 frames > 80,000 frames = 10
    // seconds @ 8kHz)
    for (int b = 0; b < 81; ++b) {
      std::fill(silenceL.begin(), silenceL.end(), 0.0);
      std::fill(silenceR.begin(), silenceR.end(), 0.0);
      rev.process(silenceL.data(), silenceR.data(), frames);
    }

    // Must be retired due to MaxRetentionSafetyCeiling
    QCOMPARE(rev.activeRingoutSlotCount(), size_t(0));
    QCOMPARE(rev.safetyCeilingRetireCount(), uint64_t(1));
    QCOMPARE(rev.lastRetireReason(),
             ReverbNode::RetireReason::MaxRetentionSafetyCeiling);
    rev.reclaimRetired();
  }
}

void TestPhase2Dsp::testDspParameterMailboxEpochAndMonotonicSequence() {
  // P0-24: Generation / Epoch tracking & strict sequenceNumber monotonicity
  // across resets
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

  // Reset mailbox: epoch must increment, sequenceNumber must preserve
  // monotonicity
  mailbox.reset();
  QCOMPARE(mailbox.currentEpoch(), uint64_t(2));
  QCOMPARE(mailbox.currentSequenceNumber(), seqBefore); // NOT zero!

  // Next update after reset: sequenceNumber continues monotonically from
  // seqBefore
  mailbox.setGeqBandGain(5, 1.5f);
  QCOMPARE(mailbox.currentEpoch(), uint64_t(2));
  QCOMPARE(mailbox.currentSequenceNumber(), seqBefore + 1);

  ContinuousDspParameters postResetParams;
  QVERIFY(mailbox.fetchUpdates(postResetParams));
  QCOMPARE(postResetParams.epoch, uint64_t(2));
  QCOMPARE(postResetParams.sequenceNumber, seqBefore + 1);
  QVERIFY(postResetParams.sequenceNumber > params.sequenceNumber);
}

void TestPhase2Dsp::testParameterCommandQueueOverflowAndDrainLimit() {
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

void TestPhase2Dsp::
    testReverbO1StateTrackingAccuracyAndTailWaveformComparison() {
  // P0-22: Comprehensive Reverb O(1) state tracking accuracy, tail continuity,
  // and decay verification
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
  QVERIFY(std::abs(energyAfterImpulse - scanAfterImpulse) / scanAfterImpulse <
          1e-3);

  // 3. Ringout with silence: verify O(1) energy matches buffer scan at every
  // step
  for (int block = 0; block < 100; ++block) {
    for (int s = 0; s < 256; ++s) {
      double l = 0.0, r = 0.0;
      reverb.process(l, r);
    }
    double e_o1 = reverb.totalInternalEnergy();
    double e_scan = reverb.debugScanTotalEnergy();
    if (e_scan > 1e-6) {
      double relErr = std::abs(e_o1 - e_scan) / e_scan;
      QVERIFY(relErr <
              0.02); // Within 2% relative error throughout dynamic decay
    }
  }

  // 4. Test ReverbNode natural decay and non-premature retirement
  ReverbNode revNode("TestReverbNode");
  revNode.configure(sr);
  revNode.prepareInstance(sr, 0.6, 0.2, 0.3, 0.0, true, true);

  const size_t frames = 256;
  std::vector<double> impulseBufL(frames, 0.0), impulseBufR(frames, 0.0);
  impulseBufL[0] = 1.0;
  impulseBufR[0] = 1.0;
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
    if (revNode.activeRingoutSlotCount() == 0)
      break;
  }
  QCOMPARE(revNode.activeRingoutSlotCount(), size_t(0));
  QCOMPARE(revNode.lastRetireReason(),
           ReverbNode::RetireReason::NaturalDecayComplete);
  revNode.reclaimRetired();

  // 5. High sample rate (192kHz) max tail execution time verification (pure
  // O(1))
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
  auto elapsedMicros =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  qDebug() << "192kHz 4096-frame Reverb O(1) process time:" << elapsedMicros
           << "microseconds";
  // 4096 frames at 192kHz deadline is 21.3 ms (21,333 us); native Release runs
  // in < 2ms
#if !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__) &&         \
    !defined(__SANITIZE_UNDEFINED__) && !defined(DESK32_SANITIZE_UNDEFINED) && \
    defined(NDEBUG)
  QVERIFY(elapsedMicros < 10000);
#else
  QVERIFY(elapsedMicros < 21333);
#endif
}

void TestPhase2Dsp::testActiveDspDirectModificationImpossibility() {
  // P0-21: Architectural verification that Audio Nodes do not expose mutable
  // DSP instances
  ParametricEqNode peqNode("Master PEQ");
  const ParametricEQ &peqConstRef = peqNode.dsp(); // Strictly const ref
  (void)peqConstRef;

  CompressorNode compNode("Master Comp");
  const Compressor &compConstRef = compNode.dsp(); // Strictly const ref
  (void)compConstRef;

  PeakLimiterNode limNode("Master Limiter");
  const PeakLimiter &limConstRef = limNode.dsp(); // Strictly const ref
  (void)limConstRef;

  ReverbNode revNode("Master Reverb");
  const AlgorithmicReverb &revConstRef = revNode.dsp(); // Strictly const ref
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

  std::vector<double> freqs = {50.0, 1000.0, 15000.0};
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
  rInL[0] = 1.0;
  rInR[0] = 1.0; // Impulse
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

  // Critical assertion: Global DSP Bypass must NOT modify or overwrite
  // individual node bypass states!
  QCOMPARE(filterNode->isBypassed(), false);
  QCOMPARE(compNode->isBypassed(), false);
  QCOMPARE(revNode->isBypassed(), true);
  QCOMPARE(limNode->isBypassed(), false);

  // Process signal while globally bypassed: audio must pass through completely
  // untouched
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
  // 1. ParameterSmoother behavior during Master Mute (smooth ramp down,
  // click-free)
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
  pipeline.process(inL.data(), inR.data(), 256, outPcm.data(),
                   desk32::engine::format::SampleFormat::Float32, false);

  auto telem = pipeline.currentTelemetry();
  QVERIFY(telem.masterPeakL > 0.49 && telem.masterPeakL < 0.51);
  QVERIFY(telem.masterPeakR > 0.24 && telem.masterPeakR < 0.26);
  QVERIFY(telem.masterRmsL > 0.49 && telem.masterRmsL < 0.51);
  QCOMPARE(telem.masterClipped, false);

  // Process hot signal that clips (>= 1.0)
  std::vector<double> hotL(256, 1.2), hotR(256, 0.8);
  pipeline.process(hotL.data(), hotR.data(), 256, outPcm.data(),
                   desk32::engine::format::SampleFormat::Float32, false);

  auto telemHot = pipeline.currentTelemetry();
  QVERIFY(telemHot.masterPeakL >= 1.0);
  QCOMPARE(telemHot.masterClipped, true);

  // Verify zero allocations during telemetry and parameter changes
  s_trackAllocations.store(true, std::memory_order_relaxed);
  s_allocationCount.store(0, std::memory_order_relaxed);

  for (int i = 0; i < 1000; ++i) {
    pipeline.process(inL.data(), inR.data(), 256, outPcm.data(),
                     desk32::engine::format::SampleFormat::Float32, false);
    auto t = pipeline.currentTelemetry();
    (void)t;
  }

  s_trackAllocations.store(false, std::memory_order_relaxed);
  QCOMPARE(s_allocationCount.load(), size_t(0));
}

void TestPhase2Dsp::testMasterMutePcmAttenuationIntegration() {
  // End-to-end integration test of Master Mute via EngineController APIs with
  // actual AudioPipeline (P1-3)
  desk32::core::EngineController &controller =
      desk32::core::EngineController::instance();
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

  auto *deckA = controller.deckManager()->deck('A');
  QVERIFY(deckA != nullptr);
  QVERIFY(deckA->loadTrack(synthInfo, 48000.0));
  controller.play("A");
  QCOMPARE(deckA->state(), desk32::engine::deck::TransportState::Playing);

  // Wait until background DecoderWorker has primed the ring buffer with decoded
  // audio
  int waitCount = 0;
  while (deckA->worker().ringBuffer().availableRead() < 8192 &&
         waitCount++ < 100) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  QVERIFY(deckA->worker().ringBuffer().availableRead() >= 4096);

  const size_t blockSize = 128;
  std::vector<float> outPcm(blockSize * 2);

  // Prime the pipeline to steady-state playback (fill lookahead buffers and
  // decoder ring buffers)
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
  QSignalSpy muteSpy(&controller,
                     &desk32::core::EngineController::masterMuteChanged);
  controller.setMasterMute(true);
  QCOMPARE(controller.isMasterMute(), true);
  QCOMPARE(muteSpy.count(), 1);

  // Process blocks through EngineController until smooth decay and settling (20
  // chunks of 4*128 frames = 213 ms > 14 tau)
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

      // Check for click/pop: sample-to-sample difference should be bounded (<
      // 0.05 per sample)
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

  // After settling (>14 tau), ParameterSmoother inside EngineController snaps
  // to 0.0: output must be strictly silent (< 1e-7, >140 dB attenuation)
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
  QCOMPARE(controller.masterGain(),
           1.5); // Master Gain 1.5 was preserved across mute!

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

  // After full ramp up, measure peak across multiple blocks to capture the sine
  // wave cycle
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
  // P1-4: Verify strictly ZERO C++ dynamic memory allocations (operator new /
  // operator new[]) in Audio Thread while Control Thread issues continuous
  // high-frequency DSP parameter updates across all nodes.
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
  auto masterChain = std::make_unique<AudioChain>("Master Chain",
                                                  ChainPurpose::MasteringChain);
  masterChain->appendNode(std::make_shared<ParametricEqNode>("PEQ"));
  masterChain->appendNode(std::make_shared<GraphicEqNode>("GEQ"));
  masterChain->appendNode(std::make_shared<HighLowPassNode>("Filter"));
  masterChain->appendNode(std::make_shared<CompressorNode>("Compressor"));
  masterChain->appendNode(std::make_shared<ReverbNode>("Reverb"));
  masterChain->appendNode(std::make_shared<PeakLimiterNode>("Limiter"));
  pipeline.swapMasterChain(std::move(masterChain));

  auto listeningChain = std::make_unique<AudioChain>(
      "Listening Chain", ChainPurpose::ListeningCorrectionChain);
  listeningChain->appendNode(
      std::make_shared<ParametricEqNode>("Listening Correction"));
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
        static_cast<float>(10000.0 - (i % 500)), 1, 0, true, true);
    if (i % 10 == 0) {
      pipeline.paramMailbox().setListeningPreset("Monitor Flat");
    }

    // Discrete Command updates (Compressor, Limiter, Reverb, Filter bypass /
    // params)
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
      pipeline.updateCompressor(-12.0 - (i % 10), 3.0, 10.0, 100.0, 2.0, 4.0,
                                true);
      pipeline.updatePeakLimiter(-1.0 - (i % 3) * 0.5, 50.0, 3.0, true);
      pipeline.updateReverb(0.6 + (i % 4) * 0.1, 0.5, 0.2, 20.0, true, true);
    }

    if (i % 50 == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }

  // P1-4-AC-01, P1-4-AC-02, P1-4-AC-03:
  // Guarantee processing of >= 1,000 Audio Processing Blocks with timeout
  // detection
  constexpr uint64_t kRequiredAudioBlocks = 1000;
  constexpr auto kTimeout = std::chrono::seconds(5);
  const auto startTime = std::chrono::steady_clock::now();
  bool timedOut = false;

  while (blocksProcessed.load(std::memory_order_acquire) <
         kRequiredAudioBlocks) {
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
  const uint64_t mbPublished =
      pipeline.paramMailbox().updateCount() - initialMailboxCount;
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

  const size_t audioNewAllocations =
      s_audioScalarNewCount.load(std::memory_order_relaxed);
  const size_t audioArrayNewAllocations =
      s_audioArrayNewCount.load(std::memory_order_relaxed);
  const size_t audioAlignedAllocations =
      s_audioAlignedNewCount.load(std::memory_order_relaxed);
  const size_t audioAlignedNothrowAllocations =
      s_audioAlignedNothrowNewCount.load(std::memory_order_relaxed);
  const size_t totalAudioAllocations =
      s_audioThreadAllocationCount.load(std::memory_order_relaxed);

  const bool testPassed = !timedOut && (totalBlocks >= kRequiredAudioBlocks) &&
                          (totalAudioAllocations == 0);

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
              static_cast<unsigned long long>(cmdApplied), audioNewAllocations,
              audioArrayNewAllocations, audioAlignedAllocations,
              audioAlignedNothrowAllocations, testPassed ? "PASS" : "FAIL");
  std::fflush(stdout);

  if (timedOut) {
    QFAIL(qPrintable(QString("Timeout waiting for audio blocks: processed %1 / "
                             "%2 required blocks")
                         .arg(totalBlocks)
                         .arg(kRequiredAudioBlocks)));
  }

  // Acceptance Criteria Assertions
  // P1-4-AC-01, P1-4-AC-02: Guaranteed >= 1,000 blocks processed
  QVERIFY2(totalBlocks >= kRequiredAudioBlocks,
           "Processed blocks must be >= 1000");

  // P1-4-AC-04: Separation of Requested, Accepted, Rejected
  QCOMPARE(cmdEnqueued + cmdRejected, commandUpdatesRequested);
  QCOMPARE(totalAccepted + totalRejected, totalRequested);

  // P1-4-AC-05: Queue push success != Applied. Consumed commands are applied.
  QCOMPARE(cmdConsumed, cmdEnqueued);
  QCOMPARE(cmdApplied, cmdConsumed);

  // P1-4-R2 & P1-4-R3: Mailbox Published, Consumed, Applied, Superseded,
  // Pending consistency
  QVERIFY(mbConsumed <= mbPublished);
  QVERIFY(mbApplied <= mbConsumed);
  QCOMPARE(mbPublished, mbConsumed + mbSuperseded + mbPending);
  QVERIFY(mbPublished > 0);
  QVERIFY(mbConsumed > 0);
  QVERIFY(mbApplied > 0);

  // P1-4-AC-08, P1-4-AC-09, P1-4-R1: C++ allocations strictly ZERO across all
  // forms
  QCOMPARE(totalAudioAllocations, size_t(0));
  QCOMPARE(audioNewAllocations, size_t(0));
  QCOMPARE(audioArrayNewAllocations, size_t(0));
  QCOMPARE(audioAlignedAllocations, size_t(0));
  QCOMPARE(audioAlignedNothrowAllocations, size_t(0));

  // Test Queue Full condition: fill queue to capacity (512 items) and verify
  // process() doesn't block or allocate
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

void TestPhase2Dsp::
    testAudioThreadAllocationTrackerOverloadsAndIntentionalDetection() {
  // P1-4-R1: Verify that all C++ dynamic allocation forms are tracked,
  // including normal scalar/array new, nothrow scalar/array new,
  // aligned scalar/array new, and aligned+nothrow scalar/array new.
  // Also verify that intentional allocations on Audio Thread are detected,
  // while allocations outside Audio Thread are NOT counted toward Audio Thread
  // allocations.

  s_trackAllocations.store(false, std::memory_order_relaxed);
  s_audioThreadAllocationCount.store(0, std::memory_order_relaxed);
  s_audioScalarNewCount.store(0, std::memory_order_relaxed);
  s_audioArrayNewCount.store(0, std::memory_order_relaxed);
  s_audioAlignedNewCount.store(0, std::memory_order_relaxed);
  s_audioAlignedNothrowNewCount.store(0, std::memory_order_relaxed);
  s_allocationCount.store(0, std::memory_order_relaxed);

  s_trackAllocations.store(true, std::memory_order_relaxed);

  // 1. Outside Audio Thread allocations: must increment s_allocationCount but
  // NOT s_audioThreadAllocationCount
  {
    int *p1 = new int(42);
    asm volatile("" : "+r"(p1) : : "memory");
    int *p2 = new int[5]{1, 2, 3, 4, 5};
    asm volatile("" : "+r"(p2) : : "memory");
    int *p3 = new (std::nothrow) int(99);
    asm volatile("" : "+r"(p3) : : "memory");
    int *p4 = new (std::nothrow) int[8];
    asm volatile("" : "+r"(p4) : : "memory");
    auto *p5 =
        static_cast<int *>(operator new(sizeof(int), std::align_val_t{64}));
    asm volatile("" : "+r"(p5) : : "memory");
    auto *p6 = static_cast<int *>(operator new[](sizeof(int) * 4,
                                                 std::align_val_t{64}));
    asm volatile("" : "+r"(p6) : : "memory");
    auto *p7 = static_cast<int *>(operator new(
        sizeof(int), std::align_val_t{64}, std::nothrow));
    asm volatile("" : "+r"(p7) : : "memory");
    auto *p8 = static_cast<int *>(operator new[](
        sizeof(int) * 4, std::align_val_t{64}, std::nothrow));
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

  // 2. Audio Thread intentional allocations: must be detected and increment
  // category counters
  {
    AudioThreadScopeGuard guard;

    // P1-4-R1-AC-01: Aligned + Nothrow scalar new
    auto *pAlignedNothrowScalar = static_cast<int *>(operator new(
        sizeof(int), std::align_val_t{64}, std::nothrow));
    asm volatile("" : "+r"(pAlignedNothrowScalar) : : "memory");
    QVERIFY(pAlignedNothrowScalar != nullptr);
    QCOMPARE(reinterpret_cast<uintptr_t>(pAlignedNothrowScalar) % 64,
             uintptr_t(0));
    QCOMPARE(s_audioAlignedNothrowNewCount.load(), size_t(1));

    // P1-4-R1-AC-02: Aligned + Nothrow array new[]
    auto *pAlignedNothrowArray = static_cast<int *>(operator new[](
        sizeof(int) * 16, std::align_val_t{64}, std::nothrow));
    asm volatile("" : "+r"(pAlignedNothrowArray) : : "memory");
    QVERIFY(pAlignedNothrowArray != nullptr);
    QCOMPARE(reinterpret_cast<uintptr_t>(pAlignedNothrowArray) % 64,
             uintptr_t(0));
    QCOMPARE(s_audioAlignedNothrowNewCount.load(), size_t(2));

    // Aligned normal scalar & array
    auto *pAlignedScalar =
        static_cast<int *>(operator new(sizeof(int), std::align_val_t{64}));
    asm volatile("" : "+r"(pAlignedScalar) : : "memory");
    auto *pAlignedArray = static_cast<int *>(operator new[](
        sizeof(int) * 8, std::align_val_t{64}));
    asm volatile("" : "+r"(pAlignedArray) : : "memory");
    QCOMPARE(s_audioAlignedNewCount.load(), size_t(2));

    // Normal scalar & array
    int *pScalar = new int(123);
    asm volatile("" : "+r"(pScalar) : : "memory");
    int *pArray = new int[4]{1, 2, 3, 4};
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

void TestPhase2Dsp::
    testDspParameterMailboxConsumedAppliedAndSupersededConsistency() {
  // P1-4-R2 & P1-4-R3: Verify accurate separation of Published, Consumed,
  // Applied, Superseded, and Pending/Unconsumed updates, along with unapplied
  // diagnostic tracking.
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
  QCOMPARE(pipeline.paramMailbox().pendingUpdatesCount(),
           uint64_t(1)); // Exactly 1 pending

  // 3. Publish 3 more updates without processing -> previous 3 are superseded,
  // 1 is pending
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
  // Consumed <= Published, Applied <= Consumed, unapplied detected when target
  // node not present
  QVERIFY(con <= pub);
  QVERIFY(app <= con);
  QCOMPARE(
      app,
      uint64_t(0)); // Unapplied because node is not present in empty pipeline
  uint64_t unapplied = con - app;
  QCOMPARE(unapplied,
           uint64_t(1)); // Diagnosed: 1 update consumed but unapplied

  // 5. Attach master chain with FilterNode and publish update with modified
  // mask
  auto masterChain =
      std::make_unique<AudioChain>("Master", ChainPurpose::MasteringChain);
  masterChain->appendNode(std::make_shared<HighLowPassNode>("Filter"));
  pipeline.swapMasterChain(std::move(masterChain));

  pipeline.paramMailbox().setFilterParams(200.0f, 4000.0f, 1, 0, true, true);
  pipeline.process(left.data(), right.data(), frames, out.data(),
                   desk32::engine::format::SampleFormat::Float32, false);

  // Now: Published = 5, Consumed = 2, Applied = 1 (this new one was applied!),
  // Pending = 0, Superseded = 3
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
  // P1-2: Verify that toggling node bypass does NOT destroy or reset parameter
  // values
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

void TestPhase2Dsp::testSeekObservabilityLogging() {
  auto writeTestWav = [](const QString &path, uint32_t sampleRate,
                         uint32_t numFrames) -> bool {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
      return false;
    const uint16_t channels = 2;
    const uint16_t bitsPerSample = 16;
    const uint32_t dataSize = numFrames * channels * (bitsPerSample / 8);
    const uint32_t riffSize = 36 + dataSize;
    const uint32_t byteRate = sampleRate * channels * (bitsPerSample / 8);
    const uint16_t blockAlign = channels * (bitsPerSample / 8);

    QByteArray hdr;
    hdr.append("RIFF", 4);
    hdr.append(reinterpret_cast<const char *>(&riffSize), 4);
    hdr.append("WAVE", 4);
    hdr.append("fmt ", 4);
    const uint32_t fmtSize = 16;
    const uint16_t audioFmt = 1;
    hdr.append(reinterpret_cast<const char *>(&fmtSize), 4);
    hdr.append(reinterpret_cast<const char *>(&audioFmt), 2);
    hdr.append(reinterpret_cast<const char *>(&channels), 2);
    hdr.append(reinterpret_cast<const char *>(&sampleRate), 4);
    hdr.append(reinterpret_cast<const char *>(&byteRate), 4);
    hdr.append(reinterpret_cast<const char *>(&blockAlign), 2);
    hdr.append(reinterpret_cast<const char *>(&bitsPerSample), 2);
    hdr.append("data", 4);
    hdr.append(reinterpret_cast<const char *>(&dataSize), 4);
    f.write(hdr);

    QByteArray pcm(static_cast<int>(dataSize), '\0');
    f.write(pcm);
    f.close();
    return true;
  };

  const QString wavA = m_tempDir->path() + "/seek_test_a.wav";
  const QString wavB = m_tempDir->path() + "/seek_test_b.wav";
  QVERIFY(writeTestWav(wavA, 48000, 48000 * 3));
  QVERIFY(writeTestWav(wavB, 48000, 48000 * 3));

  auto &ctrl = desk32::core::EngineController::instance();
  QVERIFY(ctrl.initialize(true));

  auto &logSvc = desk32::core::log::LogService::instance();

  // 1. Seek on unopened Deck D -> Failed with error_code 1010 and stage
  // "not_open"
  ctrl.seek("D", 4800);
  logSvc.flush();
  {
    desk32::core::log::LogFilter f;
    f.category = "Transport";
    auto events = logSvc.queryLogs(f);
    bool foundNotOpen = false;
    for (const auto &ev : events) {
      if (ev.operation == "Seek" && ev.target == "D") {
        foundNotOpen = true;
        QCOMPARE(ev.status, std::string("Failed"));
        QCOMPARE(ev.errorCode, 1010);
        QJsonObject d =
            QJsonDocument::fromJson(QByteArray::fromStdString(ev.detailsJson))
                .object();
        QCOMPARE(d["v"].toInt(), 1);
        QVERIFY(!d["session_id"].toString().isEmpty());
        QCOMPARE(d["trigger"].toString(), QString("user"));
        QCOMPARE(d["stage"].toString(), QString("not_open"));
        break;
      }
    }
    QVERIFY(foundNotOpen);
  }

  // 2. Load tracks on A & B, seek within buffer on Deck A
  QVERIFY(ctrl.loadTrack("A", wavA));
  QVERIFY(ctrl.loadTrack("B", wavB));
  std::this_thread::sleep_for(std::chrono::milliseconds(
      30)); // Allow decoder worker to prime ring buffer

  ctrl.seek("A", 1024);
  logSvc.flush();
  {
    desk32::core::log::LogFilter f;
    f.category = "Transport";
    auto events = logSvc.queryLogs(f);
    bool foundUserSeek = false;
    for (const auto &ev : events) {
      if (ev.operation == "Seek" && ev.target == "A" &&
          ev.status == "Success") {
        foundUserSeek = true;
        QCOMPARE(ev.errorCode, 0);
        QVERIFY(ev.durationMs >= 0.0);
        QJsonObject d =
            QJsonDocument::fromJson(QByteArray::fromStdString(ev.detailsJson))
                .object();
        QCOMPARE(d["deck"].toString(), QString("A"));
        QCOMPARE(d["trigger"].toString(), QString("user"));
        QCOMPARE(d["path"].toString(), wavA);
        QCOMPARE(d["sample_rate_hz"].toDouble(), 48000.0);
        QVERIFY(d.contains("position_ms_before"));
        QVERIFY(d.contains("requested_position_ms"));
        QVERIFY(d.contains("position_ms_after"));
        QCOMPARE(d["position_ms_after"].toDouble(),
                 d["actual_position_ms"].toDouble());
        QCOMPARE(d["within_buffer"].toBool(), true);
        break;
      }
    }
    QVERIFY(foundUserSeek);
  }

  // 3. Out-of-range seek on Deck A -> Failed with error_code 1011
  ctrl.seek("A", 48000 * 10);
  logSvc.flush();
  {
    desk32::core::log::LogFilter f;
    f.category = "Transport";
    auto events = logSvc.queryLogs(f);
    bool foundOor = false;
    for (const auto &ev : events) {
      if (ev.operation == "Seek" && ev.target == "A" && ev.status == "Failed") {
        foundOor = true;
        QCOMPARE(ev.errorCode, 1011);
        QJsonObject d =
            QJsonDocument::fromJson(QByteArray::fromStdString(ev.detailsJson))
                .object();
        QCOMPARE(d["stage"].toString(), QString("out_of_range"));
        break;
      }
    }
    QVERIFY(foundOor);
  }

  // 4. Comparative switch A -> B -> emits Seek with trigger == "compare_switch"
  ctrl.switchDeck("B", 1);
  logSvc.flush();
  {
    desk32::core::log::LogFilter f;
    f.category = "Transport";
    auto events = logSvc.queryLogs(f);
    int compareSeekCount = 0;
    for (const auto &ev : events) {
      if (ev.operation == "Seek" && ev.target == "B") {
        QJsonObject d =
            QJsonDocument::fromJson(QByteArray::fromStdString(ev.detailsJson))
                .object();
        if (d["trigger"].toString() == "compare_switch") {
          ++compareSeekCount;
          QCOMPARE(d["path"].toString(), wavB);
          QCOMPARE(d["sample_rate_hz"].toDouble(), 48000.0);
        }
      }
    }
    QCOMPARE(compareSeekCount, 1);
  }
}

void TestPhase2Dsp::testSessionAndFolderObservabilityLogging() {
  auto &ctrl = desk32::core::EngineController::instance();
  auto &logSvc = desk32::core::log::LogService::instance();

  // 1. SessionStart
  ctrl.recordSessionStart(QStringLiteral("Vulkan"), QStringLiteral("TestGPU"));
  logSvc.flush();
  {
    desk32::core::log::LogFilter f;
    f.category = "Session";
    auto events = logSvc.queryLogs(f);
    bool foundStart = false;
    for (const auto &ev : events) {
      if (ev.operation == "SessionStart") {
        foundStart = true;
        QJsonObject d =
            QJsonDocument::fromJson(QByteArray::fromStdString(ev.detailsJson))
                .object();
        QCOMPARE(d["v"].toInt(), 1);
        QVERIFY(!d["session_id"].toString().isEmpty());
        QCOMPARE(d["graphics_backend"].toString(), QString("Vulkan"));
        QCOMPARE(d["gpu_name"].toString(), QString("TestGPU"));
        QVERIFY(d.contains("git_commit"));
        QVERIFY(d.contains("qt_version"));
        QVERIFY(d.contains("pipewire_device"));
        QVERIFY(d.contains("pipewire_quantum"));
        QVERIFY(d.contains("pipewire_requested_quantum"));
        break;
      }
    }
    QVERIFY(foundStart);

    // Verify fromUtc / toUtc filtering via QML-facing fetchLogs API
    QVariantMap utcFilter;
    utcFilter["category"] = QStringLiteral("Session");
    utcFilter["fromUtc"] = QStringLiteral("2020-01-01T00:00:00Z");
    utcFilter["toUtc"] = QStringLiteral("2099-12-31T23:59:59Z");
    QVERIFY(!logSvc.fetchLogs(utcFilter).isEmpty());

    QVariantMap emptyRangeFilter;
    emptyRangeFilter["category"] = QStringLiteral("Session");
    emptyRangeFilter["fromUtc"] = QStringLiteral("2099-01-01T00:00:00Z");
    QVERIFY(logSvc.fetchLogs(emptyRangeFilter).isEmpty());
  }

  // 2. FolderSession Start + Manual AdvanceNext
  QDir folderDir(m_tempDir->path() + "/folder_obs");
  QVERIFY(QDir().mkpath(folderDir.path()));
  auto writeTinyWav = [](const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
      return false;
    const uint32_t sampleRate = 48000;
    const uint16_t channels = 2;
    const uint16_t bits = 16;
    const uint32_t numFrames = 4800;
    const uint32_t dataSize = numFrames * channels * (bits / 8);
    const uint32_t riffSize = 36 + dataSize;
    const uint32_t byteRate = sampleRate * channels * (bits / 8);
    const uint16_t blockAlign = channels * (bits / 8);
    const uint32_t fmtSize = 16;
    const uint16_t audioFmt = 1;
    QByteArray hdr;
    hdr.append("RIFF", 4);
    hdr.append(reinterpret_cast<const char *>(&riffSize), 4);
    hdr.append("WAVE", 4);
    hdr.append("fmt ", 4);
    hdr.append(reinterpret_cast<const char *>(&fmtSize), 4);
    hdr.append(reinterpret_cast<const char *>(&audioFmt), 2);
    hdr.append(reinterpret_cast<const char *>(&channels), 2);
    hdr.append(reinterpret_cast<const char *>(&sampleRate), 4);
    hdr.append(reinterpret_cast<const char *>(&byteRate), 4);
    hdr.append(reinterpret_cast<const char *>(&blockAlign), 2);
    hdr.append(reinterpret_cast<const char *>(&bits), 2);
    hdr.append("data", 4);
    hdr.append(reinterpret_cast<const char *>(&dataSize), 4);
    f.write(hdr);
    f.write(QByteArray(static_cast<int>(dataSize), '\0'));
    f.close();
    return true;
  };
  QVERIFY(writeTinyWav(folderDir.filePath("01_first.wav")));
  QVERIFY(writeTinyWav(folderDir.filePath("02_second.wav")));

  QVERIFY(ctrl.startFolderSession("C", folderDir.path(),
                                  {"01_first.wav", "02_second.wav"}));
  QVERIFY(ctrl.advanceFolderNext("C"));
  logSvc.flush();
  {
    desk32::core::log::LogFilter f;
    f.category = "FolderSession";
    auto events = logSvc.queryLogs(f);
    bool foundStart = false;
    bool foundAdv = false;
    for (const auto &ev : events) {
      QJsonObject d =
          QJsonDocument::fromJson(QByteArray::fromStdString(ev.detailsJson))
              .object();
      if (ev.operation == "StartSession" && d["deck"].toString() == "C") {
        foundStart = true;
        QCOMPARE(d["track_count"].toInt(), 2);
      }
      if (ev.operation == "AdvanceNext" && d["deck"].toString() == "C") {
        foundAdv = true;
        QCOMPARE(d["track_index_from"].toInt(), 0);
        QCOMPARE(d["track_index_to"].toInt(), 1);
        QCOMPARE(d["trigger"].toString(), QString("user"));
      }
    }
    QVERIFY(foundStart);
    QVERIFY(foundAdv);
  }

  // 3. DSP ParameterChange commit
  QVariantMap beforeMap{{"gain_db", 0.0}};
  QVariantMap afterMap{{"gain_db", 3.5}};
  ctrl.commitDspParameterChange("PEQ", beforeMap, afterMap, "user");
  logSvc.flush();
  {
    desk32::core::log::LogFilter f;
    f.category = "DSP";
    auto events = logSvc.queryLogs(f);
    bool foundParam = false;
    for (const auto &ev : events) {
      if (ev.operation == "ParameterChange") {
        foundParam = true;
        QJsonObject d =
            QJsonDocument::fromJson(QByteArray::fromStdString(ev.detailsJson))
                .object();
        QCOMPARE(d["dsp_node"].toString(), QString("PEQ"));
        QCOMPARE(d["after"].toObject()["gain_db"].toDouble(), 3.5);
        break;
      }
    }
    QVERIFY(foundParam);
  }

  // 4. SessionEnd
  ctrl.recordSessionEnd(QStringLiteral("test_complete"));
  logSvc.flush();
  {
    desk32::core::log::LogFilter f;
    f.category = "Session";
    auto events = logSvc.queryLogs(f);
    bool foundEnd = false;
    for (const auto &ev : events) {
      if (ev.operation == "SessionEnd") {
        foundEnd = true;
        QJsonObject d =
            QJsonDocument::fromJson(QByteArray::fromStdString(ev.detailsJson))
                .object();
        QCOMPARE(d["reason"].toString(), QString("test_complete"));
        QVERIFY(d["uptime_ms"].toDouble() >= 0.0);
        break;
      }
    }
    QVERIFY(foundEnd);
  }
}

void TestPhase2Dsp::testAutoAdvanceLoggingAndNoDoubleAdvance() {
  auto &ctrl = desk32::core::EngineController::instance();
  if (!ctrl.deckManager()) {
    QVERIFY(ctrl.initialize(true));
  }
  auto &logSvc = desk32::core::log::LogService::instance();

  QTemporaryDir folderDir;
  QVERIFY(folderDir.isValid());
  auto writeTinyWav = [](const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
      return false;
    const uint32_t sr = 48000;
    const uint16_t ch = 2;
    const uint16_t bits = 16;
    const uint32_t frames = 2400; // 50ms
    const uint32_t dataSize = frames * ch * (bits / 8);
    const uint32_t riffSize = 36 + dataSize;
    QByteArray hdr;
    hdr.append("RIFF", 4);
    hdr.append(reinterpret_cast<const char *>(&riffSize), 4);
    hdr.append("WAVEfmt ", 8);
    const uint32_t fmtSize = 16;
    const uint16_t pcmFmt = 1;
    const uint32_t byteRate = sr * ch * (bits / 8);
    const uint16_t blockAlign = ch * (bits / 8);
    hdr.append(reinterpret_cast<const char *>(&fmtSize), 4);
    hdr.append(reinterpret_cast<const char *>(&pcmFmt), 2);
    hdr.append(reinterpret_cast<const char *>(&ch), 2);
    hdr.append(reinterpret_cast<const char *>(&sr), 4);
    hdr.append(reinterpret_cast<const char *>(&byteRate), 4);
    hdr.append(reinterpret_cast<const char *>(&blockAlign), 2);
    hdr.append(reinterpret_cast<const char *>(&bits), 2);
    hdr.append("data", 4);
    hdr.append(reinterpret_cast<const char *>(&dataSize), 4);
    f.write(hdr);
    f.write(QByteArray(static_cast<int>(dataSize), '\0'));
    f.close();
    return true;
  };
  QVERIFY(writeTinyWav(folderDir.filePath("01_a.wav")));
  QVERIFY(writeTinyWav(folderDir.filePath("02_b.wav")));
  QVERIFY(writeTinyWav(folderDir.filePath("03_c.wav")));

  ctrl.setDeckLocked("A", false);
  QVERIFY(ctrl.startFolderSession("A", folderDir.path(),
                                  {"01_a.wav", "02_b.wav", "03_c.wav"}));
  ctrl.play("A");

  auto *deckA = ctrl.deckManager()->deck('A');
  QVERIFY(deckA != nullptr);

  // Render track 0 to EOF via offline audio callback simulation
  std::vector<float> out(2048 * 2, 0.0f);
  for (int i = 0; i < 6; ++i) {
    ctrl.renderOfflineBlock(out.data(), 2048);
  }

  // Also inject a duplicate EOF notification for the same generation/track
  desk32::engine::deck::EofNotification dup;
  dup.playbackGeneration = deckA->playbackGeneration();
  dup.decoderRequestId = deckA->worker().committedRequestId();
  dup.folderSessionId = deckA->folderSession().sessionId();
  dup.trackIndex = deckA->folderSession().currentIndex();
  dup.trackPathHash = deckA->currentTrackPathHash();
  dup.source = desk32::engine::deck::EofSource::SyntheticTest;
  QVERIFY(deckA->submitEofNotification(dup));

  // Process events so the 50ms trackTimer fires
  QTest::qWait(120);
  QCoreApplication::processEvents();
  QCOMPARE(deckA->folderSession().currentIndex(), size_t(1));

  logSvc.flush();
  desk32::core::log::LogFilter f;
  f.category = "FolderSession";
  const auto events = logSvc.queryLogs(f);

  int autoAdvCount = 0;
  int validEofCount = 0;
  int ignoredEofCount = 0;
  for (const auto &ev : events) {
    QJsonObject d =
        QJsonDocument::fromJson(QByteArray::fromStdString(ev.detailsJson))
            .object();
    if (ev.operation == "AutoAdvance" && ev.status == "Success") {
      ++autoAdvCount;
      QVERIFY(d.contains("session_id"));
      QVERIFY(d.contains("folder_session_id"));
      QVERIFY(d.contains("advance_seq"));
      QVERIFY(d.contains("playback_generation"));
      QVERIFY(d.contains("decoder_request_id"));
      QVERIFY(d.contains("track_index_from"));
      QVERIFY(d.contains("track_index_to"));
      QVERIFY(d.contains("path_from"));
      QVERIFY(d.contains("path_to"));
      QVERIFY(d.contains("trigger"));
      QVERIFY(d.contains("event_source"));
      QVERIFY(d.contains("event_timestamp"));
      QVERIFY(d.contains("transition_start_timestamp"));
      QVERIFY(d.contains("transition_complete_timestamp"));
      QCOMPARE(d["result"].toString(), QString("success"));
    } else if (ev.operation == "EofNotification") {
      if (d["valid"].toBool()) {
        ++validEofCount;
      } else {
        ++ignoredEofCount;
        QVERIFY(!d["invalid_reason"].toString().isEmpty());
      }
    }
  }
  QCOMPARE(autoAdvCount, 1);
  QCOMPARE(validEofCount, 1);
  QCOMPARE(ignoredEofCount, 1);
}

void TestPhase2Dsp::testMasterGainPostTapIsolation() {
  using namespace desk32::engine::pipeline;
  using namespace desk32::engine::dsp;
  AudioPipeline pipe(48000.0);
  ParameterSmoother smoothL(1.0, 1.0, 48000.0);
  ParameterSmoother smoothR(1.0, 1.0, 48000.0);

  const size_t N = 512;
  std::vector<double> L(N, 0.5);
  std::vector<double> R(N, 0.5);
  std::vector<float> pcm(N * 2, 0.0f);

  smoothL.reset(0.1);
  smoothR.reset(0.1);
  pipe.process(L.data(), R.data(), N, pcm.data(),
               desk32::engine::format::SampleFormat::Float32, false, &smoothL,
               &smoothR, 0.5, false);

  const DspTelemetry tel = pipe.currentTelemetry();
  // Logical Render Tap must reflect pre-fader, pre-level-match signal (~0.5)
  QVERIFY(std::abs(tel.renderTapPeakL - 0.5) < 1e-4);
  QVERIFY(std::abs(tel.renderTapPeakR - 0.5) < 1e-4);
  // Final master peak must reflect 0.5 * 0.1 * 0.5 = 0.025
  QVERIFY(std::abs(tel.masterPeakL - 0.025) < 1e-3);
  QVERIFY(std::abs(tel.masterPeakR - 0.025) < 1e-3);
}

void TestPhase2Dsp::
    testPhase2GapFixesBitPerfectGeqListeningMetersAndSmoothing() {
  using namespace desk32::engine::pipeline;
  using namespace desk32::engine::routing;
  using namespace desk32::engine::preset;
  using desk32::core::EngineController;

  // 1. Item 8 & Item 14: AudioPipeline Direct Mode zero-dither, Level 1
  // comparison & Pre-DSP Peak/RMS
  {
    AudioPipeline pipe(48000.0);
    auto masterChain =
        std::make_unique<AudioChain>("MC", ChainPurpose::MasteringChain);
    masterChain->appendNode(std::make_shared<GraphicEqNode>("Graphic EQ"));
    pipe.swapMasterChain(std::move(masterChain));
    ParameterSmoother sL(1.0, 1.0, 48000.0), sR(1.0, 1.0, 48000.0);
    sL.reset(0.25);
    sR.reset(0.25);

    constexpr size_t N = 256;
    std::vector<double> L(N, 0.5), R(N, 0.5);
    std::vector<int32_t> pcmInt(N * 2, 0);
    pipe.process(L.data(), R.data(), N, pcmInt.data(),
                 desk32::engine::format::SampleFormat::Int24_32, true, &sL, &sR,
                 1.0, /*directModeBypass=*/true);
    const DspTelemetry directTel = pipe.currentTelemetry();
    QVERIFY(directTel.level1SamplesIdentical);
    QCOMPARE(directTel.level1MaxAbsDiff, 0.0);
    QVERIFY(directTel.level1VerifiedBlocks >= 1);
    QVERIFY(std::abs(directTel.preDspPeakL - 0.5) < 1e-9);
    QVERIFY(std::abs(directTel.preDspRmsL - 0.5) < 1e-9);

    // Non-direct mode with post-tap attenuation: Pre-DSP remains 0.5 while
    // Master Peak is attenuated (~0.125)
    pipe.process(L.data(), R.data(), N, pcmInt.data(),
                 desk32::engine::format::SampleFormat::Int24_32, false, &sL,
                 &sR, 1.0, /*directModeBypass=*/false);
    const DspTelemetry postTel = pipe.currentTelemetry();
    QVERIFY(std::abs(postTel.preDspPeakL - 0.5) < 1e-9);
    QVERIFY(std::abs(postTel.masterPeakL - 0.125) < 1e-3);
  }

  // 2. Item 15: OutputPathRouter & AudioPipeline volume smoothing
  {
    OutputPathRouter router;
    router.setSampleRate(48000.0);
    router.setMode(OutputPathMode::RenderMonitorPath);
    router.setRenderMonitorVolume(1.0);
    std::vector<double> in(256 * 2, 1.0), out(256 * 2, 0.0);
    router.routeBlock(in.data(), out.data(), 256); // primes smoother at 1.0
    QCOMPARE(out[0], 1.0);
    router.setRenderMonitorVolume(0.2);
    router.routeBlock(in.data(), out.data(), 256);
    // Should smooth gradually rather than jumping straight to 0.2 on frame 0
    QVERIFY(out[0] > 0.5 && out[0] < 1.0);
    QVERIFY(out[255 * 2] < out[0]);
  }

  // 3. EngineController Integration (Items 8, 9, 12, 13)
  EngineController &ctrl = EngineController::instance();
  QVERIFY(ctrl.initialize(true));
  PresetManager::instance().setCustomRootPath(m_tempDir->path() +
                                              "/p2_presets");

  // Item 9: Fixed sample rate + 4-state getAudioCapabilities()
  QVERIFY(ctrl.setFixedGraphSampleRate(96000.0));
  QCOMPARE(ctrl.graphSampleRate(), 96000.0);
  const QVariantMap caps = ctrl.getAudioCapabilities();
  QVERIFY(caps.contains("directMode"));
  QVERIFY(caps.contains("sampleRateFollowing"));
  QVERIFY(caps.contains("exclusiveAccess"));
  QVERIFY(caps.contains("hardwareBitPerfect"));

  // Item 8: 5-state bit-perfect & Level 1/2/3 verification report
  const QVariantMap bpReport = ctrl.getBitPerfectVerificationReport();
  QVERIFY(bpReport.contains("modeStateText"));
  QVERIFY(bpReport.contains("level1"));
  QVERIFY(bpReport.contains("level2"));
  QVERIFY(bpReport.contains("level3"));
  QCOMPARE(bpReport["level3"].toMap()["status"].toString(),
           QString("NotVerified"));

  // Item 12: GEQ per-band enable/disable preserves gain while flattening curve
  ctrl.setDspNodeBypass("GEQ", false);
  ctrl.setGeqBandGain(18 /* 1 kHz */, 6.0);
  QVERIFY(ctrl.isGeqBandEnabled(18));
  ctrl.setGeqBandEnabled(18, false);
  QVERIFY(!ctrl.isGeqBandEnabled(18));
  QCOMPARE(ctrl.getGeqBandGain(18), 6.0); // Gain preserved!
  const QVariantList disabledCurve = ctrl.getGeqCurve(64);
  double maxDisabledDb = 0.0;
  for (const auto &pt : disabledCurve) {
    maxDisabledDb =
        std::max(maxDisabledDb, std::abs(pt.toMap()["db"].toDouble()));
  }
  QVERIFY(maxDisabledDb < 0.05);
  ctrl.setGeqBandEnabled(18, true);
  const QVariantList enabledCurve = ctrl.getGeqCurve(64);
  double maxEnabledDb = 0.0;
  for (const auto &pt : enabledCurve) {
    maxEnabledDb =
        std::max(maxEnabledDb, std::abs(pt.toMap()["db"].toDouble()));
  }
  QVERIFY(maxEnabledDb > 5.0);

  // Item 13: Unified Listening Correction presets (9 factory + user L/R 16-band
  // roundtrip)
  const QStringList lcPresets = ctrl.availableListeningPresets();
  QVERIFY(lcPresets.contains("Nearfield Speaker"));
  QVERIFY(lcPresets.contains("Open-Back Headphone"));
  QVERIFY(lcPresets.contains("Closed-Back Headphone"));
  QVERIFY(lcPresets.contains("DJ Club Monitor"));

  const QJsonObject acPreset = PresetManager::instance().loadPreset(
      PresetCategories::ListeningCorrection, "Acoustic Clarity");
  std::array<ParametricEQ::BandConfig, ParametricEQ::BANDS_PER_CHANNEL>
      expectedL{}, expectedR{};
  QVERIFY(ParametricEQ::getPresetBandConfigs("Acoustic Clarity", expectedL,
                                             expectedR));
  QCOMPARE(acPreset["left_bands"].toArray()[5].toObject()["gain_db"].toDouble(),
           expectedL[5].gainDb);

  // Independent L vs R bands + user preset save/load
  ctrl.setListeningBand(false, 4, 250.0, 5.5, 1.4, 0, true);
  ctrl.setListeningBand(true, 4, 250.0, -4.5, 1.4, 0, true);
  QCOMPARE(ctrl.getListeningBand(false, 4)["gainDb"].toDouble(), 5.5);
  QCOMPARE(ctrl.getListeningBand(true, 4)["gainDb"].toDouble(), -4.5);
  QVERIFY(ctrl.savePreset(
      PresetCategories::ListeningCorrection, "My Asymmetric Rig",
      ctrl.captureCurrentStateAsPreset(PresetCategories::ListeningCorrection)));
  ctrl.setListeningPreset("Monitor Flat");
  QCOMPARE(ctrl.getListeningBand(false, 4)["gainDb"].toDouble(), 0.0);
  QVERIFY(ctrl.applyPresetProfile(PresetCategories::ListeningCorrection,
                                  "My Asymmetric Rig"));
  QCOMPARE(ctrl.getListeningBand(false, 4)["gainDb"].toDouble(), 5.5);
  QCOMPARE(ctrl.getListeningBand(true, 4)["gainDb"].toDouble(), -4.5);
}

void TestPhase2Dsp::testPhase4AuditDspReverbContractsOverflowAndSmoothing() {
  using namespace desk32::engine::pipeline;
  using desk32::core::EngineController;
  using namespace desk32::engine::preset;

  // 1. Reverb TailPreserving Bypass + maxTailRetentionSeconds timeout +
  // processing budget cap (§5.6)
  {
    ReverbNode rev("TailRev");
    rev.configure(48000.0);
    rev.setBypassBehavior(BypassBehavior::TailPreserving);
    rev.prepareInstance(48000.0, 0.75, 0.3, 0.5, 0.0, true, true,
                        0.25 /* max 0.25s tail */, 0.0);

    std::vector<double> excL(1024, 0.6), excR(1024, 0.6);
    rev.process(excL.data(), excR.data(), 1024);

    rev.setBypassed(true);
    QVERIFY(rev.wantsProcessWhileBypassed());
    QCOMPARE(rev.nodeState(), NodeState::TailRingout);

    std::vector<double> tailL(512, 0.3), tailR(512, 0.3);
    rev.process(tailL.data(), tailR.data(), 512);
    double maxTailDiff = 0.0;
    for (size_t i = 0; i < 512; ++i) {
      maxTailDiff = std::max(maxTailDiff, std::abs(tailL[i] - 0.3));
    }
    QVERIFY(maxTailDiff >
            1e-4); // Decaying wet tail added to dry 0.3 while bypassed!

    // Exceed 0.25s = 12000 frames retention ceiling -> terminates wet tail
    // cleanly
    for (int b = 0; b < 30; ++b) {
      std::fill(tailL.begin(), tailL.end(), 0.3);
      std::fill(tailR.begin(), tailR.end(), 0.3);
      rev.process(tailL.data(), tailR.data(), 512);
    }
    QCOMPARE(tailL[0], 0.3);
    QVERIFY(rev.tailRetentionTimeoutRetireCount() >= 1);
  }

  // 2. ParametricEQ >16-band variable overflow storage (§5.2) + Controller
  // warning (§11.1)
  {
    ParametricEQ peq;
    std::vector<ParametricEQ::BandConfig> twentyBands(20);
    for (size_t i = 0; i < 20; ++i) {
      twentyBands[i].frequency = 50.0 + static_cast<double>(i) * 400.0;
      twentyBands[i].gainDb = 1.5;
      twentyBands[i].Q = 1.2;
      twentyBands[i].enabled = true;
    }
    const auto rep = peq.importBandsWithReport(twentyBands, twentyBands);
    QVERIFY(rep.truncated);
    QCOMPARE(rep.requestedLeft, size_t(20));
    QCOMPARE(rep.activeLeft, ParametricEQ::BANDS_PER_CHANNEL);
    QCOMPARE(rep.overflowLeft, size_t(4));
    QCOMPARE(peq.overflowLeftBands().size(), size_t(4));
    QCOMPARE(peq.exportBands(false).size(), size_t(20));
  }

  // 3. BiquadFilter coefficient smoothing + AudioChain post-init bypass
  // crossfade (0 RT alloc) (§5.9)
  {
    BiquadFilter filt;
    filt.setCoefficients(BiquadFilter::calculateCoefficients(
        BiquadFilterType::Peak, 48000.0, 1000.0, 1.0, 0.0));
    const auto boost = BiquadFilter::calculateCoefficients(
        BiquadFilterType::Peak, 48000.0, 1000.0, 1.0, 12.0);
    filt.setCoefficientsSmooth(boost, 64);
    QVERIFY(filt.isSmoothingCoefficients());
    for (int s = 0; s < 64; ++s) {
      (void)filt.process(0.1);
    }
    QVERIFY(!filt.isSmoothingCoefficients());

    AudioChain chain("SmoothChain", ChainPurpose::MasteringChain);
    auto peqNode = std::make_shared<ParametricEqNode>("Master PEQ");
    peqNode->configure(48000.0);
    chain.appendNode(peqNode);
    chain.configure(48000.0);
    chain.setBypassed(false);

    std::vector<double> l(256, 0.5), r(256, 0.5);
    chain.process(l.data(), r.data(), 256); // Block 0: prime chain

    chain.setBypassed(true);
    std::fill(l.begin(), l.end(), 0.5);
    std::fill(r.begin(), r.end(), 0.5);
    // Zero RT heap allocation check during bypass crossfade
    s_audioThreadAllocationCount.store(0, std::memory_order_relaxed);
    s_trackAllocations.store(true, std::memory_order_relaxed);
    {
      AudioThreadScopeGuard rtGuard;
      chain.process(l.data(), r.data(), 256);
    }
    s_trackAllocations.store(false, std::memory_order_relaxed);
    QCOMPARE(s_audioThreadAllocationCount.load(), size_t(0));
  }

  // 4. EngineController §8.2 4-state capabilities, §9.2 node contracts,
  // Bit-Perfect Level 1 independent of Direct Mode, and >16-band preset
  // truncation warning signal + LogService entry
  EngineController &ctrl = EngineController::instance();
  QVERIFY(ctrl.initialize(true));
  PresetManager::instance().setCustomRootPath(m_tempDir->path() +
                                              "/p2_audit_presets");

  const QVariantMap caps = ctrl.getAudioCapabilities();
  QVERIFY(caps["directMode"].toString() == "Unknown" ||
          caps["directMode"].toString() == "Supported" ||
          caps["directMode"].toString() == "Unavailable" ||
          caps["directMode"].toString() == "Unsupported");
  QCOMPARE(caps["exclusiveAccess"].toString(), QString("Unsupported"));
  QCOMPARE(caps["hardwareBitPerfect"].toString(), QString("Unknown"));

  const QVariantList contracts = ctrl.getAudioNodeContracts();
  QVERIFY(contracts.size() >= 8);
  QCOMPARE(contracts[0].toMap()["sampleFormat"].toString(),
           QString("Float64Planar"));
  QCOMPARE(contracts[0].toMap()["inputChannels"].toInt(), 2);
  QCOMPARE(contracts[0].toMap()["outputChannels"].toInt(), 2);

  // Independent Bit-Perfect Level 1 with Direct Mode OFF + dither OFF +
  // lossless F32LE
  ctrl.setDirectModeEnabled(false);
  ctrl.setGlobalDspBypass(true);
  ctrl.setMasterMute(false);
  ctrl.setMasterGain(1.0);
  ctrl.setListeningVolume(1.0);
  ctrl.setListeningMuted(false);
  ctrl.setOutputPcmFormat("F32LE");
  ctrl.setLiveDitherEnabled(false);

  std::vector<float> dummyOut(256 * 2, 0.0f);
  ctrl.renderOfflineBlock(dummyOut.data(), 256);
  const QVariantMap bpIndependent = ctrl.getBitPerfectVerificationReport();
  QCOMPARE(bpIndependent["level1"].toMap()["status"].toString(),
           QString("Verified"));
  QCOMPARE(bpIndependent["level1"].toMap()["ditherActive"].toBool(), false);
  QVERIFY(bpIndependent["level1"].toMap()["outputPcmLossless"].toBool());
  QVERIFY(bpIndependent["level1"].toMap()["verifiedBlocks"].toULongLong() >= 1);
  QVERIFY(bpIndependent["level1"].toMap()["totalBlocks"].toULongLong() >=
          bpIndependent["level1"].toMap()["verifiedBlocks"].toULongLong());
  QCOMPARE(bpIndependent["level1"].toMap()["hashMatched"].toBool(), true);
  QCOMPARE(bpIndependent["level1"].toMap()["inputHashHex"].toString(),
           bpIndependent["level1"].toMap()["outputHashHex"].toString());
  QVERIFY(
      !bpIndependent["level2"].toMap()["backendStatus"].toString().isEmpty());
  QCOMPARE(bpIndependent["level3"].toMap()["unverifiable"].toBool(), true);
  QCOMPARE(bpIndependent["level3"]
               .toMap()["externalHardwareCaptureRequired"]
               .toBool(),
           true);

  // Live dither toggle on integer quantization format S24_32LE (§3.4 / §4.3)
  ctrl.setOutputPcmFormat("S24_32LE");
  std::vector<int32_t> rawInt32(256 * 2, 0);
  ctrl.setLiveDitherEnabled(false);
  ctrl.renderOfflineRawBlock(rawInt32.data(), 256);
  const QVariantMap bpS24Clean = ctrl.getBitPerfectVerificationReport();
  QCOMPARE(bpS24Clean["level1"].toMap()["status"].toString(),
           QString("Verified"));
  QCOMPARE(bpS24Clean["level1"].toMap()["ditherActive"].toBool(), false);
  QVERIFY(bpS24Clean["level1"].toMap()["outputPcmLossless"].toBool());

  // Re-enable live dither on S24_32LE -> Level 1 & bitPerfectStatus immediately
  // reflect TPDF dither
  ctrl.setLiveDitherEnabled(true);
  ctrl.renderOfflineRawBlock(rawInt32.data(), 256);
  const QVariantMap bpS24Dither = ctrl.getBitPerfectVerificationReport();
  QCOMPARE(bpS24Dither["level1"].toMap()["status"].toString(),
           QString("NotVerified"));
  QCOMPARE(bpS24Dither["level1"].toMap()["ditherActive"].toBool(), true);
  QCOMPARE(ctrl.bitPerfectStatus(), 1);
  ctrl.setOutputPcmFormat("F32LE");

  // Empty deck MUST remain Indeterminate even with Direct Mode ON and offline
  // blocks processed
  ctrl.setOutputRoutingMode(1);
  ctrl.setDirectModeEnabled(true);
  ctrl.renderOfflineBlock(dummyOut.data(), 256);
  QCOMPARE(ctrl.bitPerfectStatus(), 0);
  QCOMPARE(ctrl.bitPerfectModeState(), 1);
  ctrl.setDirectModeEnabled(false);
  ctrl.renderOfflineBlock(dummyOut.data(), 256);
  QCOMPARE(ctrl.bitPerfectStatus(), 0);
  QCOMPARE(ctrl.bitPerfectModeState(), 1);
  ctrl.setOutputRoutingMode(0);

  // Transition to Verified during active track playback logs 1
  // BitPerfectVerified report without duplicates
  {
    const QString bpWav = m_tempDir->path() + "/bp_verify_track.wav";
    QFile wf(bpWav);
    QVERIFY(wf.open(QIODevice::WriteOnly));
    const uint32_t sr = 48000, nFrames = 4800, dataSz = nFrames * 2 * 2,
                   riffSz = 36 + dataSz, br = sr * 4;
    const uint16_t pcmFmt = 1, ch = 2, bits = 16, ba = 4;
    const uint32_t fmtSz = 16;
    QByteArray hdr;
    hdr.append("RIFF", 4);
    hdr.append(reinterpret_cast<const char *>(&riffSz), 4);
    hdr.append("WAVE", 4);
    hdr.append("fmt ", 4);
    hdr.append(reinterpret_cast<const char *>(&fmtSz), 4);
    hdr.append(reinterpret_cast<const char *>(&pcmFmt), 2);
    hdr.append(reinterpret_cast<const char *>(&ch), 2);
    hdr.append(reinterpret_cast<const char *>(&sr), 4);
    hdr.append(reinterpret_cast<const char *>(&br), 4);
    hdr.append(reinterpret_cast<const char *>(&ba), 2);
    hdr.append(reinterpret_cast<const char *>(&bits), 2);
    hdr.append("data", 4);
    hdr.append(reinterpret_cast<const char *>(&dataSz), 4);
    wf.write(hdr);
    wf.write(QByteArray(static_cast<int>(dataSz), '\0'));
    wf.close();

    QVERIFY(ctrl.loadTrack("A", bpWav));
    ctrl.setOutputRoutingMode(
        1); // Render Monitor Path (bypasses Listening Correction)
    ctrl.setPeqBypassed(true);
    ctrl.setGlobalDspBypass(false);
    ctrl.setDirectModeEnabled(false);

    // Before initial L1 block verification for this loaded track -> MUST be
    // Indeterminate
    QCOMPARE(ctrl.bitPerfectStatus(), 0);
    QCOMPARE(ctrl.bitPerfectModeState(), 1);
    QCOMPARE(ctrl.getBitPerfectVerificationReport()["level1"]
                 .toMap()["status"]
                 .toString(),
             QString("Indeterminate"));

    desk32::core::log::LogFilter bpFilter;
    bpFilter.category = "AudioEngine";
    bpFilter.searchText = "BitPerfect";
    desk32::core::log::LogService::instance().flush();
    const size_t beforeCount =
        desk32::core::log::LogService::instance().queryLogs(bpFilter).size();

    ctrl.renderOfflineBlock(dummyOut.data(), 256);
    QCOMPARE(ctrl.bitPerfectStatus(), 2);
    QCOMPARE(ctrl.bitPerfectModeState(), 3);
    ctrl.checkBitPerfectVerificationTransition();
    ctrl.checkBitPerfectVerificationTransition(); // must not duplicate in same
                                                  // Verified state
    desk32::core::log::LogService::instance().flush();
    const auto afterFirst =
        desk32::core::log::LogService::instance().queryLogs(bpFilter);
    QCOMPARE(afterFirst.size(), beforeCount + 1);
    QCOMPARE(QString::fromStdString(afterFirst.front().operation),
             QStringLiteral("BitPerfectVerified"));
    QVERIFY(afterFirst.front().detailsJson.find("\"level1\"") !=
            std::string::npos);
    QVERIFY(afterFirst.front().detailsJson.find("\"level2\"") !=
            std::string::npos);
    QVERIFY(afterFirst.front().detailsJson.find("\"level3\"") !=
            std::string::npos);

    // Toggling Direct Mode ON invalidates L1 -> remains Indeterminate until a
    // post-toggle L1 block is verified
    ctrl.setDirectModeEnabled(true);
    QCOMPARE(ctrl.bitPerfectStatus(), 0);
    QCOMPARE(ctrl.bitPerfectModeState(), 1);
    ctrl.setDirectModeEnabled(false);

    // Drop to NotVerified (PEQ ON) then back to Verified (PEQ OFF) -> logs one
    // more event
    ctrl.setPeqBypassed(false);
    ctrl.checkBitPerfectVerificationTransition();
    ctrl.setPeqBypassed(true);
    ctrl.renderOfflineBlock(dummyOut.data(), 256);
    ctrl.checkBitPerfectVerificationTransition();
    desk32::core::log::LogService::instance().flush();
    const auto afterSecond =
        desk32::core::log::LogService::instance().queryLogs(bpFilter);
    QCOMPARE(afterSecond.size(), beforeCount + 2);

    ctrl.setOutputRoutingMode(0);
    ctrl.stop("A");
  }

  // >16-band Listening Correction preset emits presetImportWarning & sets
  // report
  QSignalSpy warnSpy(&ctrl, &EngineController::presetImportWarning);
  QVariantMap bigPreset;
  QVariantList bigBands;
  for (int i = 0; i < 20; ++i) {
    QVariantMap bm;
    bm["index"] = i;
    bm["frequency"] = 100.0 + i * 500.0;
    bm["gain_db"] = 2.0;
    bm["q"] = 1.0;
    bm["type"] = 0;
    bm["enabled"] = true;
    bigBands.append(bm);
  }
  bigPreset["left_bands"] = bigBands;
  bigPreset["right_bands"] = bigBands;
  QVERIFY(ctrl.savePreset(PresetCategories::ListeningCorrection,
                          "Huge20BandRig", bigPreset));
  QVERIFY(ctrl.applyPresetProfile(PresetCategories::ListeningCorrection,
                                  "Huge20BandRig"));
  QVERIFY(warnSpy.count() >= 1);
  const QVariantMap importRep = ctrl.getLastPresetImportReport();
  QVERIFY(importRep["truncated"].toBool());
  QCOMPARE(importRep["overflowLeftBands"].toInt(), 4);
}

void TestPhase2Dsp::testAutoPauseOtherDeckOnPlay() {
  auto &ctrl = desk32::core::EngineController::instance();
  if (!ctrl.deckManager()) {
    QVERIFY(ctrl.initialize(true));
  }

  QTemporaryDir tempDir;
  QVERIFY(tempDir.isValid());
  auto writeWav = [](const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
      return false;
    const uint32_t sr = 48000;
    const uint16_t ch = 2;
    const uint16_t bits = 16;
    const uint32_t frames = 4800;
    const uint32_t dataSize = frames * ch * (bits / 8);
    const uint32_t riffSize = 36 + dataSize;
    QByteArray hdr;
    hdr.append("RIFF", 4);
    hdr.append(reinterpret_cast<const char *>(&riffSize), 4);
    hdr.append("WAVEfmt ", 8);
    const uint32_t fmtSize = 16;
    const uint16_t pcmFmt = 1;
    const uint32_t byteRate = sr * ch * (bits / 8);
    const uint16_t blockAlign = ch * (bits / 8);
    hdr.append(reinterpret_cast<const char *>(&fmtSize), 4);
    hdr.append(reinterpret_cast<const char *>(&pcmFmt), 2);
    hdr.append(reinterpret_cast<const char *>(&ch), 2);
    hdr.append(reinterpret_cast<const char *>(&sr), 4);
    hdr.append(reinterpret_cast<const char *>(&byteRate), 4);
    hdr.append(reinterpret_cast<const char *>(&blockAlign), 2);
    hdr.append(reinterpret_cast<const char *>(&bits), 2);
    hdr.append("data", 4);
    hdr.append(reinterpret_cast<const char *>(&dataSize), 4);
    f.write(hdr);
    f.write(QByteArray(static_cast<int>(dataSize), '\0'));
    f.close();
    return true;
  };

  const QString wavA = tempDir.filePath("a.wav");
  const QString wavB = tempDir.filePath("b.wav");
  QVERIFY(writeWav(wavA));
  QVERIFY(writeWav(wavB));

  ctrl.setDeckLocked("A", false);
  ctrl.setDeckLocked("B", false);
  ctrl.setSwitchingMode(0); // Normal mode
  QVERIFY(ctrl.loadTrack("A", wavA));
  QVERIFY(ctrl.loadTrack("B", wavB));

  // Play Deck A -> Deck A is Playing
  ctrl.play("A");
  QCOMPARE(ctrl.deckManager()->deck('A')->state(),
           desk32::engine::deck::TransportState::Playing);
  QCOMPARE(ctrl.deckManager()->deck('B')->state(),
           desk32::engine::deck::TransportState::Stopped);

  // Play Deck B while Deck A is playing -> Deck A must auto-pause and Deck B
  // becomes active & Playing
  ctrl.play("B");
  QCOMPARE(ctrl.activeDeckId(), QString("B"));
  QCOMPARE(ctrl.deckManager()->deck('A')->state(),
           desk32::engine::deck::TransportState::Paused);
  QCOMPARE(ctrl.deckManager()->deck('B')->state(),
           desk32::engine::deck::TransportState::Playing);

  // Play Deck A while Deck B is playing -> Deck B must auto-pause
  ctrl.play("A");
  QCOMPARE(ctrl.activeDeckId(), QString("A"));
  QCOMPARE(ctrl.deckManager()->deck('B')->state(),
           desk32::engine::deck::TransportState::Paused);
  QCOMPARE(ctrl.deckManager()->deck('A')->state(),
           desk32::engine::deck::TransportState::Playing);

  // Switch-only (Normal mode) A -> B without pressing Play -> Deck A must
  // auto-pause and Deck B stays Paused
  ctrl.switchDeck("B", 0);
  QCOMPARE(ctrl.activeDeckId(), QString("B"));
  QCOMPARE(ctrl.deckManager()->deck('A')->state(),
           desk32::engine::deck::TransportState::Paused);
  QCOMPARE(ctrl.deckManager()->deck('B')->state(),
           desk32::engine::deck::TransportState::Paused);

  // Folder Session on A -> Play B pauses A -> Switch back to A resumes A &
  // pauses B
  QDir fsDir(tempDir.filePath("folder"));
  QVERIFY(QDir().mkpath(fsDir.path()));
  QVERIFY(writeWav(fsDir.filePath("01.wav")));
  QVERIFY(writeWav(fsDir.filePath("02.wav")));
  QVERIFY(ctrl.startFolderSession("A", fsDir.path(), {"01.wav", "02.wav"}));
  ctrl.play("A");
  QCOMPARE(ctrl.deckManager()->deck('A')->state(),
           desk32::engine::deck::TransportState::Playing);

  ctrl.play("B");
  QCOMPARE(ctrl.deckManager()->deck('A')->state(),
           desk32::engine::deck::TransportState::Paused);
  QVERIFY(ctrl.deckManager()->deck('A')->wasPausedByNormalSwitch());
  QCOMPARE(ctrl.deckManager()->deck('B')->state(),
           desk32::engine::deck::TransportState::Playing);

  ctrl.switchDeck("A", 0);
  QCOMPARE(ctrl.deckManager()->deck('A')->state(),
           desk32::engine::deck::TransportState::Playing);
  QCOMPARE(ctrl.deckManager()->deck('B')->state(),
           desk32::engine::deck::TransportState::Paused);

  ctrl.stop("A");
  ctrl.stop("B");
}

void TestPhase2Dsp::testFolderSessionEndLoggingWithReasons() {
  auto &ctrl = desk32::core::EngineController::instance();
  if (!ctrl.deckManager()) {
    QVERIFY(ctrl.initialize(true));
  }
  auto &logSvc = desk32::core::log::LogService::instance();

  QTemporaryDir tempDir;
  QVERIFY(tempDir.isValid());
  auto writeWav = [](const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
      return false;
    const uint32_t sr = 48000;
    const uint16_t ch = 2;
    const uint16_t bits = 16;
    const uint32_t frames = 2400; // 50 ms
    const uint32_t dataSize = frames * ch * (bits / 8);
    const uint32_t riffSize = 36 + dataSize;
    QByteArray hdr;
    hdr.append("RIFF", 4);
    hdr.append(reinterpret_cast<const char *>(&riffSize), 4);
    hdr.append("WAVEfmt ", 8);
    const uint32_t fmtSize = 16;
    const uint16_t pcmFmt = 1;
    const uint32_t byteRate = sr * ch * (bits / 8);
    const uint16_t blockAlign = ch * (bits / 8);
    hdr.append(reinterpret_cast<const char *>(&fmtSize), 4);
    hdr.append(reinterpret_cast<const char *>(&pcmFmt), 2);
    hdr.append(reinterpret_cast<const char *>(&ch), 2);
    hdr.append(reinterpret_cast<const char *>(&sr), 4);
    hdr.append(reinterpret_cast<const char *>(&byteRate), 4);
    hdr.append(reinterpret_cast<const char *>(&blockAlign), 2);
    hdr.append(reinterpret_cast<const char *>(&bits), 2);
    hdr.append("data", 4);
    hdr.append(reinterpret_cast<const char *>(&dataSize), 4);
    f.write(hdr);
    f.write(QByteArray(static_cast<int>(dataSize), '\0'));
    f.close();
    return true;
  };

  const QString wav1 = tempDir.filePath("01_one.wav");
  const QString wav2 = tempDir.filePath("02_two.wav");
  const QString singleWav = tempDir.filePath("single.wav");
  QVERIFY(writeWav(wav1));
  QVERIFY(writeWav(wav2));
  QVERIFY(writeWav(singleWav));

  ctrl.setDeckLocked("A", false);
  ctrl.setDeckLocked("B", false);

  // 1. user_stop
  QVERIFY(ctrl.startFolderSession("A", tempDir.path(),
                                  {"01_one.wav", "02_two.wav"}));
  ctrl.stop("A");

  // 2. deck_locked
  QVERIFY(ctrl.startFolderSession("A", tempDir.path(),
                                  {"01_one.wav", "02_two.wav"}));
  ctrl.setDeckLocked("A", true);
  ctrl.setDeckLocked("A", false);

  // 3. comparative_switch
  QVERIFY(ctrl.startFolderSession("A", tempDir.path(),
                                  {"01_one.wav", "02_two.wav"}));
  QVERIFY(ctrl.loadTrack("B", singleWav));
  ctrl.switchDeck("B", 1);

  // 4. track_replaced
  QVERIFY(ctrl.startFolderSession("A", tempDir.path(),
                                  {"01_one.wav", "02_two.wav"}));
  QVERIFY(ctrl.loadTrack("A", singleWav));

  // 5. end_of_folder (manual advance past final track)
  QVERIFY(ctrl.startFolderSession("A", tempDir.path(),
                                  {"01_one.wav", "02_two.wav"}));
  QVERIFY(ctrl.advanceFolderNext("A"));
  QVERIFY(!ctrl.advanceFolderNext("A"));

  logSvc.flush();
  desk32::core::log::LogFilter f;
  f.category = "FolderSession";
  const auto events = logSvc.queryLogs(f);

  QSet<QString> reasons;
  for (const auto &ev : events) {
    if (ev.operation == "EndSession") {
      const QJsonObject d =
          QJsonDocument::fromJson(QByteArray::fromStdString(ev.detailsJson))
              .object();
      QVERIFY(d.value("folder_session_id").toInteger() > 0);
      QVERIFY(!d.value("reason").toString().isEmpty());
      reasons.insert(d.value("reason").toString());
    }
  }
  QVERIFY(reasons.contains(QStringLiteral("user_stop")));
  QVERIFY(reasons.contains(QStringLiteral("deck_locked")));
  QVERIFY(reasons.contains(QStringLiteral("comparative_switch")));
  QVERIFY(reasons.contains(QStringLiteral("track_replaced")));
  QVERIFY(reasons.contains(QStringLiteral("end_of_folder")));
}

void TestPhase2Dsp::testHpfLpfClickFreeEnableDisableCrossfade() {
  // §5.9: Toggling HPF/LPF ON or OFF in live AudioPipeline must keep filter
  // state warmed while disabled and apply a smooth ~5ms section crossfade
  // without step discontinuities.
  constexpr double sr = 48000.0;
  constexpr double freqHz = 50.0; // Strong bass signal removed by 500 Hz HPF
  constexpr double amp = 0.8;
  const double maxExpectedStep =
      amp * 2.0 * std::numbers::pi * freqHz / sr + 0.01;

  HighLowPassFilter flt;
  flt.configure(sr, 500.0, 20000.0, HighLowPassFilter::FilterSlope::Slope24dB,
                HighLowPassFilter::FilterSlope::Slope12dB);
  flt.setHpfEnabled(false);
  flt.setLpfEnabled(false);

  double prevL = 0.0;
  double maxStepOn = 0.0;
  double maxStepOff = 0.0;
  size_t sampleIdx = 0;

  auto genSample = [&](size_t idx) {
    return amp * std::sin(2.0 * std::numbers::pi * freqHz *
                          static_cast<double>(idx) / sr);
  };

  // 1. Warm up signal with HPF OFF for 2048 samples (verify bit-exact
  // passthrough)
  for (int i = 0; i < 2048; ++i) {
    const double x = genSample(sampleIdx++);
    double l = x, r = x;
    flt.process(l, r);
    QCOMPARE(l, x);
    prevL = l;
  }

  // 2. Enable HPF at 500 Hz with 240-sample (5ms) smooth crossfade near peak
  // amplitude
  const auto coeffsOn = HighLowPassFilter::calculateCoefficients(
      sr, 500.0, 20000.0, HighLowPassFilter::FilterSlope::Slope24dB,
      HighLowPassFilter::FilterSlope::Slope12dB, true, false);
  flt.applyCoefficients(coeffsOn, 240);
  QVERIFY(flt.isSmoothingTransition());

  for (int i = 0; i < 480; ++i) {
    const double x = genSample(sampleIdx++);
    double l = x, r = x;
    flt.process(l, r);
    maxStepOn = std::max(maxStepOn, std::abs(l - prevL));
    prevL = l;
  }
  QVERIFY(!flt.isSmoothingTransition());
  QVERIFY2(maxStepOn < maxExpectedStep,
           qPrintable(QString("HPF ON step too large: %1 >= %2")
                          .arg(maxStepOn)
                          .arg(maxExpectedStep)));

  // 3. Disable HPF with 240-sample smooth crossfade
  const auto coeffsOff = HighLowPassFilter::calculateCoefficients(
      sr, 500.0, 20000.0, HighLowPassFilter::FilterSlope::Slope24dB,
      HighLowPassFilter::FilterSlope::Slope12dB, false, false);
  flt.applyCoefficients(coeffsOff, 240);
  QVERIFY(flt.isSmoothingTransition());

  for (int i = 0; i < 480; ++i) {
    const double x = genSample(sampleIdx++);
    double l = x, r = x;
    flt.process(l, r);
    maxStepOff = std::max(maxStepOff, std::abs(l - prevL));
    prevL = l;
  }
  QVERIFY(!flt.isSmoothingTransition());
  QVERIFY2(maxStepOff < maxExpectedStep,
           qPrintable(QString("HPF OFF step too large: %1 >= %2")
                          .arg(maxStepOff)
                          .arg(maxExpectedStep)));

  // 4. AudioPipeline end-to-end mailbox test with zero RT heap allocation
  AudioPipeline pipeline(sr);
  auto chain = std::make_unique<AudioChain>("FilterSmoothChain",
                                            ChainPurpose::MasteringChain);
  auto fltNode = std::make_shared<HighLowPassNode>("HPF / LPF");
  fltNode->configure(sr);
  fltNode->setBypassed(false);
  chain->appendNode(fltNode);
  chain->configure(sr);
  chain->setBypassed(false);
  pipeline.swapMasterChain(std::move(chain));

  constexpr size_t blockFrames = 256;
  std::vector<double> inL(blockFrames), inR(blockFrames);
  std::vector<float> outPcm(blockFrames * 2);

  // Prime pipeline with HPF OFF at 500 Hz
  pipeline.paramMailbox().setFilterParams(500.0f, 20000.0f, 1, 0, false, false,
                                          0.70710678f, 0.70710678f);
  for (int b = 0; b < 8; ++b) {
    for (size_t i = 0; i < blockFrames; ++i) {
      const double s = genSample(sampleIdx++);
      inL[i] = s;
      inR[i] = s;
    }
    pipeline.process(inL.data(), inR.data(), blockFrames, outPcm.data(),
                     SampleFormat::Float32, false);
  }
  double pipePrev = static_cast<double>(outPcm[(blockFrames - 1) * 2]);

  // Toggle HPF ON via mailbox and check zero RT allocations & zero step click
  pipeline.paramMailbox().setFilterParams(500.0f, 20000.0f, 1, 0, true, false,
                                          0.70710678f, 0.70710678f);
  double pipeMaxStep = 0.0;
  s_audioThreadAllocationCount.store(0, std::memory_order_relaxed);
  s_trackAllocations.store(true, std::memory_order_relaxed);
  {
    AudioThreadScopeGuard rtGuard;
    for (int b = 0; b < 4; ++b) {
      for (size_t i = 0; i < blockFrames; ++i) {
        const double s = genSample(sampleIdx++);
        inL[i] = s;
        inR[i] = s;
      }
      pipeline.process(inL.data(), inR.data(), blockFrames, outPcm.data(),
                       SampleFormat::Float32, false);
      for (size_t i = 0; i < blockFrames; ++i) {
        const double cur = static_cast<double>(outPcm[i * 2]);
        pipeMaxStep = std::max(pipeMaxStep, std::abs(cur - pipePrev));
        pipePrev = cur;
      }
    }
  }
  s_trackAllocations.store(false, std::memory_order_relaxed);
  QCOMPARE(s_audioThreadAllocationCount.load(), size_t(0));
  QVERIFY2(pipeMaxStep < maxExpectedStep,
           qPrintable(QString("Pipeline HPF ON step too large: %1 >= %2")
                          .arg(pipeMaxStep)
                          .arg(maxExpectedStep)));
}

void TestPhase2Dsp::testReverbGainStagingCombCountAndStability() {
  QCOMPARE(AlgorithmicReverb::NUM_COMBS, size_t(8));
  QCOMPARE(AlgorithmicReverb::NUM_ALLPASSES, size_t(4));

  const double sr = 48000.0;

  // 1. Parameter mapping sanity: roomSize [0, 1] maps smoothly to feedback
  // < 1.0,
  //    and getter preserves normalized roomSize without raw 1:1 comb blowout.
  {
    AlgorithmicReverb rev;
    rev.configure(sr, 0.8, 0.3, 0.6, 0.0, true, true);
    QCOMPARE(rev.roomSize(), 0.8);
    QVERIFY(rev.combFeedback() > 0.8 &&
            rev.combFeedback() < AlgorithmicReverb::MAX_COMB_FEEDBACK);
    QVERIFY(rev.combInputGain() > 0.0 && rev.combInputGain() < 1.0);
  }

  // 2. Sustained low-frequency (50 Hz) and mid-frequency (1 kHz) headroom
  // across roomSize & wet
  //    (Previously unnormalized 4-comb summing reached >10x gain and clipped
  //    into harsh noise.)
  struct GainCase {
    double roomSize;
    double wet;
    double freqHz;
    double amp;
  };
  const GainCase cases[] = {{0.7, 0.2, 50.0, 0.8},   {0.8, 0.6, 50.0, 0.8},
                            {0.8, 1.0, 50.0, 0.8},   {0.95, 1.0, 50.0, 0.8},
                            {0.8, 0.6, 1000.0, 0.8}, {0.95, 1.0, 1000.0, 0.8}};
  for (const auto &c : cases) {
    AlgorithmicReverb rev;
    rev.configure(sr, c.roomSize, 0.3, c.wet, 0.0, true, true);
    double maxAbs = 0.0;
    double sumSq = 0.0;
    const size_t total = 96000;
    const size_t measureStart = 48000;
    for (size_t n = 0; n < total; ++n) {
      const double x = c.amp * std::sin(2.0 * std::numbers::pi * c.freqHz *
                                        static_cast<double>(n) / sr);
      double l = x, r = x;
      rev.process(l, r);
      if (n >= measureStart) {
        maxAbs = std::max(maxAbs, std::max(std::abs(l), std::abs(r)));
        sumSq += 0.5 * (l * l + r * r);
      }
    }
    const double rms =
        std::sqrt(sumSq / static_cast<double>(total - measureStart));
    QVERIFY2(maxAbs < 1.0,
             qPrintable(
                 QString("Reverb clipped at room=%1 wet=%2 freq=%3Hz maxAbs=%4")
                     .arg(c.roomSize)
                     .arg(c.wet)
                     .arg(c.freqHz)
                     .arg(maxAbs)));
    QVERIFY2(
        rms > 0.02,
        qPrintable(
            QString(
                "Reverb wet output too weak at room=%1 wet=%2 freq=%3Hz rms=%4")
                .arg(c.roomSize)
                .arg(c.wet)
                .arg(c.freqHz)
                .arg(rms)));
  }

  // 3. Multi-tone / broadband wet RMS stays balanced (~0.2..0.7x) at 100% wet
  // across room sizes
  for (double room : {0.2, 0.7, 0.95}) {
    AlgorithmicReverb rev;
    rev.configure(sr, room, 0.3, 1.0, 0.0, true, true);
    double inSq = 0.0, outSq = 0.0;
    uint32_t rng = 0x12345678u;
    const size_t total = 96000;
    const size_t measureStart = 48000;
    for (size_t n = 0; n < total; ++n) {
      rng = rng * 1664525u + 1013904223u;
      const double white = (static_cast<double>(rng) / 2147483648.0) - 1.0;
      const double x = 0.5 * white;
      double l = x, r = x;
      rev.process(l, r);
      if (n >= measureStart) {
        inSq += x * x;
        outSq += 0.5 * (l * l + r * r);
      }
    }
    const double ratio = std::sqrt(outSq / inSq);
    QVERIFY2(ratio > 0.15 && ratio < 0.85,
             qPrintable(QString("Broadband wet/dry RMS ratio out of balanced "
                                "range at room=%1: %2")
                            .arg(room)
                            .arg(ratio)));
  }

  // 4. Damping = 1.0 does not mute comb feedback (MAX_COMB_DAMP < 1.0)
  {
    AlgorithmicReverb rev;
    rev.configure(sr, 0.8, 1.0, 1.0, 0.0, true, true);
    double l = 1.0, r = 1.0;
    rev.process(l, r);
    double tailEnergy = 0.0;
    for (int i = 0; i < 4096; ++i) {
      double tl = 0.0, tr = 0.0;
      rev.process(tl, tr);
      tailEnergy += tl * tl + tr * tr;
    }
    QVERIFY2(tailEnergy > 1e-4, "Damping=1.0 muted reverb tail completely");
  }
}

void TestPhase2Dsp::testDspInputOutputTrimsPresetRulesAndPreamp() {
  using namespace desk32::engine::pipeline;
  using namespace desk32::engine::preset;
  using namespace desk32::engine::dsp;
  using desk32::core::EngineController;
  using desk32::core::log::LogFilter;
  using desk32::core::log::LogService;

  // 1. AudioPipeline unit verification: all 7 DSP trims survive bypass, Direct
  // Mode bypasses trims, 0 RT allocations
  {
    AudioPipeline pipe(48000.0);
    auto chain =
        std::make_unique<AudioChain>("TrimChain", ChainPurpose::MasteringChain);
    auto peq = std::make_shared<ParametricEqNode>("Master PEQ");
    auto geq = std::make_shared<GraphicEqNode>("Graphic EQ");
    auto flt = std::make_shared<HighLowPassNode>("HPF / LPF");
    auto comp = std::make_shared<CompressorNode>("Compressor");
    auto rev = std::make_shared<ReverbNode>("Reverb");
    auto lim = std::make_shared<PeakLimiterNode>("Peak Limiter");
    peq->setBypassed(true);
    geq->setBypassed(true);
    flt->setBypassed(true);
    comp->setBypassed(true);
    rev->setBypassed(true);
    lim->setBypassed(true);
    chain->appendNode(peq);
    chain->appendNode(geq);
    chain->appendNode(flt);
    chain->appendNode(comp);
    chain->appendNode(rev);
    chain->appendNode(lim);
    pipe.swapMasterChain(std::move(chain));

    constexpr size_t N = 256;
    std::vector<double> L(N, 0.25), R(N, 0.25);
    std::vector<float> out(N * 2, 0.0f);

    // Prime at 0 dB -> Level 1 bit-identical
    pipe.process(L.data(), R.data(), N, out.data(),
                 desk32::engine::format::SampleFormat::Float32, false);
    QVERIFY(pipe.currentTelemetry().level1SamplesIdentical);
    QCOMPARE(pipe.currentTelemetry().level1MaxAbsDiff, 0.0);

    // Check every single one of the 7 DSPs (in and out) while all DSPs are
    // bypassed AND global chain is bypassed
    const size_t targets[7] = {static_cast<size_t>(DspTrimTarget::Peq),
                               static_cast<size_t>(DspTrimTarget::Geq),
                               static_cast<size_t>(DspTrimTarget::Hpf),
                               static_cast<size_t>(DspTrimTarget::Lpf),
                               static_cast<size_t>(DspTrimTarget::Compressor),
                               static_cast<size_t>(DspTrimTarget::Limiter),
                               static_cast<size_t>(DspTrimTarget::Reverb)};
    for (size_t idx : targets) {
      for (bool isOut : {false, true}) {
        pipe.paramMailbox().setDspTrim(idx, isOut, 6.0206f); // ~2.0x linear
        s_audioThreadAllocationCount.store(0, std::memory_order_relaxed);
        s_trackAllocations.store(true, std::memory_order_relaxed);
        {
          AudioThreadScopeGuard rtGuard;
          for (int b = 0; b < 16; ++b) {
            std::fill(L.begin(), L.end(), 0.25);
            std::fill(R.begin(), R.end(), 0.25);
            pipe.process(L.data(), R.data(), N, out.data(),
                         desk32::engine::format::SampleFormat::Float32, false);
          }
        }
        s_trackAllocations.store(false, std::memory_order_relaxed);
        QCOMPARE(s_audioThreadAllocationCount.load(), size_t(0));
        QVERIFY2(std::abs(static_cast<double>(out[(N - 1) * 2]) - 0.5) < 1e-3,
                 qPrintable(QString("Trim failed for target %1 out=%2: got %3")
                                .arg(idx)
                                .arg(isOut)
                                .arg(out[(N - 1) * 2])));

        // Global chain bypass still keeps trims active
        pipe.paramQueue().push({DspCommandType::SetGlobalBypass, 0, 0, 1});
        std::fill(L.begin(), L.end(), 0.25);
        std::fill(R.begin(), R.end(), 0.25);
        pipe.process(L.data(), R.data(), N, out.data(),
                     desk32::engine::format::SampleFormat::Float32, false);
        QVERIFY(std::abs(static_cast<double>(out[(N - 1) * 2]) - 0.5) < 1e-3);

        // Direct mode bypasses trims
        std::fill(L.begin(), L.end(), 0.25);
        std::fill(R.begin(), R.end(), 0.25);
        pipe.process(L.data(), R.data(), N, out.data(),
                     desk32::engine::format::SampleFormat::Float32, false,
                     nullptr, nullptr, 1.0, true);
        QVERIFY(std::abs(static_cast<double>(out[(N - 1) * 2]) - 0.25) < 1e-6);

        pipe.paramQueue().push({DspCommandType::SetGlobalBypass, 0, 0, 0});
        pipe.paramMailbox().setDspTrim(idx, isOut, 0.0f);
        for (int b = 0; b < 20; ++b) {
          std::fill(L.begin(), L.end(), 0.25);
          std::fill(R.begin(), R.end(), 0.25);
          pipe.process(L.data(), R.data(), N, out.data(),
                       desk32::engine::format::SampleFormat::Float32, false);
        }
      }
    }
    // Returning all trims to 0 dB restores exact Level 1 identity
    std::fill(L.begin(), L.end(), 0.25);
    std::fill(R.begin(), R.end(), 0.25);
    pipe.process(L.data(), R.data(), N, out.data(),
                 desk32::engine::format::SampleFormat::Float32, false);
    QVERIFY(pipe.currentTelemetry().level1SamplesIdentical);
    QCOMPARE(pipe.currentTelemetry().level1MaxAbsDiff, 0.0);
  }

  // 2. EngineController integration: bitPerfectStatus, clamping, preset
  // exclusion, <16 OFF, >16 & unsupported filter logs, Preamp
  EngineController &ctrl = EngineController::instance();
  QVERIFY(ctrl.initialize(true));
  PresetManager::instance().setCustomRootPath(m_tempDir->path() +
                                              "/p2_trim_presets");

  // Verify defaults & clamping (±12 dB)
  const QStringList allSeven = {"PEQ",        "GEQ",     "HPF",   "LPF",
                                "Compressor", "Limiter", "Reverb"};
  for (const QString &n : allSeven) {
    QCOMPARE(ctrl.getDspTrim(n, false), 0.0);
    QCOMPARE(ctrl.getDspTrim(n, true), 0.0);
  }
  ctrl.setDspTrim("HPF", false, 24.0);
  QCOMPARE(ctrl.getDspTrim("HPF", false), 12.0);
  ctrl.setDspTrim("HPF", false, -18.5);
  QCOMPARE(ctrl.getDspTrim("HPF", false), -12.0);
  ctrl.resetAllDspTrims();
  QCOMPARE(ctrl.getDspTrim("HPF", false), 0.0);

  // Bit-Perfect check: trim != 0 while all DSPs bypassed -> NotVerified (1);
  // reset -> Verified (2)
  const QString wavPath = m_tempDir->path() + "/trim_bp_test.wav";
  {
    QFile wf(wavPath);
    QVERIFY(wf.open(QIODevice::WriteOnly));
    const uint32_t sr = 48000, nFrames = 4800, dataSz = nFrames * 2 * 2,
                   riffSz = 36 + dataSz, br = sr * 4;
    const uint16_t pcmFmt = 1, ch = 2, bits = 16, ba = 4;
    const uint32_t fmtSz = 16;
    QByteArray hdr;
    hdr.append("RIFF", 4);
    hdr.append(reinterpret_cast<const char *>(&riffSz), 4);
    hdr.append("WAVE", 4);
    hdr.append("fmt ", 4);
    hdr.append(reinterpret_cast<const char *>(&fmtSz), 4);
    hdr.append(reinterpret_cast<const char *>(&pcmFmt), 2);
    hdr.append(reinterpret_cast<const char *>(&ch), 2);
    hdr.append(reinterpret_cast<const char *>(&sr), 4);
    hdr.append(reinterpret_cast<const char *>(&br), 4);
    hdr.append(reinterpret_cast<const char *>(&ba), 2);
    hdr.append(reinterpret_cast<const char *>(&bits), 2);
    hdr.append("data", 4);
    hdr.append(reinterpret_cast<const char *>(&dataSz), 4);
    wf.write(hdr);
    wf.write(QByteArray(static_cast<int>(dataSz), '\0'));
    wf.close();
  }
  QVERIFY(ctrl.loadTrack("A", wavPath));
  ctrl.setOutputRoutingMode(1);
  ctrl.setOutputPcmFormat("F32LE");
  ctrl.setLiveDitherEnabled(false);
  ctrl.setDirectModeEnabled(false);
  ctrl.setGlobalDspBypass(false);
  std::vector<float> dummy(512 * 2, 0.0f);
  ctrl.renderOfflineBlock(dummy.data(), 512);
  QCOMPARE(ctrl.bitPerfectStatus(), 2);

  ctrl.setDspTrim("LPF", true, 0.1);
  ctrl.renderOfflineBlock(dummy.data(), 512);
  QCOMPARE(ctrl.bitPerfectStatus(), 1);
  QCOMPARE(ctrl.getBitPerfectVerificationReport()["dspTrimsZero"].toBool(),
           false);

  ctrl.resetAllDspTrims();
  ctrl.renderOfflineBlock(dummy.data(), 512);
  QCOMPARE(ctrl.bitPerfectStatus(), 2);
  QCOMPARE(ctrl.getBitPerfectVerificationReport()["dspTrimsZero"].toBool(),
           true);
  ctrl.setOutputRoutingMode(0);

  // Preset rules:
  // (a) Trims excluded from presets & untouched on preset switch
  ctrl.setDspTrim("PEQ", false, 3.5);
  ctrl.setDspTrim("Compressor", true, -2.5);
  const QVariantMap capturedMaster =
      ctrl.captureCurrentStateAsPreset(PresetCategories::MasterDsp);
  QVERIFY(!capturedMaster.contains("trims"));
  QVERIFY(!capturedMaster.value("peq").toMap().contains("input_trim_db"));
  QVERIFY(ctrl.savePreset(PresetCategories::MasterDsp, "MasterNoTrimCheck",
                          capturedMaster));
  ctrl.setDspTrim("PEQ", false, -1.5);
  QVERIFY(ctrl.applyPresetProfile(PresetCategories::MasterDsp,
                                  "MasterNoTrimCheck"));
  QCOMPARE(ctrl.getDspTrim("PEQ", false), -1.5);
  QCOMPARE(ctrl.getDspTrim("Compressor", true), -2.5);
  ctrl.resetAllDspTrims();

  // (b) 12-band preset loaded into 16-band PEQ leaves bands 12..15 OFF (flat),
  // applies Preamp only at PEQ input
  QVariantMap twelvePreset;
  QVariantList twelveBands;
  for (int i = 0; i < 12; ++i) {
    QVariantMap b;
    b["index"] = i;
    b["frequency"] = 50.0 + i * 1000.0;
    b["gain_db"] = (i == 3) ? 4.0 : 0.0;
    b["q"] = 1.0;
    b["type"] = 0;
    b["enabled"] = true;
    twelveBands.append(b);
  }
  twelvePreset["left_bands"] = twelveBands;
  twelvePreset["right_bands"] = twelveBands;
  twelvePreset["preamp_db"] = -4.5;
  QVERIFY(ctrl.savePreset(PresetCategories::ListeningCorrection,
                          "Legacy12WithPreamp", twelvePreset));
  QVERIFY(ctrl.applyPresetProfile(PresetCategories::ListeningCorrection,
                                  "Legacy12WithPreamp"));
  QCOMPARE(ctrl.getListeningBand(false, 3)["gainDb"].toDouble(), 4.0);
  QCOMPARE(ctrl.getListeningBand(false, 3)["enabled"].toBool(), true);
  for (int i = 12; i < 16; ++i) {
    QCOMPARE(ctrl.getListeningBand(false, i)["enabled"].toBool(), false);
    QCOMPARE(ctrl.getListeningBand(false, i)["gainDb"].toDouble(), 0.0);
    QCOMPARE(ctrl.getListeningBand(true, i)["enabled"].toBool(), false);
  }
  QCOMPARE(ctrl.getListeningPreampDb(), -4.5);
  QCOMPARE(ctrl.getDspTrim("PEQ", false), 0.0);
  QCOMPARE(ctrl.masterGain(), 1.0);
  // Switching back to Monitor Flat removes Preamp
  ctrl.setListeningPreset("Monitor Flat");
  QCOMPARE(ctrl.getListeningPreampDb(), 0.0);

  // Master PEQ 12-band + Preamp check
  QVariantMap master12;
  QVariantMap masterPeqObj;
  masterPeqObj["enabled"] = true;
  masterPeqObj["preamp_db"] = -6.0;
  masterPeqObj["left_bands"] = twelveBands;
  masterPeqObj["right_bands"] = twelveBands;
  master12["peq"] = masterPeqObj;
  QVERIFY(ctrl.savePreset(PresetCategories::MasterDsp, "Master12WithPreamp",
                          master12));
  QVERIFY(ctrl.applyPresetProfile(PresetCategories::MasterDsp,
                                  "Master12WithPreamp"));
  QCOMPARE(ctrl.getPeqPreampDb(), -6.0);
  QCOMPARE(ctrl.getPeqBand(false, 12)["enabled"].toBool(), false);
  QCOMPARE(ctrl.getPeqBand(false, 15)["enabled"].toBool(), false);
  QCOMPARE(ctrl.getDspTrim("PEQ", false), 0.0);
  QCOMPARE(ctrl.masterGain(), 1.0);
  ctrl.applyPeqPreset("Monitor Flat");
  QCOMPARE(ctrl.getPeqPreampDb(), 0.0);

  // (c) >16-band truncation & unsupported filter type rejection + report +
  // LogService
  QVariantMap badPreset;
  QVariantList badBands;
  for (int i = 0; i < 19; ++i) {
    QVariantMap b;
    b["index"] = i;
    b["frequency"] = 80.0 + i * 500.0;
    b["gain_db"] = 2.0;
    b["q"] = 1.0;
    if (i == 2)
      b["type"] = QStringLiteral("AllPass");
    else if (i == 4)
      b["type"] = 99;
    else
      b["type"] = 0;
    b["enabled"] = true;
    badBands.append(b);
  }
  badPreset["left_bands"] = badBands;
  badPreset["right_bands"] = badBands;
  QVERIFY(ctrl.savePreset(PresetCategories::ListeningCorrection,
                          "ExcessAndBadFilter", badPreset));
  QVERIFY(ctrl.applyPresetProfile(PresetCategories::ListeningCorrection,
                                  "ExcessAndBadFilter"));
  const QVariantMap rep = ctrl.getLastPresetImportReport();
  QVERIFY(rep["truncated"].toBool());
  QCOMPARE(rep["truncatedCountLeft"].toInt(), 3);
  QCOMPARE(rep["unsupportedFilterCount"].toInt(), 4); // L[2], L[4], R[2], R[4]
  QCOMPARE(ctrl.getListeningBand(false, 2)["enabled"].toBool(), false);
  QCOMPARE(ctrl.getListeningBand(false, 4)["enabled"].toBool(), false);

  LogService::instance().flush();
  LogFilter f;
  f.category = "PresetManagement";
  f.target = "ExcessAndBadFilter";
  const auto logs = LogService::instance().queryLogs(f);
  QVERIFY(!logs.empty());
  QCOMPARE(QString::fromStdString(logs.front().operation),
           QStringLiteral("ImportListeningPeq"));
  QVERIFY(logs.front().detailsJson.find("truncated_left") !=
          std::string::npos);
  QVERIFY(logs.front().detailsJson.find("unsupported_filters") !=
          std::string::npos);

  ctrl.setListeningPreset("Monitor Flat");
  ctrl.setDspNodeBypass("PEQ", true);
  ctrl.resetAllDspTrims();
  ctrl.stop("A");
}

QTEST_GUILESS_MAIN(TestPhase2Dsp)
#include "test_phase2_dsp.moc"
