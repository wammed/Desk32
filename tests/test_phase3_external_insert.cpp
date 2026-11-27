#include <QObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "core/EngineController.hpp"
#include "core/log/LogService.hpp"
#include "engine/audio/WaveformExtractor.hpp"
#include "engine/insert/DelayCompensationBuffer.hpp"
#include "engine/insert/ExternalInsertBridge.hpp"
#include "engine/insert/ExternalInsertNode.hpp"
#include "engine/insert/PipeWireInsertTransport.hpp"
#include "engine/preset/PresetManager.hpp"
#include <QDir>
#include <QFile>

using namespace desk32::engine::insert;
using namespace desk32::engine::preset;
using namespace desk32::engine::dsp;
using namespace desk32::core;

// Real-time heap allocation tracking for Audio Thread safety verification
static std::atomic<bool> s_trackAllocations{false};
static std::atomic<size_t> s_allocationCount{0};
static thread_local bool t_isAudioThread{false};

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void *operator new(size_t size) {
  if (s_trackAllocations.load(std::memory_order_relaxed) && t_isAudioThread) {
    s_allocationCount.fetch_add(1, std::memory_order_relaxed);
  }
  return std::malloc(size);
}

void *operator new(size_t size, const std::nothrow_t &) noexcept {
  if (s_trackAllocations.load(std::memory_order_relaxed) && t_isAudioThread) {
    s_allocationCount.fetch_add(1, std::memory_order_relaxed);
  }
  return std::malloc(size);
}

void *operator new[](size_t size) {
  if (s_trackAllocations.load(std::memory_order_relaxed) && t_isAudioThread) {
    s_allocationCount.fetch_add(1, std::memory_order_relaxed);
  }
  return std::malloc(size);
}

void *operator new[](size_t size, const std::nothrow_t &) noexcept {
  if (s_trackAllocations.load(std::memory_order_relaxed) && t_isAudioThread) {
    s_allocationCount.fetch_add(1, std::memory_order_relaxed);
  }
  return std::malloc(size);
}

void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }
void operator delete[](void *p, size_t) noexcept { std::free(p); }

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

class TestPhase3ExternalInsert : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void cleanupTestCase();

  // 1. External Insert Node audio processing and bypass transitions
  void testExternalInsertNodeAudioProcessing();

  // 2. Delay compensation buffer accuracy and reset
  void testDelayCompensationAccuracy();

  // 3. Feedback loop detection & automatic bypass
  void testFeedbackLoopDetectionAndAutoBypass();

  // 4. Sample rate and buffer size mismatch handling
  void testRateAndBufferMismatchRejection();

  // 5. Host disconnect and recovery lifecycle transitions
  void testDisconnectRecoverySafety();

  // 6. Strict separation of reported vs measured latency
  void testReportedVsMeasuredLatencySeparation();

  // 7. Real-time safety: Strictly zero dynamic memory allocation in process
  // loop
  void testZeroDynamicAllocationInInsertNode();

  // 8. PresetManager: XDG Base Directory, category isolation, factory presets,
  // atomic save/load
  void testPresetManagerXdgPathsAndSerialization();

  // 9. EngineController: Integration of External Insert and Preset APIs
  void testEngineControllerIntegration();

  // 10. EngineController: DSP Parameter Preview & Master Control APIs
  // (Section 5.9 & 10.2.3)
  void testEngineControllerDspPreviewAndMasterControl();

  // 11. P1-1: Global DSP Bypass Queue Consistency, Atomic Push, and Rollback
  void testGlobalDspBypassQueueConsistencyAndRollback();

  // 12. P1-2: Individual DSP Node Bypass Queue Push Failure, Consistency, and
  // Parameter Preservation
  void testDspNodeBypassQueueFailureAndConsistency();

  // 13. Phase 3 Gap Fixes: pw-dump discovery, verified recovery, chain
  // position, disconnect policies, auto impulse, metadata & session state
  void testPhase3GapFixesInsertPoliciesDiscoveryAndSessionState();

  // 14. PipeWireInsertTransport loopback/disconnect/mismatch & 2-step latency
  // calibration stability judgment
  void testPipeWireInsertTransportAndTwoStepLatencyCalibration();

  // 15. Live connection enforcement, pipeline bypass sync, isolated probe
  // calibration, reconnect & pw-dump filtering
  void testLiveTransportEnforcementCalibrationAndReconnect();

  // 16. Gate G4: Master Balance / L-R Mute Direct Mode safety / Monitor -20 dB
  // Dim
  void testMasterBalanceMuteLRDimAndDirectModeSafety();

  // 17. Gate G4: WaveformExtractor async extraction + disk cache & RT-safe FFT
  // SPSC ring
  void testWaveformExtractorCacheAndSpectrumRing();

  // 18. Gate G4: External Insert Send/Return peak telemetry & immediate Active
  // state poll
  void testExternalInsertSendReturnTelemetryAndStatusPolling();

private:
  std::unique_ptr<QTemporaryDir> m_tempDir;
};

void TestPhase3ExternalInsert::initTestCase() {
  m_tempDir = std::make_unique<QTemporaryDir>();
  QVERIFY(m_tempDir->isValid());
  QString dbPath = m_tempDir->path() + "/test_phase3_log.db";
  desk32::core::log::LogService::instance().start(dbPath);
}

void TestPhase3ExternalInsert::cleanupTestCase() {
  desk32::core::log::LogService::instance().stop();
}

void TestPhase3ExternalInsert::testExternalInsertNodeAudioProcessing() {
  ExternalInsertNode node;
  node.configure(48000.0);

  // Initial default state: bypassed
  QVERIFY(node.isBypassed());

  std::vector<double> left(256, 0.75);
  std::vector<double> right(256, -0.75);

  // 1. When bypassed, audio must pass through dry (untouched)
  node.process(left.data(), right.data(), 256);
  QCOMPARE(left[0], 0.75);
  QCOMPARE(right[0], -0.75);

  // 2. Configure external insert with valid parameters
  ExternalPortInfo config{
      "CarlaHost",   "carla:in_l", "carla:in_r", "carla:out_l",
      "carla:out_r", 48000.0,      256,          128};
  QVERIFY(node.bridge().configure(config, 48000.0, 256));
  node.setBypassed(false);
  QVERIFY(!node.isBypassed());

  // Attach mock processor simulating external plugin inversion
  node.bridge().setMockProcessor([](double *l, double *r, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
      l[i] = -l[i];
      r[i] = -r[i];
    }
  });

  // Process audio: verify state becomes Active and signal is processed (wet)
  node.process(left.data(), right.data(), 256);
  QCOMPARE(node.bridge().state(), InsertState::Active);
  QCOMPARE(left[0], -0.75);
  QCOMPARE(right[0], 0.75);

  // 3. Re-engage bypass: audio must revert immediately to dry pass-through
  node.setBypassed(true);
  QVERIFY(node.isBypassed());
  node.process(left.data(), right.data(), 256);
  QCOMPARE(left[0], -0.75); // Unchanged from previous block
  QCOMPARE(right[0], 0.75);
}

void TestPhase3ExternalInsert::testDelayCompensationAccuracy() {
  DelayCompensationBuffer delayBuffer;
  QCOMPARE(delayBuffer.delayFrames(), size_t(0));

  // Zero delay: pass-through
  std::vector<double> left(256, 0.0), right(256, 0.0);
  left[0] = 1.0;
  right[0] = 0.5;
  delayBuffer.process(left.data(), right.data(), 256);
  QCOMPARE(left[0], 1.0);
  QCOMPARE(right[0], 0.5);

  // Set delay to exactly 64 frames
  delayBuffer.reset();
  delayBuffer.setDelayFrames(64);
  QCOMPARE(delayBuffer.delayFrames(), size_t(64));

  std::fill(left.begin(), left.end(), 0.0);
  std::fill(right.begin(), right.end(), 0.0);
  left[0] = 1.0; // Impulse at frame 0
  right[0] = 1.0;

  delayBuffer.process(left.data(), right.data(), 256);
  // At output, frame 0 should be 0.0, and impulse should emerge at index 64
  QCOMPARE(left[0], 0.0);
  QCOMPARE(right[0], 0.0);
  QCOMPARE(left[64], 1.0);
  QCOMPARE(right[64], 1.0);
  QCOMPARE(left[65], 0.0);
  QCOMPARE(right[65], 0.0);

  // Test delay across block boundaries (e.g. 300 frames delay with 256 frame
  // blocks)
  delayBuffer.reset();
  delayBuffer.setDelayFrames(300);
  std::fill(left.begin(), left.end(), 0.0);
  std::fill(right.begin(), right.end(), 0.0);
  left[0] = 2.5;

  // Block 1 (frames 0..255)
  delayBuffer.process(left.data(), right.data(), 256);
  for (size_t i = 0; i < 256; ++i) {
    QCOMPARE(left[i], 0.0);
  }

  // Block 2 (frames 256..511): impulse should appear at index 300 - 256 = 44
  std::fill(left.begin(), left.end(), 0.0);
  delayBuffer.process(left.data(), right.data(), 256);
  QCOMPARE(left[44], 2.5);
  QCOMPARE(left[43], 0.0);
  QCOMPARE(left[45], 0.0);

  // Test buffer reset
  delayBuffer.reset();
  std::fill(left.begin(), left.end(), 0.0);
  delayBuffer.process(left.data(), right.data(), 256);
  QCOMPARE(left[44], 0.0);
}

void TestPhase3ExternalInsert::testFeedbackLoopDetectionAndAutoBypass() {
  ExternalInsertBridge bridge;

  // Direct feedback: sendPortL == returnPortL
  ExternalPortInfo directFeedback{"CarlaHost",
                                  "desk32:send_l",
                                  "desk32:send_r",
                                  "desk32:send_l",
                                  "desk32:return_r",
                                  48000.0,
                                  256,
                                  0};

  bool ok = bridge.configure(directFeedback, 48000.0, 256);
  QVERIFY(!ok);
  QCOMPARE(bridge.state(), InsertState::Error);
  QVERIFY(bridge.isBypassed());
  QVERIFY(bridge.lastError().find("Feedback loop detected") !=
          std::string::npos);

  // Direct feedback on Right channel
  ExternalPortInfo rightFeedback{"CarlaHost",
                                 "desk32:send_l",
                                 "desk32:send_r",
                                 "desk32:return_l",
                                 "desk32:send_r",
                                 48000.0,
                                 256,
                                 0};
  ExternalInsertBridge rightBridge;
  QVERIFY(!rightBridge.configure(rightFeedback, 48000.0, 256));
  QCOMPARE(rightBridge.state(), InsertState::Error);
  QVERIFY(rightBridge.isBypassed());
}

void TestPhase3ExternalInsert::testRateAndBufferMismatchRejection() {
  ExternalInsertBridge bridge;

  // Sample rate mismatch: engine at 48000 Hz, external host at 96000 Hz
  ExternalPortInfo rateMismatchConfig{
      "CarlaHost",   "carla:in_l", "carla:in_r", "carla:out_l",
      "carla:out_r", 96000.0,      256,          64};
  bool okRate = bridge.configure(rateMismatchConfig, 48000.0, 256);
  QVERIFY(!okRate);
  QCOMPARE(bridge.state(), InsertState::RateMismatch);
  QVERIFY(bridge.isBypassed());
  QVERIFY(bridge.lastError().find("Sample rate mismatch") != std::string::npos);

  // Buffer size mismatch: engine at 256, external host at 512
  ExternalPortInfo bufferMismatchConfig{
      "CarlaHost",   "carla:in_l", "carla:in_r", "carla:out_l",
      "carla:out_r", 48000.0,      512,          64};
  ExternalInsertBridge bufBridge;
  bool okBuf = bufBridge.configure(bufferMismatchConfig, 48000.0, 256);
  QVERIFY(!okBuf);
  QCOMPARE(bufBridge.state(), InsertState::BufferMismatch);
  QVERIFY(bufBridge.isBypassed());
  QVERIFY(bufBridge.lastError().find("Buffer size mismatch") !=
          std::string::npos);
}

void TestPhase3ExternalInsert::testDisconnectRecoverySafety() {
  ExternalInsertBridge bridge;
  ExternalPortInfo config{
      "CarlaHost",   "carla:in_l", "carla:in_r", "carla:out_l",
      "carla:out_r", 48000.0,      256,          64};
  QVERIFY(bridge.configure(config, 48000.0, 256));
  QCOMPARE(bridge.state(), InsertState::Connected);
  QVERIFY(!bridge.isBypassed());

  std::vector<double> left(256, 0.42), right(256, 0.84);
  bridge.process(left.data(), right.data(), 256);
  QCOMPARE(bridge.state(), InsertState::Active);

  // Simulate external host crash / disconnect event
  bridge.handleDisconnect();
  QCOMPARE(bridge.state(), InsertState::Disconnected);
  QVERIFY(bridge.isBypassed());
  QVERIFY(bridge.lastError().find("disconnected") != std::string::npos);

  // While disconnected, audio must pass through smoothly without failure
  bridge.process(left.data(), right.data(), 256);
  QCOMPARE(left[0], 0.42);
  QCOMPARE(right[0], 0.84);

  // Simulate recovery event
  bridge.handleRecovery();
  QCOMPARE(bridge.state(), InsertState::Connected);
  QVERIFY(!bridge.isBypassed());
  QVERIFY(bridge.lastError().empty());

  // XRUN tracking test
  QCOMPARE(bridge.xrunCount(), 0u);
  bridge.recordXRun();
  QCOMPARE(bridge.xrunCount(), 1u);
  QCOMPARE(bridge.state(), InsertState::XRun);
  bridge.resetXRunCount();
  QCOMPARE(bridge.xrunCount(), 0u);
}

void TestPhase3ExternalInsert::testReportedVsMeasuredLatencySeparation() {
  ExternalInsertBridge bridge;
  const uint32_t reportedLatency = 240; // Host reports 240 frames
  ExternalPortInfo config{"LSP_Limiter", "lsp:in_l",     "lsp:in_r",
                          "lsp:out_l",   "lsp:out_r",    48000.0,
                          256,           reportedLatency};
  QVERIFY(bridge.configure(config, 48000.0, 256));

  // Initially, reported latency is known, measured latency is unmeasured (-1)
  QCOMPARE(bridge.reportedLatencyFrames(), reportedLatency);
  QCOMPARE(bridge.measuredLatencyFrames(), -1);
  QCOMPARE(bridge.measuredLatencyMs(), -1.0);

  // Trigger impulse measurement
  bridge.triggerLatencyMeasurement();
  std::vector<double> left(256, 0.0), right(256, 0.0);
  bridge.process(left.data(), right.data(), 256);

  // Simulate impulse return after 240 + 16 = 256 frames roundtrip
  uint64_t returnFrame = 64 + 256;
  bridge.recordImpulseReturn(returnFrame);

  // Verify reported vs measured separation
  QCOMPARE(bridge.reportedLatencyFrames(), 240u);
  QCOMPARE(bridge.measuredLatencyFrames(), 256);
  double expectedMs = (256.0 / 48000.0) * 1000.0;
  QVERIFY(std::abs(bridge.measuredLatencyMs() - expectedMs) < 1e-6);

  // Node reports combined latency (reported + delay compensation)
  ExternalInsertNode node;
  node.configure(48000.0);
  node.bridge().configure(config, 48000.0, 256);
  node.delayBuffer().setDelayFrames(64);
  QCOMPARE(node.latencyFrames(), int64_t(reportedLatency + 64));
}

void TestPhase3ExternalInsert::testZeroDynamicAllocationInInsertNode() {
  ExternalInsertNode node;
  node.configure(48000.0);
  ExternalPortInfo config{
      "CarlaHost",   "carla:in_l", "carla:in_r", "carla:out_l",
      "carla:out_r", 48000.0,      256,          128};
  node.bridge().configure(config, 48000.0, 256);
  node.delayBuffer().setDelayFrames(128);
  node.setBypassed(false);

  std::vector<double> left(256, 0.5);
  std::vector<double> right(256, -0.5);

  // Prime the processor
  node.process(left.data(), right.data(), 256);

  // Enable heap allocation tracking on audio thread
  t_isAudioThread = true;
  s_allocationCount.store(0, std::memory_order_relaxed);
  s_trackAllocations.store(true, std::memory_order_release);

  // Execute 1,000 real-time audio blocks
  for (size_t block = 0; block < 1000; ++block) {
    node.process(left.data(), right.data(), 256);
  }

  s_trackAllocations.store(false, std::memory_order_release);
  t_isAudioThread = false;
  size_t allocations = s_allocationCount.load(std::memory_order_relaxed);

  QCOMPARE(allocations, size_t(0));
}

void TestPhase3ExternalInsert::testPresetManagerXdgPathsAndSerialization() {
  QTemporaryDir tempDir;
  QVERIFY(tempDir.isValid());

  PresetManager &pm = PresetManager::instance();
  pm.setCustomRootPath(tempDir.path() + "/presets");

  // 1. Factory Presets Verification
  QStringList lcPresets = pm.listPresets(PresetCategories::ListeningCorrection);
  QVERIFY(lcPresets.contains("Monitor Flat"));
  QVERIFY(lcPresets.contains("Harman Target"));
  QVERIFY(pm.isFactoryPreset(PresetCategories::ListeningCorrection,
                             "Monitor Flat"));

  QJsonObject flatJson =
      pm.loadPreset(PresetCategories::ListeningCorrection, "Monitor Flat");
  QVERIFY(!flatJson.isEmpty());
  QCOMPARE(flatJson["name"].toString(), QString("Monitor Flat"));
  QCOMPARE(flatJson["is_factory"].toBool(), true);

  // Attempting to delete or overwrite factory preset must fail
  QVERIFY(
      !pm.deletePreset(PresetCategories::ListeningCorrection, "Monitor Flat"));
  QJsonObject dummy;
  QVERIFY(!pm.savePreset(PresetCategories::ListeningCorrection, "Monitor Flat",
                         dummy, true));

  // 2. Custom User Preset Save & Load (External Insert Category)
  QJsonObject customInsert;
  customInsert["client_name"] = "CustomCarla";
  customInsert["send_port_l"] = "carla:in_1";
  customInsert["send_port_r"] = "carla:in_2";
  customInsert["return_port_l"] = "carla:out_1";
  customInsert["return_port_r"] = "carla:out_2";
  customInsert["sample_rate"] = 48000.0;
  customInsert["buffer_size"] = 256;
  customInsert["reported_latency_frames"] = 96;

  QVERIFY(pm.savePreset(PresetCategories::ExternalInsert, "My Studio Carla",
                        customInsert));
  QVERIFY(pm.listPresets(PresetCategories::ExternalInsert)
              .contains("My Studio Carla"));

  QJsonObject loaded =
      pm.loadPreset(PresetCategories::ExternalInsert, "My Studio Carla");
  QCOMPARE(loaded["client_name"].toString(), QString("CustomCarla"));
  QCOMPARE(loaded["reported_latency_frames"].toInt(), 96);
  QCOMPARE(loaded["is_factory"].toBool(), false);

  // 3. Category Isolation: Trying to load an external insert preset under
  // listening correction must fail
  QJsonObject isolated =
      pm.loadPreset(PresetCategories::ListeningCorrection, "My Studio Carla");
  QVERIFY(isolated.isEmpty());

  // 4. Delete User Preset
  QVERIFY(pm.deletePreset(PresetCategories::ExternalInsert, "My Studio Carla"));
  QVERIFY(!pm.listPresets(PresetCategories::ExternalInsert)
               .contains("My Studio Carla"));
  QVERIFY(pm.loadPreset(PresetCategories::ExternalInsert, "My Studio Carla")
              .isEmpty());
}

void TestPhase3ExternalInsert::testEngineControllerIntegration() {
  EngineController &controller = EngineController::instance();
  QVERIFY(controller.initialize(true));

  QSignalSpy insertSpy(&controller,
                       &EngineController::externalInsertStatusChanged);
  QSignalSpy presetSpy(&controller, &EngineController::presetManagementChanged);

  // 1. Configure External Insert
  bool configOk = controller.configureExternalInsert(
      "CarlaHost", "carla:audio_in_1", "carla:audio_in_2", "carla:audio_out_1",
      "carla:audio_out_2", 48000.0, 256, 128);
  QVERIFY(configOk);
  QVERIFY(insertSpy.count() > 0);
  QCOMPARE(controller.externalInsertReportedLatency(), 128);
  QCOMPARE(controller.externalInsertStateText(), QString("Connected"));
  QVERIFY(!controller.isExternalInsertBypassed());

  // 2. Telemetry Status Map
  QVariantMap status = controller.getExternalInsertStatus();
  QCOMPARE(status["clientName"].toString(), QString("CarlaHost"));
  QCOMPARE(status["reportedLatencyFrames"].toInt(), 128);
  QCOMPARE(status["sendPortL"].toString(), QString("carla:audio_in_1"));

  // 3. Bypass / Unbypass
  controller.setExternalInsertBypass(true);
  QVERIFY(controller.isExternalInsertBypassed());
  controller.setExternalInsertBypass(false);
  QVERIFY(!controller.isExternalInsertBypassed());

  // 4. Delay Compensation
  controller.setInsertDelayCompensation(64);
  QCOMPARE(controller.insertDelayCompensation(), 64);

  // 5. Latency measurement simulation
  controller.triggerInsertLatencyMeasurement();
  controller.recordInsertImpulseReturn(64 + 128);
  QCOMPARE(controller.externalInsertMeasuredLatency(), 128);
  QVERIFY(controller.externalInsertMeasuredLatencyMs() > 0.0);

  // 6. Disconnect / Recovery simulation
  controller.simulateInsertDisconnect();
  QCOMPARE(controller.externalInsertStateText(), QString("Disconnected"));
  QVERIFY(controller.isExternalInsertBypassed());

  controller.simulateInsertRecovery();
  QCOMPARE(controller.externalInsertStateText(), QString("Connected"));
  QVERIFY(!controller.isExternalInsertBypassed());

  // 7. Preset Management APIs
  QStringList lcList = controller.listPresets("listening_correction");
  QVERIFY(lcList.contains("Harman Target"));

  // Capture live state and save custom preset
  QVariantMap captured =
      controller.captureCurrentStateAsPreset("external_insert");
  QCOMPARE(captured["client_name"].toString(), QString("CarlaHost"));

  bool saveOk =
      controller.savePreset("external_insert", "SavedCarlaConfig", captured);
  QVERIFY(saveOk);
  QVERIFY(presetSpy.count() > 0);

  QVariantMap loaded =
      controller.loadPreset("external_insert", "SavedCarlaConfig");
  QCOMPARE(loaded["client_name"].toString(), QString("CarlaHost"));

  QVERIFY(controller.applyPresetProfile("external_insert", "SavedCarlaConfig"));

  // Cleanup
  QVERIFY(controller.deletePreset("external_insert", "SavedCarlaConfig"));

  // 8. Master Gain Independent L/R Controls & Synchronization
  QSignalSpy masterGainSpy(&controller, &EngineController::masterGainChanged);
  QSignalSpy masterGainLSpy(&controller, &EngineController::masterGainLChanged);
  QSignalSpy masterGainRSpy(&controller, &EngineController::masterGainRChanged);

  // Set Master Gain collectively
  controller.setMasterGain(0.8);
  QCOMPARE(controller.masterGain(), 0.8);
  QCOMPARE(controller.masterGainL(), 0.8);
  QCOMPARE(controller.masterGainR(), 0.8);
  QVERIFY(masterGainSpy.count() > 0);
  QVERIFY(masterGainLSpy.count() > 0);
  QVERIFY(masterGainRSpy.count() > 0);

  masterGainSpy.clear();
  masterGainLSpy.clear();
  masterGainRSpy.clear();

  // Set Master Gain L independently
  controller.setMasterGainL(1.2);
  QCOMPARE(controller.masterGainL(), 1.2);
  QCOMPARE(controller.masterGainR(), 0.8);
  QCOMPARE(controller.masterGain(), 1.0); // (1.2 + 0.8) / 2 = 1.0
  QCOMPARE(masterGainLSpy.count(), 1);
  QCOMPARE(masterGainRSpy.count(), 0);
  QCOMPARE(masterGainSpy.count(), 1);

  masterGainSpy.clear();
  masterGainLSpy.clear();
  masterGainRSpy.clear();

  // Set Master Gain R independently
  controller.setMasterGainR(1.6);
  QCOMPARE(controller.masterGainL(), 1.2);
  QCOMPARE(controller.masterGainR(), 1.6);
  QCOMPARE(controller.masterGain(), 1.4); // (1.2 + 1.6) / 2 = 1.4
  QCOMPARE(masterGainLSpy.count(), 0);
  QCOMPARE(masterGainRSpy.count(), 1);
  QCOMPARE(masterGainSpy.count(), 1);

  // Reset to unity
  controller.setMasterGain(1.0);
  QCOMPARE(controller.masterGainL(), 1.0);
  QCOMPARE(controller.masterGainR(), 1.0);
  QCOMPARE(controller.masterGain(), 1.0);
}

void TestPhase3ExternalInsert::
    testEngineControllerDspPreviewAndMasterControl() {
  EngineController &controller = EngineController::instance();

  // 1. DSP Parameter Get/Set and signals
  QSignalSpy filterSpy(&controller, &EngineController::filterParametersChanged);
  QSignalSpy compSpy(&controller,
                     &EngineController::compressorParametersChanged);
  QSignalSpy limSpy(&controller, &EngineController::limiterParametersChanged);
  QSignalSpy revSpy(&controller, &EngineController::reverbParametersChanged);

  controller.setFilterParams(180.0, true, 1, 9500.0, true, 0);
  QVERIFY(filterSpy.count() > 0);
  QVariantMap flt = controller.getFilterParams();
  QCOMPARE(flt["hpfFrequency"].toDouble(), 180.0);
  QCOMPARE(flt["hpfEnabled"].toBool(), true);
  QCOMPARE(flt["hpfSlope"].toInt(), 1); // 24 dB/oct
  QCOMPARE(flt["lpfFrequency"].toDouble(), 9500.0);
  QCOMPARE(flt["lpfEnabled"].toBool(), true);
  QCOMPARE(flt["lpfSlope"].toInt(), 0); // 12 dB/oct

  controller.setCompressorParams(-16.0, 3.5, 12.0, 90.0, 2.5, 4.0);
  QVERIFY(compSpy.count() > 0);
  QVariantMap comp = controller.getCompressorParams();
  QCOMPARE(comp["thresholdDb"].toDouble(), -16.0);
  QCOMPARE(comp["ratio"].toDouble(), 3.5);
  QCOMPARE(comp["attackMs"].toDouble(), 12.0);
  QCOMPARE(comp["releaseMs"].toDouble(), 90.0);
  QCOMPARE(comp["makeupGainDb"].toDouble(), 2.5);
  QCOMPARE(comp["kneeDb"].toDouble(), 4.0);

  controller.setLimiterParams(-2.0, 45.0, 3.0);
  QVERIFY(limSpy.count() > 0);
  QVariantMap lim = controller.getLimiterParams();
  QCOMPARE(lim["ceilingDb"].toDouble(), -2.0);
  QCOMPARE(lim["releaseMs"].toDouble(), 45.0);
  QCOMPARE(lim["lookaheadMs"].toDouble(), 3.0);

  controller.setReverbParams(0.7, 0.4, 0.25, 30.0, true);
  QVERIFY(revSpy.count() > 0);
  QVariantMap rev = controller.getReverbParams();
  QCOMPARE(rev["roomSize"].toDouble(), 0.7);
  QCOMPARE(rev["damping"].toDouble(), 0.4);
  QCOMPARE(rev["predelayMs"].toDouble(), 30.0);
  QCOMPARE(rev["tailPreserving"].toBool(), true);

  // 2. Individual Bypass vs Global Bypass Non-Destructive Behavior
  // (Section 10.2.3 & 5.9)
  controller.setDspNodeBypass("Filter", false);     // Active
  controller.setDspNodeBypass("Compressor", false); // Active
  controller.setDspNodeBypass("Reverb", true);      // Bypassed
  controller.setDspNodeBypass("Limiter", false);    // Active

  QCOMPARE(controller.isDspNodeBypassed("Filter"), false);
  QCOMPARE(controller.isDspNodeBypassed("Compressor"), false);
  QCOMPARE(controller.isDspNodeBypassed("Reverb"), true);
  QCOMPARE(controller.isDspNodeBypassed("Limiter"), false);

  // Engage Global Bypass
  controller.setGlobalDspBypass(true);
  QCOMPARE(controller.globalDspBypass(), true);

  // Must NOT destroy individual bypass flags!
  QCOMPARE(controller.isDspNodeBypassed("Filter"), false);
  QCOMPARE(controller.isDspNodeBypassed("Compressor"), false);
  QCOMPARE(controller.isDspNodeBypassed("Reverb"), true);
  QCOMPARE(controller.isDspNodeBypassed("Limiter"), false);

  // Disengage Global Bypass
  controller.setGlobalDspBypass(false);
  QCOMPARE(controller.globalDspBypass(), false);

  // Individual flags intact
  QCOMPARE(controller.isDspNodeBypassed("Filter"), false);
  QCOMPARE(controller.isDspNodeBypassed("Compressor"), false);
  QCOMPARE(controller.isDspNodeBypassed("Reverb"), true); // Still bypassed!
  QCOMPARE(controller.isDspNodeBypassed("Limiter"), false);

  // 3. Master Control: Master Gain, Mute & Headroom
  QSignalSpy muteSpy(&controller, &EngineController::masterMuteChanged);
  controller.setMasterGain(1.15);
  QCOMPARE(controller.masterGain(), 1.15);

  controller.setMasterMute(true);
  QCOMPARE(controller.isMasterMute(), true);
  QCOMPARE(muteSpy.count(), 1);

  controller.setMasterMute(false);
  QCOMPARE(controller.isMasterMute(), false);
  QCOMPARE(controller.masterGain(), 1.15); // Preserved

  // Telemetry getters
  QVERIFY(controller.masterHeadroomDb() >= 0.0);
  QCOMPARE(controller.masterClipped(), false);
}

void TestPhase3ExternalInsert::
    testGlobalDspBypassQueueConsistencyAndRollback() {
  EngineController &controller = EngineController::instance();
  auto pipe = controller.pipeline();
  QVERIFY(pipe != nullptr);
  desk32::engine::dsp::DspCommand dummy;

  // Initial state: disengaged
  controller.setGlobalDspBypass(false);
  QCOMPARE(controller.globalDspBypass(), false);

  // Drain any pending commands
  while (pipe->paramQueue().pop(dummy)) {
  }

  // Case 1: Normal toggle succeeds on both chains
  QSignalSpy bypassSpy(&controller, &EngineController::globalDspBypassChanged);
  controller.setGlobalDspBypass(true);
  QCOMPARE(controller.globalDspBypass(), true);
  QCOMPARE(bypassSpy.count(), 1);

  // Drain and verify AudioPipeline received commands
  while (pipe->paramQueue().pop(dummy)) {
    if (dummy.type == desk32::engine::dsp::DspCommandType::SetGlobalBypass) {
      QCOMPARE(dummy.boolFlag1, uint8_t(1));
    }
  }

  controller.setGlobalDspBypass(false);
  QCOMPARE(controller.globalDspBypass(), false);
  while (pipe->paramQueue().pop(dummy)) {
  }

  // Case 2: Parameter Queue Full (512 items)
  // Fill queue to capacity so hasSpace(2) and push() fail
  for (size_t i = 0; i < desk32::engine::dsp::ParameterCommandQueue::CAPACITY;
       ++i) {
    desk32::engine::dsp::DspCommand fillCmd;
    fillCmd.type = desk32::engine::dsp::DspCommandType::None;
    pipe->paramQueue().push(fillCmd);
  }
  QCOMPARE(pipe->paramQueue().hasSpace(2), false);

  QSignalSpy overflowSpy(&controller,
                         &EngineController::dspQueueOverflowWarning);
  bypassSpy.clear();

  // Attempt to engage Global Bypass with full queue
  controller.setGlobalDspBypass(true);

  // AC-01: UI and DSP state must NOT remain desynchronized; controller state
  // remains false!
  QCOMPARE(controller.globalDspBypass(), false);
  QVERIFY(overflowSpy.count() > 0);

  // Case 3: Partial push prevention via hasSpace(2)
  // Clear queue, fill to CAPACITY - 1 (only 1 slot left)
  while (pipe->paramQueue().pop(dummy)) {
  }
  for (size_t i = 0;
       i < desk32::engine::dsp::ParameterCommandQueue::CAPACITY - 1; ++i) {
    desk32::engine::dsp::DspCommand fillCmd;
    fillCmd.type = desk32::engine::dsp::DspCommandType::None;
    pipe->paramQueue().push(fillCmd);
  }
  QCOMPARE(pipe->paramQueue().hasSpace(2), false);

  overflowSpy.clear();
  bypassSpy.clear();
  controller.setGlobalDspBypass(true);

  // AC-02: Partial state avoided, state remains false
  QCOMPARE(controller.globalDspBypass(), false);
  QVERIFY(overflowSpy.count() > 0);

  // Case 4: Multi-Producer Concurrency Verification of pushTwo Atomicity (P1-1)
  // Verify that under concurrent push operations from multiple threads,
  // pushTwo() ALWAYS enqueues the Master and Listening commands consecutively
  // without interleaving from other producers.
  {
    desk32::engine::dsp::ParameterCommandQueue testQueue;
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<size_t> pushTwoSuccessCount{0};

    // 2 threads calling pushTwo
    std::vector<std::thread> producers;
    for (int t = 0; t < 2; ++t) {
      producers.emplace_back([&, t]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < 100; ++i) {
          desk32::engine::dsp::DspCommand c1, c2;
          c1.type = desk32::engine::dsp::DspCommandType::SetGlobalBypass;
          c1.subIndex = 0; // Master
          c1.index = static_cast<uint8_t>(t * 100 + i);
          c2.type = desk32::engine::dsp::DspCommandType::SetGlobalBypass;
          c2.subIndex = 1; // Listening
          c2.index = static_cast<uint8_t>(t * 100 + i);
          if (testQueue.pushTwo(c1, c2)) {
            pushTwoSuccessCount.fetch_add(1, std::memory_order_relaxed);
          }
          std::this_thread::yield();
        }
      });
    }

    // 2 threads calling single push
    for (int t = 0; t < 2; ++t) {
      producers.emplace_back([&, t]() {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < 100; ++i) {
          desk32::engine::dsp::DspCommand c;
          c.type = desk32::engine::dsp::DspCommandType::None;
          c.index = static_cast<uint8_t>(t * 100 + i);
          testQueue.push(c);
          std::this_thread::yield();
        }
      });
    }

    // Consumer thread popping and verifying that Master and Listening are
    // ALWAYS consecutive
    std::atomic<bool> atomicityViolation{false};
    std::thread consumer([&]() {
      while (!done.load(std::memory_order_acquire)) {
        desk32::engine::dsp::DspCommand cmd;
        if (testQueue.pop(cmd)) {
          if (cmd.type ==
                  desk32::engine::dsp::DspCommandType::SetGlobalBypass &&
              cmd.subIndex == 0) {
            desk32::engine::dsp::DspCommand nextCmd;
            while (!testQueue.pop(nextCmd)) {
              std::this_thread::yield();
            }
            if (nextCmd.type !=
                    desk32::engine::dsp::DspCommandType::SetGlobalBypass ||
                nextCmd.subIndex != 1 || nextCmd.index != cmd.index) {
              atomicityViolation.store(true, std::memory_order_relaxed);
            }
          }
        } else {
          std::this_thread::yield();
        }
      }
      // Drain remaining
      desk32::engine::dsp::DspCommand cmd;
      while (testQueue.pop(cmd)) {
        if (cmd.type == desk32::engine::dsp::DspCommandType::SetGlobalBypass &&
            cmd.subIndex == 0) {
          desk32::engine::dsp::DspCommand nextCmd;
          while (!testQueue.pop(nextCmd)) {
            std::this_thread::yield();
          }
          if (nextCmd.type !=
                  desk32::engine::dsp::DspCommandType::SetGlobalBypass ||
              nextCmd.subIndex != 1 || nextCmd.index != cmd.index) {
            atomicityViolation.store(true, std::memory_order_relaxed);
          }
        }
      }
    });

    start.store(true, std::memory_order_release);
    for (auto &p : producers)
      p.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    done.store(true, std::memory_order_release);
    consumer.join();

    QVERIFY(pushTwoSuccessCount.load() > 50);
    QCOMPARE(atomicityViolation.load(), false);
  }

  // Case 5: Queue drains and resync succeeds
  while (pipe->paramQueue().pop(dummy)) {
  }
  QCOMPARE(pipe->paramQueue().hasSpace(2), true);

  bool resyncOk = controller.resyncGlobalDspBypass();
  QCOMPARE(resyncOk, true);
  QCOMPARE(controller.isGlobalDspBypassDesynced(), false);

  // Clean up
  while (pipe->paramQueue().pop(dummy)) {
  }
  controller.setGlobalDspBypass(false);
  while (pipe->paramQueue().pop(dummy)) {
  }
}

void TestPhase3ExternalInsert::testDspNodeBypassQueueFailureAndConsistency() {
  EngineController &controller = EngineController::instance();
  auto pipe = controller.pipeline();
  QVERIFY(pipe != nullptr);
  desk32::engine::dsp::DspCommand dummy;

  // Initial state: ensure Filter is not bypassed (both Accepted and Applied in
  // Audio Thread)
  controller.setDspNodeBypass("Filter", true);
  controller.setDspNodeBypass("Filter", false);
  std::vector<float> initPcm(256 * 2, 0.0f);
  controller.renderOfflineBlock(initPcm.data(), 256);
  QCOMPARE(controller.isDspNodeBypassed("Filter"), false);
  QCOMPARE(controller.isDspNodeAppliedBypassed("Filter"), false);

  // Case 1: Requested -> Accepted -> Applied lifecycle verification (P1-2)
  QSignalSpy filterSpy(&controller, &EngineController::filterParametersChanged);
  controller.setDspNodeBypass("Filter", true);

  // 1. Accepted: updated in Controller atomic state and UI model
  QCOMPARE(controller.isDspNodeBypassed("Filter"), true);
  QVERIFY(filterSpy.count() > 0);

  // 2. Not yet Applied in Audio Thread (pipeline has not drained parameter
  // command queue yet)
  QCOMPARE(controller.isDspNodeAppliedBypassed("Filter"), false);

  // 3. Audio Thread cycle runs (renderOfflineBlock drains parameter queue into
  // active DSP nodes)
  std::vector<float> pcm(256 * 2, 0.0f);
  controller.renderOfflineBlock(pcm.data(), 256);

  // 4. Applied: DSP node in Audio Thread now confirms bypass engaged
  QCOMPARE(controller.isDspNodeAppliedBypassed("Filter"), true);

  // Toggle back to false
  filterSpy.clear();
  controller.setDspNodeBypass("Filter", false);
  QCOMPARE(controller.isDspNodeBypassed("Filter"), false); // Accepted
  QCOMPARE(controller.isDspNodeAppliedBypassed("Filter"),
           true); // Still true until Audio Thread runs
  controller.renderOfflineBlock(pcm.data(), 256); // Audio Thread cycle
  QCOMPARE(controller.isDspNodeAppliedBypassed("Filter"), false); // Applied
  QVERIFY(filterSpy.count() > 0);

  // Case 2: Parameter Queue Full -> Failure handling
  for (size_t i = 0; i < desk32::engine::dsp::ParameterCommandQueue::CAPACITY;
       ++i) {
    desk32::engine::dsp::DspCommand fillCmd;
    fillCmd.type = desk32::engine::dsp::DspCommandType::None;
    pipe->paramQueue().push(fillCmd);
  }

  QSignalSpy overflowSpy(&controller,
                         &EngineController::dspQueueOverflowWarning);
  filterSpy.clear();

  // Attempt to bypass Filter while queue is full
  controller.setDspNodeBypass("Filter", true);

  // AC-04: UI and DSP state must remain consistent on failure: state must stay
  // false!
  QCOMPARE(controller.isDspNodeBypassed("Filter"), false);
  QCOMPARE(controller.isDspNodeAppliedBypassed("Filter"), false);
  QVERIFY(overflowSpy.count() > 0);
  // UI signal re-emitted so UI reverts to false
  QVERIFY(filterSpy.count() > 0);

  // Drain queue
  while (pipe->paramQueue().pop(dummy)) {
  }

  // Case 3: Resynchronization
  bool resyncOk = controller.resyncDspNodeBypass("Filter");
  QCOMPARE(resyncOk, true);
  while (pipe->paramQueue().pop(dummy)) {
  }

  // Case 4: Node bypass does not affect Global DSP Bypass
  controller.setGlobalDspBypass(false);
  controller.setDspNodeBypass("Compressor", true);
  QCOMPARE(controller.isDspNodeBypassed("Compressor"), true);
  QCOMPARE(controller.globalDspBypass(), false);

  controller.setDspNodeBypass("Compressor", false);
  while (pipe->paramQueue().pop(dummy)) {
  }
}

void TestPhase3ExternalInsert::
    testPhase3GapFixesInsertPoliciesDiscoveryAndSessionState() {
  using namespace desk32::engine::insert;
  using namespace desk32::engine::preset;
  using desk32::core::EngineController;

  // 1. Item 16: pw-dump JSON port discovery & verified port recovery
  const std::string fakePwDump = R"([
      {"id": 10, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "CarlaRack", "node.latency": "256/48000", "latency.internal.rate": 128}}},
      {"id": 11, "type": "PipeWire:Interface:Port", "info": {"direction": "input", "props": {"node.id": 10, "media.type": "Audio", "port.name": "audio_in_1"}}},
      {"id": 12, "type": "PipeWire:Interface:Port", "info": {"direction": "input", "props": {"node.id": 10, "media.type": "Audio", "port.name": "audio_in_2"}}},
      {"id": 13, "type": "PipeWire:Interface:Port", "info": {"direction": "output", "props": {"node.id": 10, "media.type": "Audio", "port.name": "audio_out_1"}}},
      {"id": 14, "type": "PipeWire:Interface:Port", "info": {"direction": "output", "props": {"node.id": 10, "media.type": "Audio", "port.name": "audio_out_2"}}}
    ])";
  const auto discovered =
      ExternalInsertBridge::parsePwDumpPorts(fakePwDump, 48000.0, 256);
  QCOMPARE(discovered.size(), size_t(1));
  QCOMPARE(discovered[0].clientName, std::string("CarlaRack"));
  QCOMPARE(discovered[0].sendPortL, std::string("CarlaRack:audio_in_1"));
  QCOMPARE(discovered[0].returnPortR, std::string("CarlaRack:audio_out_2"));
  QCOMPARE(discovered[0].reportedLatencyFrames, uint32_t(128));
  QVERIFY(discovered[0].latencyReported);

  ExternalInsertBridge recBridge;
  QVERIFY(recBridge.configure(discovered[0], 48000.0, 256));
  recBridge.handleDisconnect();
  QVERIFY(
      !recBridge.handleRecoveryWithVerification({})); // Missing ports -> Error
  QCOMPARE(recBridge.state(), InsertState::Error);
  QVERIFY(recBridge.handleRecoveryWithVerification(
      discovered)); // Matching ports -> Connected
  QCOMPARE(recBridge.state(), InsertState::Connected);

  // 2. Item 18 & 19: DisconnectPolicy (AutoBypass/Mute/Stop), Not Reported vs 0
  // frames, auto impulse return detection
  ExternalInsertBridge bridge;
  ExternalPortInfo unrep{
      "NoLatHost",       "n:in1",     "n:in2", "n:out1", "n:out2",
      48000.0,           256,         0,       false,    "MyPlugin",
      "lv2://my/plugin", "WarmPreset"};
  QVERIFY(bridge.configure(unrep, 48000.0, 256));
  QCOMPARE(bridge.hasReportedLatency(), false);
  QCOMPARE(bridge.reportedLatencyText(), std::string("Not Reported"));

  // Ring buffer loopback mock with 48-frame delay
  constexpr size_t DELAY = 48;
  std::vector<double> ringL(512, 0.0), ringR(512, 0.0);
  size_t writePos = 0;
  bridge.setMockProcessor(
      [&ringL, &ringR, &writePos](double *l, double *r, size_t n) {
        for (size_t i = 0; i < n; ++i) {
          ringL[(writePos + DELAY) % 512] = l[i];
          ringR[(writePos + DELAY) % 512] = r[i];
          l[i] = ringL[writePos % 512];
          r[i] = ringR[writePos % 512];
          ringL[writePos % 512] = 0.0;
          ringR[writePos % 512] = 0.0;
          ++writePos;
        }
      });
  bridge.triggerLatencyMeasurement();
  std::vector<double> bL(256, 0.0), bR(256, 0.0);
  bridge.process(
      bL.data(), bR.data(),
      256); // Send at frame 64, detected automatically at frame 64+48
  QCOMPARE(bridge.measuredLatencyFrames(), int64_t(DELAY));

  bridge.setDisconnectPolicy(DisconnectPolicy::MuteOutput);
  bridge.handleDisconnect();
  std::fill(bL.begin(), bL.end(), 0.7);
  std::fill(bR.begin(), bR.end(), 0.7);
  bridge.process(bL.data(), bR.data(), 256);
  QCOMPARE(bL[0], 0.0);

  bridge.handleRecovery();
  bridge.setDisconnectPolicy(DisconnectPolicy::StopPlayback);
  bridge.handleDisconnect();
  bridge.process(bL.data(), bR.data(), 256);
  QVERIFY(bridge.stopPlaybackRequested());

  // 3. Item 17 & 20: Preset format_version, Insert position reordering,
  // AudioDevice preset, session state save/restore
  PresetManager &pm = PresetManager::instance();
  pm.setCustomRootPath(m_tempDir->path() + "/p3_gap_presets");
  // Write future format_version preset directly -> loadPreset must reject it
  {
    QString badPath =
        pm.categoryDirectory(PresetCategories::ListeningCorrection) +
        "/future_ver.json";
    QFile bf(badPath);
    QVERIFY(bf.open(QIODevice::WriteOnly));
    bf.write("{\"category\":\"listening_correction\",\"name\":\"future_ver\","
             "\"format_version\":999,\"bands\":[]}");
    bf.close();
    QVERIFY(pm.loadPreset(PresetCategories::ListeningCorrection, "future_ver")
                .isEmpty());
  }

  EngineController &ctrl = EngineController::instance();
  QVERIFY(ctrl.initialize(true));
  QVERIFY(ctrl.configureExternalInsertWithMetadata(
      "Carla", "c:in1", "c:in2", "c:out1", "c:out2", 48000.0, 256, 0, false,
      "LSP Comp", "http://lsp-plug.in/comp", "StudioPreset1"));
  ctrl.setExternalInsertPosition(0); // Move ExternalInsert before PEQ!
  QCOMPARE(ctrl.externalInsertPosition(), 0);
  // Reverb bypass still resolves properly via dynamic node mapping
  ctrl.setDspNodeBypass("Reverb", false);
  QCOMPARE(ctrl.isDspNodeBypassed("Reverb"), false);
  ctrl.setExternalInsertDisconnectPolicy(
      static_cast<int>(DisconnectPolicy::AutoBypass));

  // AudioDevice preset save & apply
  ctrl.setFixedGraphSampleRate(96000.0);
  ctrl.setListeningVolume(0.75);
  QVERIFY(ctrl.savePreset(
      PresetCategories::AudioDevice, "Rig96k",
      ctrl.captureCurrentStateAsPreset(PresetCategories::AudioDevice)));
  ctrl.setFixedGraphSampleRate(48000.0);
  ctrl.setListeningVolume(1.0);
  QVERIFY(ctrl.applyPresetProfile(PresetCategories::AudioDevice, "Rig96k"));
  QCOMPARE(ctrl.graphSampleRate(), 96000.0);
  QVERIFY(std::abs(ctrl.listeningVolume() - 0.75) < 1e-6);

  // Session state save & restore
  const QString sessPath = m_tempDir->path() + "/session_state.json";
  QVERIFY(ctrl.saveSessionState(sessPath));
  ctrl.setFixedGraphSampleRate(48000.0);
  ctrl.setDspNodeBypass("Reverb", true);
  QVERIFY(ctrl.restoreSessionState(sessPath));
  QCOMPARE(ctrl.graphSampleRate(), 96000.0);
  QCOMPARE(ctrl.isDspNodeBypassed("Reverb"), false);
  QCOMPARE(ctrl.externalInsertPosition(), 0);
  const QVariantMap st = ctrl.getExternalInsertStatus();
  QCOMPARE(st["latencyReported"].toBool(), false);
  QCOMPARE(st["reportedLatencyText"].toString(), QString("Not Reported"));
  QCOMPARE(st["pluginName"].toString(), QString("LSP Comp"));
  QCOMPARE(st["hostPresetReference"].toString(), QString("StudioPreset1"));
}

void TestPhase3ExternalInsert::
    testPipeWireInsertTransportAndTwoStepLatencyCalibration() {
  // 1. PipeWireInsertTransport simulation loopback + graph removal +
  // rate/buffer mismatch detection
  auto transport = std::make_shared<PipeWireInsertTransport>();
  transport->setSimulationMode(true);
  transport->setSimulationLoopbackLatencyFrames(32);

  ExternalInsertBridge bridge;
  bridge.setTransport(transport);
  ExternalPortInfo cfg{"SimHost",   "sim:in_l", "sim:in_r", "sim:out_l",
                       "sim:out_r", 48000.0,    256,        32};
  QVERIFY(bridge.configure(cfg, 48000.0, 256));
  QVERIFY(bridge.isTransportLiveConnected());
  QCOMPARE(bridge.transportDiagnostics().activeLinksCreated, uint32_t(4));

  bridge.triggerLatencyMeasurement();
  std::vector<double> l(256, 0.0), r(256, 0.0);
  bridge.process(l.data(), r.data(), 256);
  QCOMPARE(bridge.measuredLatencyFrames(), int64_t(32));

  transport->simulateGraphNodeRemoved();
  std::fill(l.begin(), l.end(), 0.25);
  std::fill(r.begin(), r.end(), 0.25);
  bridge.process(l.data(), r.data(), 256);
  QCOMPARE(bridge.state(), InsertState::Disconnected);
  QVERIFY(bridge.isBypassed());

  // Rate & buffer mismatch monitoring
  QVERIFY(bridge.configure(cfg, 48000.0, 256));
  transport->simulateRateMismatch(96000);
  bridge.process(l.data(), r.data(), 256);
  QCOMPARE(bridge.state(), InsertState::RateMismatch);
  QVERIFY(bridge.isBypassed());

  QVERIFY(bridge.configure(cfg, 48000.0, 256));
  transport->simulateBufferMismatch(512);
  bridge.process(l.data(), r.data(), 256);
  QCOMPARE(bridge.state(), InsertState::BufferMismatch);
  QVERIFY(bridge.isBypassed());

  // 2. Two-step Latency Alignment & Stability Judgment via EngineController
  EngineController &ctrl = EngineController::instance();
  QVERIFY(ctrl.initialize(true));
  QVERIFY(ctrl.configureExternalInsert("SimRack", "s:in1", "s:in2", "s:out1",
                                       "s:out2", 48000.0, 256, 0));
  ctrl.setInsertDelayCompensation(0);

  // Case A: High jitter -> Unstable -> confirm refused
  ctrl.beginInsertLatencyCalibration();
  QVERIFY(ctrl.recordInsertLatencyCalibrationSample(48, 36.0));
  QVERIFY(!ctrl.recordInsertLatencyCalibrationSample(55, 36.0)); // spread=7 > 2
  QCOMPARE(ctrl.getLatencyAlignmentReport()["confidenceText"].toString(),
           QString("Unstable"));
  QVERIFY(!ctrl.confirmRecommendedInsertLatency(false));
  QCOMPARE(ctrl.insertDelayCompensation(), 0); // Auto-apply held!

  // Case B: Low SNR -> LowSnr -> confirm refused
  ctrl.beginInsertLatencyCalibration();
  QVERIFY(!ctrl.recordInsertLatencyCalibrationSample(64, 9.5)); // < 18 dB
  QCOMPARE(ctrl.getLatencyAlignmentReport()["confidenceText"].toString(),
           QString("LowSnr"));
  QVERIFY(!ctrl.confirmRecommendedInsertLatency(false));
  QCOMPARE(ctrl.insertDelayCompensation(), 0);

  // Case C: Stable multi-sample -> High confidence, NOT applied until
  // confirmRecommendedInsertLatency()
  ctrl.beginInsertLatencyCalibration();
  QVERIFY(ctrl.recordInsertLatencyCalibrationSample(64, 42.0));
  QVERIFY(ctrl.recordInsertLatencyCalibrationSample(65, 41.0));
  QVERIFY(ctrl.recordInsertLatencyCalibrationSample(64, 43.0));
  const QVariantMap rep = ctrl.getLatencyAlignmentReport();
  QCOMPARE(rep["confidenceText"].toString(), QString("High"));
  QCOMPARE(rep["recommendedFrames"].toLongLong(), qint64(64));
  QCOMPARE(rep["confirmed"].toBool(), false);
  QCOMPARE(ctrl.insertDelayCompensation(), 0); // Not auto-applied!

  QVERIFY(ctrl.confirmRecommendedInsertLatency());
  QCOMPARE(ctrl.insertDelayCompensation(), 64);
  QCOMPARE(ctrl.getLatencyAlignmentReport()["confirmed"].toBool(), true);
}

void TestPhase3ExternalInsert::
    testLiveTransportEnforcementCalibrationAndReconnect() {
  // 1. Live mode (simulationMode == false) with requireLiveTransport == true
  // must NOT fake Connected
  auto liveTr = std::make_shared<PipeWireInsertTransport>();
  liveTr->setSimulationMode(false);
  ExternalInsertBridge strictBridge;
  strictBridge.setTransport(liveTr);
  strictBridge.setRequireLiveTransport(true);
  ExternalPortInfo bogusCfg{
      "NonExistentHost_XYZ", "bogus:in_1", "bogus:in_2", "bogus:out_1",
      "bogus:out_2",         48000.0,      256,          0};
  QVERIFY(!strictBridge.configure(bogusCfg, 48000.0, 256));
  QCOMPARE(strictBridge.state(), InsertState::HostNotFound);
  QVERIFY(strictBridge.isBypassed());
  QVERIFY(!strictBridge.isTransportLiveConnected());

  // 2. pw-dump parser skips desk32-* nodes and orders _2 / _1 naturally to L /
  // R
  const std::string dumpJson = R"([
      {"id": 1, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "desk32-insert-send"}}},
      {"id": 2, "type": "PipeWire:Interface:Port", "info": {"direction": "input", "props": {"node.id": 1, "media.type": "Audio", "port.name": "in_1"}}},
      {"id": 3, "type": "PipeWire:Interface:Port", "info": {"direction": "input", "props": {"node.id": 1, "media.type": "Audio", "port.name": "in_2"}}},
      {"id": 4, "type": "PipeWire:Interface:Port", "info": {"direction": "output", "props": {"node.id": 1, "media.type": "Audio", "port.name": "out_1"}}},
      {"id": 5, "type": "PipeWire:Interface:Port", "info": {"direction": "output", "props": {"node.id": 1, "media.type": "Audio", "port.name": "out_2"}}},
      {"id": 10, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "Carla", "node.rate": "1/48000"}}},
      {"id": 11, "type": "PipeWire:Interface:Port", "info": {"direction": "input", "props": {"node.id": 10, "media.type": "Audio", "port.name": "audio_in_2"}}},
      {"id": 12, "type": "PipeWire:Interface:Port", "info": {"direction": "input", "props": {"node.id": 10, "media.type": "Audio", "port.name": "audio_in_1"}}},
      {"id": 13, "type": "PipeWire:Interface:Port", "info": {"direction": "output", "props": {"node.id": 10, "media.type": "Audio", "port.name": "audio_out_2"}}},
      {"id": 14, "type": "PipeWire:Interface:Port", "info": {"direction": "output", "props": {"node.id": 10, "media.type": "Audio", "port.name": "audio_out_1"}}}
    ])";
  const auto parsed =
      ExternalInsertBridge::parsePwDumpPorts(dumpJson, 44100.0, 256);
  QCOMPARE(parsed.size(), size_t(1));
  QCOMPARE(parsed[0].clientName, std::string("Carla"));
  QCOMPARE(parsed[0].sendPortL, std::string("Carla:audio_in_1"));
  QCOMPARE(parsed[0].sendPortR, std::string("Carla:audio_in_2"));
  QCOMPARE(parsed[0].returnPortL, std::string("Carla:audio_out_1"));
  QCOMPARE(parsed[0].returnPortR, std::string("Carla:audio_out_2"));
  QCOMPARE(parsed[0].sampleRate, 48000.0);

  // 3. EngineController::calibrateInsertLatency() refuses when disconnected,
  //    measures actual loopback delay through transport (not fake 128) when
  //    connected, and syncs pipeline node bypass + reconnects cleanly.
  EngineController &ctrl = EngineController::instance();
  QVERIFY(ctrl.initialize(true));
  QVERIFY(ctrl.insertNode()->bridge().requireLiveTransport());

  // Disconnected calibration must NOT fabricate 128 frames
  ctrl.calibrateInsertLatency();
  QCOMPARE(ctrl.externalInsertMeasuredLatency(), -1);

  // Connect with 48-frame loopback latency and verify pipeline bypass sync +
  // real probe calibration
  ctrl.insertTransport()->setSimulationLoopbackLatencyFrames(48);
  QVERIFY(ctrl.configureExternalInsert("CalibHost", "c:in1", "c:in2", "c:out1",
                                       "c:out2", 48000.0, 256, 0));
  std::vector<float> dummyOut(256 * 2, 0.0f);
  ctrl.renderOfflineBlock(dummyOut.data(),
                          256); // Drain SetNodeBypass command on audio pipeline
  QCOMPARE(ctrl.isDspNodeAppliedBypassed("ExternalInsert"), false);

  ctrl.calibrateInsertLatency();
  QCOMPARE(ctrl.externalInsertMeasuredLatency(), 48);
  QVERIFY(!ctrl.pipeline()->calibrationMonitorMute());
  QVERIFY(!ctrl.insertNode()->bridge().isCalibrationHoldMute());
  const QVariantMap rep = ctrl.getLatencyAlignmentReport();
  QCOMPARE(rep["hasMeasurement"].toBool(), true);
  QCOMPARE(rep["recommendedFrames"].toLongLong(), qint64(48));
  QCOMPARE(rep["confidenceText"].toString(), QString("High"));
  QVERIFY(rep["peakSnrDb"].toDouble() > 18.0);
  QVERIFY(ctrl.confirmRecommendedInsertLatency(false));
  QCOMPARE(ctrl.insertDelayCompensation(), 48);

  // Verify smooth calibration monitor mute in AudioPipeline preserves user mute
  // state
  {
    auto pipe = ctrl.pipeline();
    QVERIFY(!pipe->listeningMuted());
    QVERIFY(!pipe->renderMonitorMuted());
    std::vector<double> pl(512, 0.5), pr(512, -0.5);
    std::vector<float> pcmOut(512 * 2, 0.0f);
    // Prime smoother at 1.0
    pipe->process(pl.data(), pr.data(), 512, pcmOut.data(),
                  desk32::engine::format::SampleFormat::Float32, false);
    QVERIFY(std::abs(pcmOut[1022]) > 0.1f);

    // Engage calibration monitor mute -> ramps smoothly to 0.0 (~100ms for 10ms
    // exponential tau)
    pipe->setCalibrationMonitorMute(true);
    for (int b = 0; b < 10; ++b) {
      std::fill(pl.begin(), pl.end(), 0.5);
      std::fill(pr.begin(), pr.end(), -0.5);
      pipe->process(pl.data(), pr.data(), 512, pcmOut.data(),
                    desk32::engine::format::SampleFormat::Float32, false);
    }
    QVERIFY(std::abs(pcmOut[1022]) < 1e-4f);
    QVERIFY(!pipe->listeningMuted());
    QVERIFY(!pipe->renderMonitorMuted());

    // Release calibration monitor mute -> smoothly restores
    pipe->setCalibrationMonitorMute(false);
    for (int b = 0; b < 10; ++b) {
      std::fill(pl.begin(), pl.end(), 0.5);
      std::fill(pr.begin(), pr.end(), -0.5);
      pipe->process(pl.data(), pr.data(), 512, pcmOut.data(),
                    desk32::engine::format::SampleFormat::Float32, false);
    }
    QVERIFY(std::abs(pcmOut[1022]) > 0.49f);
  }

  // Disconnect -> pipeline node bypassed -> reconnect restores connection &
  // pipeline unbypass
  ctrl.disconnectExternalInsert();
  QCOMPARE(ctrl.externalInsertStateText(), QString("Disconnected"));
  ctrl.renderOfflineBlock(dummyOut.data(), 256);
  QCOMPARE(ctrl.isDspNodeAppliedBypassed("ExternalInsert"), true);

  QVERIFY(ctrl.reconnectExternalInsert());
  QCOMPARE(ctrl.externalInsertStateText(), QString("Connected"));
  ctrl.renderOfflineBlock(dummyOut.data(), 256);
  QCOMPARE(ctrl.isDspNodeAppliedBypassed("ExternalInsert"), false);
  QCOMPARE(ctrl.externalInsertStateText(), QString("Active"));

  // 4. Regression: RT Audio Thread consumes graph disconnect event first while
  // Active,
  //    auto-engages Dry bypass, and Control Thread polling MUST still record
  //    GraphDisconnect log.
  QSignalSpy statusSpy(&ctrl, &EngineController::externalInsertStatusChanged);
  ctrl.insertTransport()->simulateGraphNodeRemoved();
  ctrl.renderOfflineBlock(dummyOut.data(),
                          256); // Audio thread runs before Control thread timer
  QCOMPARE(ctrl.externalInsertStateText(), QString("Disconnected"));
  QVERIFY(ctrl.isExternalInsertBypassed());

  // Wait for 33ms dspTimer -> pollExternalInsertHealth() to detect RT notice
  // and log GraphDisconnect
  QTRY_VERIFY_WITH_TIMEOUT(statusSpy.count() > 0, 1000);
  desk32::core::log::LogService::instance().flush();
  QVariantMap logFilter;
  logFilter["category"] = QString("ExternalInsert");
  const QVariantList logs =
      desk32::core::log::LogService::instance().fetchLogs(logFilter);
  bool foundGraphDisconnect = false;
  for (const QVariant &v : logs) {
    const QVariantMap row = v.toMap();
    if (row["operation"].toString() == QString("GraphDisconnect")) {
      foundGraphDisconnect = true;
      break;
    }
  }
  QVERIFY(foundGraphDisconnect);
}

void TestPhase3ExternalInsert::testMasterBalanceMuteLRDimAndDirectModeSafety() {
  EngineController &ctrl = EngineController::instance();
  ctrl.setMasterGain(1.0);
  ctrl.setMasterMute(false);
  ctrl.setMasterMuteL(false);
  ctrl.setMasterMuteR(false);
  ctrl.setMasterBalance(0.0);
  ctrl.setMonitorDimEnabled(false);

  // 1. Q-04: Engaging L Mute or R Mute automatically disengages Direct Mode
  ctrl.setDirectModeEnabled(true);
  QVERIFY(ctrl.isDirectModeEnabled());
  ctrl.setMasterMuteL(true);
  QVERIFY(ctrl.isMasterMuteL());
  QVERIFY(!ctrl.isDirectModeEnabled());
  ctrl.setMasterMuteL(false);

  ctrl.setDirectModeEnabled(true);
  QVERIFY(ctrl.isDirectModeEnabled());
  ctrl.setMasterMuteR(true);
  QVERIFY(ctrl.isMasterMuteR());
  QVERIFY(!ctrl.isDirectModeEnabled());
  ctrl.setMasterMuteR(false);

  // 2. Q-03: Monitor Dim (-20 dB = 0.1 amplitude) affects monitor output
  // cleanly
  auto pipe = ctrl.pipeline();
  QVERIFY(pipe != nullptr);
  pipe->setOutputMode(desk32::engine::routing::OutputPathMode::ListeningPath);
  ctrl.setListeningVolume(1.0);
  ctrl.setListeningMuted(false);

  constexpr size_t frames = 512;
  std::vector<double> pl(frames, 0.8), pr(frames, 0.8);
  std::vector<float> pcm(frames * 2, 0.0f);
  for (int b = 0; b < 8; ++b) {
    std::fill(pl.begin(), pl.end(), 0.8);
    std::fill(pr.begin(), pr.end(), 0.8);
    pipe->process(pl.data(), pr.data(), frames, pcm.data(),
                  desk32::engine::format::SampleFormat::Float32, false);
  }
  const float normalAmp = pcm[(frames - 1) * 2];
  QVERIFY(std::abs(normalAmp - 0.8f) < 0.02f);

  ctrl.setMonitorDimEnabled(true);
  QVERIFY(ctrl.isMonitorDimEnabled());
  QCOMPARE(ctrl.bitPerfectStatus(),
           1); // Monitor Dim marks Bit-Perfect NotVerified
  for (int b = 0; b < 10; ++b) {
    std::fill(pl.begin(), pl.end(), 0.8);
    std::fill(pr.begin(), pr.end(), 0.8);
    pipe->process(pl.data(), pr.data(), frames, pcm.data(),
                  desk32::engine::format::SampleFormat::Float32, false);
  }
  const float dimAmp = pcm[(frames - 1) * 2];
  QVERIFY(std::abs(dimAmp - 0.08f) < 0.01f); // Exact -20 dB (0.8 * 0.1 = 0.08)

  // Render Tap peak inside telemetry MUST remain undimmed (0.8)
  QVERIFY(std::abs(pipe->currentTelemetry().renderTapPeakL - 0.8) < 1e-4);

  // 3. Balance non-zero marks Bit-Perfect NotVerified and clamps [-1, 1]
  ctrl.setMonitorDimEnabled(false);
  ctrl.setMasterBalance(2.5);
  QCOMPARE(ctrl.masterBalance(), 1.0);
  QCOMPARE(ctrl.bitPerfectStatus(), 1);
  ctrl.setMasterBalance(0.0);
}

void TestPhase3ExternalInsert::testWaveformExtractorCacheAndSpectrumRing() {
  QTemporaryDir cacheTmp;
  QVERIFY(cacheTmp.isValid());
  qputenv("XDG_CACHE_HOME", cacheTmp.path().toUtf8());

  // Build a tiny 0.2s 48kHz 16-bit stereo WAV file with a 1kHz sine wave at 0.5
  // peak
  const QString wavPath = cacheTmp.path() + "/sine1k.wav";
  {
    QFile f(wavPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const uint32_t sampleRate = 48000;
    const uint32_t totalSamples = 9600;
    const uint16_t channels = 2;
    const uint16_t bits = 16;
    const uint32_t dataBytes = totalSamples * channels * (bits / 8);
    const uint32_t riffSize = 36 + dataBytes;
    const uint32_t byteRate = sampleRate * channels * (bits / 8);
    const uint16_t blockAlign = channels * (bits / 8);
    auto w16 = [&](uint16_t v) {
      f.write(reinterpret_cast<const char *>(&v), 2);
    };
    auto w32 = [&](uint32_t v) {
      f.write(reinterpret_cast<const char *>(&v), 4);
    };
    f.write("RIFF", 4);
    w32(riffSize);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    w32(16);
    w16(1);
    w16(channels);
    w32(sampleRate);
    w32(byteRate);
    w16(blockAlign);
    w16(bits);
    f.write("data", 4);
    w32(dataBytes);
    for (uint32_t i = 0; i < totalSamples; ++i) {
      const double val = 0.5 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 *
                                        i / sampleRate);
      const int16_t s16 =
          static_cast<int16_t>(std::clamp(val, -1.0, 1.0) * 32767.0);
      w16(static_cast<uint16_t>(s16));
      w16(static_cast<uint16_t>(s16));
    }
    f.close();
  }

  {
    desk32::engine::audio::WaveformExtractor ext1;
    std::atomic<bool> ready{false};
    ext1.setReadyCallback([&ready](const std::string &, bool ok) {
      if (ok)
        ready.store(true, std::memory_order_release);
    });
    ext1.requestWaveform(wavPath.toStdString());
    QTRY_VERIFY_WITH_TIMEOUT(ready.load(std::memory_order_acquire), 3000);

    desk32::engine::audio::WaveformData wd;
    QVERIFY(ext1.getWaveform(wavPath.toStdString(), wd));
    QCOMPARE(wd.bucketCount, uint32_t(1024));
    QVERIFY(wd.totalFrames >= 9000);
    QCOMPARE(wd.rmsL.size(), size_t(1024));
    QCOMPARE(wd.rmsR.size(), size_t(1024));
    float maxPeak = 0.0f;
    double sumRmsSq = 0.0;
    for (float v : wd.maxL)
      maxPeak = std::max(maxPeak, v);
    for (float v : wd.rmsL)
      sumRmsSq += static_cast<double>(v) * static_cast<double>(v);
    const double meanRms =
        std::sqrt(sumRmsSq / static_cast<double>(wd.rmsL.size()));
    QVERIFY(maxPeak > 0.45f && maxPeak < 0.55f);
    // 0.5 peak sine wave RMS = 0.5 / sqrt(2) ~= 0.3536
    QVERIFY2(meanRms > 0.32 && meanRms < 0.39,
             qPrintable(QString("meanRms=%1").arg(meanRms)));
    // 0.5 peak stereo 1kHz sine ~= -6.02 LUFS
    QVERIFY(wd.hasLufs);
    QVERIFY2(std::abs(wd.integratedLufs - (-6.02)) < 0.35,
             qPrintable(QString("extracted lufs=%1").arg(wd.integratedLufs)));
    const auto lufsInfo = ext1.queryTrackLufs(wavPath.toStdString());
    QVERIFY(lufsInfo.ready && lufsInfo.hasLufs && !lufsInfo.measuring);
    QVERIFY(std::abs(lufsInfo.lufs - wd.integratedLufs) < 1e-6);
    QCOMPARE(ext1.diskCacheFileCount(), size_t(1));
    QVERIFY(ext1.diskCacheSizeBytes() > 24000);

    // Fresh extractor loads directly from disk cache (preserving RMS & LUFS)
    // and clearDiskCache cleans it
    desk32::engine::audio::WaveformExtractor ext2;
    std::atomic<bool> ready2{false};
    ext2.setReadyCallback([&ready2](const std::string &, bool ok) {
      if (ok)
        ready2.store(true, std::memory_order_release);
    });
    ext2.requestWaveform(wavPath.toStdString());
    QTRY_VERIFY_WITH_TIMEOUT(ready2.load(std::memory_order_acquire), 2000);
    desk32::engine::audio::WaveformData wdCached;
    QVERIFY(ext2.getWaveform(wavPath.toStdString(), wdCached));
    QVERIFY(wdCached.hasLufs);
    QVERIFY(std::abs(wdCached.integratedLufs - wd.integratedLufs) < 1e-6);
    QCOMPARE(wdCached.rmsL.size(), size_t(1024));
    QVERIFY(ext2.clearDiskCache());
    QCOMPARE(ext2.diskCacheFileCount(), size_t(0));
  }

  // RT-safe FFT SPSC sample push (0 heap allocations) + 1kHz peak check via
  // EngineController
  EngineController &ctrl = EngineController::instance();
  auto pipe = ctrl.pipeline();
  QVERIFY(pipe != nullptr);
  constexpr size_t blk = 512;
  std::vector<double> pl(blk, 0.0), pr(blk, 0.0);
  std::vector<float> pcm(blk * 2, 0.0f);

  t_isAudioThread = true;
  s_allocationCount.store(0);
  s_trackAllocations.store(true);
  for (int iter = 0; iter < 6; ++iter) {
    for (size_t i = 0; i < blk; ++i) {
      const double idx =
          static_cast<double>(static_cast<size_t>(iter) * blk + i);
      const double s =
          0.5 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * idx / 48000.0);
      pl[i] = s;
      pr[i] = s;
    }
    pipe->process(pl.data(), pr.data(), blk, pcm.data(),
                  desk32::engine::format::SampleFormat::Float32, false);
  }
  s_trackAllocations.store(false);
  t_isAudioThread = false;
  QCOMPARE(s_allocationCount.load(), size_t(0));

  const QVariantList bins = ctrl.getSpectrumBins(0, 160);
  QCOMPARE(bins.size(), 160);
  // 1kHz sits cleanly around bin index ~91 on 20Hz..20kHz log scale: check
  // strong peak vs 60Hz/15kHz
  double peakDb = -90.0;
  int peakIdx = -1;
  for (int b = 0; b < bins.size(); ++b) {
    if (bins[b].toDouble() > peakDb) {
      peakDb = bins[b].toDouble();
      peakIdx = b;
    }
  }
  QVERIFY(peakDb > -10.0);
  QVERIFY(peakIdx >= 75 && peakIdx <= 105);

  // Master mute must immediately drop spectrum bins to -90 dB floor so UI
  // decays to standby
  ctrl.setMasterMute(true);
  const QVariantList mutedBins = ctrl.getSpectrumBins(0, 160);
  QCOMPARE(mutedBins.size(), 160);
  double mutedMax = -100.0;
  for (int b = 0; b < mutedBins.size(); ++b) {
    mutedMax = std::max(mutedMax, mutedBins[b].toDouble());
  }
  QVERIFY2(mutedMax <= -89.9, qPrintable(QString("mutedMax=%1").arg(mutedMax)));
  ctrl.setMasterMute(false);
}

void TestPhase3ExternalInsert::
    testExternalInsertSendReturnTelemetryAndStatusPolling() {
  EngineController &ctrl = EngineController::instance();
  ctrl.insertNode()->bridge().setRequireLiveTransport(false);
  QVERIFY(ctrl.configureExternalInsert("TelemetryMock", "inL", "inR", "outL",
                                       "outR", 48000.0, 256, 0));
  ctrl.setExternalInsertBypass(false);

  ctrl.insertNode()->bridge().setMockProcessor(
      [](double *l, double *r, size_t n) {
        for (size_t i = 0; i < n; ++i) {
          l[i] *= 0.5;
          r[i] *= 0.5;
        }
      });

  QSignalSpy statusSpy(&ctrl, &EngineController::externalInsertStatusChanged);
  std::vector<double> l(256, 0.8), r(256, 0.8);
  ctrl.insertNode()->bridge().process(l.data(), r.data(), 256);

  QCOMPARE(ctrl.externalInsertStateText(), QString("Active"));
  QVERIFY(std::abs(ctrl.externalInsertSendPeak() - 0.8) < 1e-4);
  QVERIFY(std::abs(ctrl.externalInsertReturnPeak() - 0.4) < 1e-4);

  // 33ms dspTimer automatically polls Connected -> Active transition cleanly
  QTRY_VERIFY_WITH_TIMEOUT(statusSpy.count() >= 1, 1000);

  ctrl.disconnectExternalInsert();
  ctrl.insertNode()->bridge().setMockProcessor(nullptr);
}

QTEST_GUILESS_MAIN(TestPhase3ExternalInsert)
#include "test_phase3_external_insert.moc"
