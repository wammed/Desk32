#include <QTest>
#include <QObject>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <cmath>
#include <vector>
#include <array>
#include <atomic>
#include <cstdlib>

#include "engine/insert/DelayCompensationBuffer.hpp"
#include "engine/insert/ExternalInsertBridge.hpp"
#include "engine/insert/ExternalInsertNode.hpp"
#include "engine/preset/PresetManager.hpp"
#include "core/EngineController.hpp"
#include "core/log/LogService.hpp"

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

void* operator new(size_t size) {
    if (s_trackAllocations.load(std::memory_order_relaxed) && t_isAudioThread) {
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void* operator new(size_t size, const std::nothrow_t&) noexcept {
    if (s_trackAllocations.load(std::memory_order_relaxed) && t_isAudioThread) {
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void* operator new[](size_t size) {
    if (s_trackAllocations.load(std::memory_order_relaxed) && t_isAudioThread) {
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    if (s_trackAllocations.load(std::memory_order_relaxed) && t_isAudioThread) {
        s_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

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

    // 7. Real-time safety: Strictly zero dynamic memory allocation in process loop
    void testZeroDynamicAllocationInInsertNode();

    // 8. PresetManager: XDG Base Directory, category isolation, factory presets, atomic save/load
    void testPresetManagerXdgPathsAndSerialization();

    // 9. EngineController: Integration of External Insert and Preset APIs
    void testEngineControllerIntegration();

    // 10. EngineController: DSP Parameter Preview & Master Control APIs (Section 5.9 & 10.2.3)
    void testEngineControllerDspPreviewAndMasterControl();

    // 11. P1-1: Global DSP Bypass Queue Consistency, Atomic Push, and Rollback
    void testGlobalDspBypassQueueConsistencyAndRollback();

    // 12. P1-2: Individual DSP Node Bypass Queue Push Failure, Consistency, and Parameter Preservation
    void testDspNodeBypassQueueFailureAndConsistency();

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
        "CarlaHost",
        "carla:in_l", "carla:in_r",
        "carla:out_l", "carla:out_r",
        48000.0, 256, 128
    };
    QVERIFY(node.bridge().configure(config, 48000.0, 256));
    node.setBypassed(false);
    QVERIFY(!node.isBypassed());

    // Attach mock processor simulating external plugin inversion
    node.bridge().setMockProcessor([](double* l, double* r, size_t frames) {
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
    left[0] = 1.0;  // Impulse at frame 0
    right[0] = 1.0;

    delayBuffer.process(left.data(), right.data(), 256);
    // At output, frame 0 should be 0.0, and impulse should emerge at index 64
    QCOMPARE(left[0], 0.0);
    QCOMPARE(right[0], 0.0);
    QCOMPARE(left[64], 1.0);
    QCOMPARE(right[64], 1.0);
    QCOMPARE(left[65], 0.0);
    QCOMPARE(right[65], 0.0);

    // Test delay across block boundaries (e.g. 300 frames delay with 256 frame blocks)
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
    ExternalPortInfo directFeedback{
        "CarlaHost",
        "desk32:send_l", "desk32:send_r",
        "desk32:send_l", "desk32:return_r",
        48000.0, 256, 0
    };

    bool ok = bridge.configure(directFeedback, 48000.0, 256);
    QVERIFY(!ok);
    QCOMPARE(bridge.state(), InsertState::Error);
    QVERIFY(bridge.isBypassed());
    QVERIFY(bridge.lastError().find("Feedback loop detected") != std::string::npos);

    // Direct feedback on Right channel
    ExternalPortInfo rightFeedback{
        "CarlaHost",
        "desk32:send_l", "desk32:send_r",
        "desk32:return_l", "desk32:send_r",
        48000.0, 256, 0
    };
    ExternalInsertBridge rightBridge;
    QVERIFY(!rightBridge.configure(rightFeedback, 48000.0, 256));
    QCOMPARE(rightBridge.state(), InsertState::Error);
    QVERIFY(rightBridge.isBypassed());
}

void TestPhase3ExternalInsert::testRateAndBufferMismatchRejection() {
    ExternalInsertBridge bridge;

    // Sample rate mismatch: engine at 48000 Hz, external host at 96000 Hz
    ExternalPortInfo rateMismatchConfig{
        "CarlaHost",
        "carla:in_l", "carla:in_r",
        "carla:out_l", "carla:out_r",
        96000.0, 256, 64
    };
    bool okRate = bridge.configure(rateMismatchConfig, 48000.0, 256);
    QVERIFY(!okRate);
    QCOMPARE(bridge.state(), InsertState::RateMismatch);
    QVERIFY(bridge.isBypassed());
    QVERIFY(bridge.lastError().find("Sample rate mismatch") != std::string::npos);

    // Buffer size mismatch: engine at 256, external host at 512
    ExternalPortInfo bufferMismatchConfig{
        "CarlaHost",
        "carla:in_l", "carla:in_r",
        "carla:out_l", "carla:out_r",
        48000.0, 512, 64
    };
    ExternalInsertBridge bufBridge;
    bool okBuf = bufBridge.configure(bufferMismatchConfig, 48000.0, 256);
    QVERIFY(!okBuf);
    QCOMPARE(bufBridge.state(), InsertState::BufferMismatch);
    QVERIFY(bufBridge.isBypassed());
    QVERIFY(bufBridge.lastError().find("Buffer size mismatch") != std::string::npos);
}

void TestPhase3ExternalInsert::testDisconnectRecoverySafety() {
    ExternalInsertBridge bridge;
    ExternalPortInfo config{
        "CarlaHost",
        "carla:in_l", "carla:in_r",
        "carla:out_l", "carla:out_r",
        48000.0, 256, 64
    };
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
    ExternalPortInfo config{
        "LSP_Limiter",
        "lsp:in_l", "lsp:in_r",
        "lsp:out_l", "lsp:out_r",
        48000.0, 256, reportedLatency
    };
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
        "CarlaHost",
        "carla:in_l", "carla:in_r",
        "carla:out_l", "carla:out_r",
        48000.0, 256, 128
    };
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

    PresetManager& pm = PresetManager::instance();
    pm.setCustomRootPath(tempDir.path() + "/presets");

    // 1. Factory Presets Verification
    QStringList lcPresets = pm.listPresets(PresetCategories::ListeningCorrection);
    QVERIFY(lcPresets.contains("Monitor Flat"));
    QVERIFY(lcPresets.contains("Harman Target"));
    QVERIFY(pm.isFactoryPreset(PresetCategories::ListeningCorrection, "Monitor Flat"));

    QJsonObject flatJson = pm.loadPreset(PresetCategories::ListeningCorrection, "Monitor Flat");
    QVERIFY(!flatJson.isEmpty());
    QCOMPARE(flatJson["name"].toString(), QString("Monitor Flat"));
    QCOMPARE(flatJson["is_factory"].toBool(), true);

    // Attempting to delete or overwrite factory preset must fail
    QVERIFY(!pm.deletePreset(PresetCategories::ListeningCorrection, "Monitor Flat"));
    QJsonObject dummy;
    QVERIFY(!pm.savePreset(PresetCategories::ListeningCorrection, "Monitor Flat", dummy, true));

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

    QVERIFY(pm.savePreset(PresetCategories::ExternalInsert, "My Studio Carla", customInsert));
    QVERIFY(pm.listPresets(PresetCategories::ExternalInsert).contains("My Studio Carla"));

    QJsonObject loaded = pm.loadPreset(PresetCategories::ExternalInsert, "My Studio Carla");
    QCOMPARE(loaded["client_name"].toString(), QString("CustomCarla"));
    QCOMPARE(loaded["reported_latency_frames"].toInt(), 96);
    QCOMPARE(loaded["is_factory"].toBool(), false);

    // 3. Category Isolation: Trying to load an external insert preset under listening correction must fail
    QJsonObject isolated = pm.loadPreset(PresetCategories::ListeningCorrection, "My Studio Carla");
    QVERIFY(isolated.isEmpty());

    // 4. Delete User Preset
    QVERIFY(pm.deletePreset(PresetCategories::ExternalInsert, "My Studio Carla"));
    QVERIFY(!pm.listPresets(PresetCategories::ExternalInsert).contains("My Studio Carla"));
    QVERIFY(pm.loadPreset(PresetCategories::ExternalInsert, "My Studio Carla").isEmpty());
}

void TestPhase3ExternalInsert::testEngineControllerIntegration() {
    EngineController& controller = EngineController::instance();
    QVERIFY(controller.initialize());

    QSignalSpy insertSpy(&controller, &EngineController::externalInsertStatusChanged);
    QSignalSpy presetSpy(&controller, &EngineController::presetManagementChanged);

    // 1. Configure External Insert
    bool configOk = controller.configureExternalInsert(
        "CarlaHost",
        "carla:audio_in_1", "carla:audio_in_2",
        "carla:audio_out_1", "carla:audio_out_2",
        48000.0, 256, 128
    );
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
    QVariantMap captured = controller.captureCurrentStateAsPreset("external_insert");
    QCOMPARE(captured["client_name"].toString(), QString("CarlaHost"));

    bool saveOk = controller.savePreset("external_insert", "SavedCarlaConfig", captured);
    QVERIFY(saveOk);
    QVERIFY(presetSpy.count() > 0);

    QVariantMap loaded = controller.loadPreset("external_insert", "SavedCarlaConfig");
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

void TestPhase3ExternalInsert::testEngineControllerDspPreviewAndMasterControl() {
    EngineController& controller = EngineController::instance();

    // 1. DSP Parameter Get/Set and signals
    QSignalSpy filterSpy(&controller, &EngineController::filterParametersChanged);
    QSignalSpy compSpy(&controller, &EngineController::compressorParametersChanged);
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

    // 2. Individual Bypass vs Global Bypass Non-Destructive Behavior (Section 10.2.3 & 5.9)
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

void TestPhase3ExternalInsert::testGlobalDspBypassQueueConsistencyAndRollback() {
    EngineController& controller = EngineController::instance();
    auto pipe = controller.pipeline();
    QVERIFY(pipe != nullptr);
    desk32::engine::dsp::DspCommand dummy;

    // Initial state: disengaged
    controller.setGlobalDspBypass(false);
    QCOMPARE(controller.globalDspBypass(), false);

    // Drain any pending commands
    while (pipe->paramQueue().pop(dummy)) {}

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
    while (pipe->paramQueue().pop(dummy)) {}

    // Case 2: Parameter Queue Full (512 items)
    // Fill queue to capacity so hasSpace(2) and push() fail
    for (size_t i = 0; i < desk32::engine::dsp::ParameterCommandQueue::CAPACITY; ++i) {
        desk32::engine::dsp::DspCommand fillCmd;
        fillCmd.type = desk32::engine::dsp::DspCommandType::None;
        pipe->paramQueue().push(fillCmd);
    }
    QCOMPARE(pipe->paramQueue().hasSpace(2), false);

    QSignalSpy overflowSpy(&controller, &EngineController::dspQueueOverflowWarning);
    bypassSpy.clear();

    // Attempt to engage Global Bypass with full queue
    controller.setGlobalDspBypass(true);

    // AC-01: UI and DSP state must NOT remain desynchronized; controller state remains false!
    QCOMPARE(controller.globalDspBypass(), false);
    QVERIFY(overflowSpy.count() > 0);

    // Case 3: Partial push prevention via hasSpace(2)
    // Clear queue, fill to CAPACITY - 1 (only 1 slot left)
    while (pipe->paramQueue().pop(dummy)) {}
    for (size_t i = 0; i < desk32::engine::dsp::ParameterCommandQueue::CAPACITY - 1; ++i) {
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
    // Verify that under concurrent push operations from multiple threads, pushTwo() ALWAYS enqueues
    // the Master and Listening commands consecutively without interleaving from other producers.
    {
        desk32::engine::dsp::ParameterCommandQueue testQueue;
        std::atomic<bool> start{false};
        std::atomic<bool> done{false};
        std::atomic<size_t> pushTwoSuccessCount{0};

        // 2 threads calling pushTwo
        std::vector<std::thread> producers;
        for (int t = 0; t < 2; ++t) {
            producers.emplace_back([&, t]() {
                while (!start.load(std::memory_order_acquire)) {}
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
                while (!start.load(std::memory_order_acquire)) {}
                for (int i = 0; i < 100; ++i) {
                    desk32::engine::dsp::DspCommand c;
                    c.type = desk32::engine::dsp::DspCommandType::None;
                    c.index = static_cast<uint8_t>(t * 100 + i);
                    testQueue.push(c);
                    std::this_thread::yield();
                }
            });
        }

        // Consumer thread popping and verifying that Master and Listening are ALWAYS consecutive
        std::atomic<bool> atomicityViolation{false};
        std::thread consumer([&]() {
            while (!done.load(std::memory_order_acquire)) {
                desk32::engine::dsp::DspCommand cmd;
                if (testQueue.pop(cmd)) {
                    if (cmd.type == desk32::engine::dsp::DspCommandType::SetGlobalBypass && cmd.subIndex == 0) {
                        desk32::engine::dsp::DspCommand nextCmd;
                        while (!testQueue.pop(nextCmd)) {
                            std::this_thread::yield();
                        }
                        if (nextCmd.type != desk32::engine::dsp::DspCommandType::SetGlobalBypass ||
                            nextCmd.subIndex != 1 ||
                            nextCmd.index != cmd.index) {
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
                if (cmd.type == desk32::engine::dsp::DspCommandType::SetGlobalBypass && cmd.subIndex == 0) {
                    desk32::engine::dsp::DspCommand nextCmd;
                    while (!testQueue.pop(nextCmd)) {
                        std::this_thread::yield();
                    }
                    if (nextCmd.type != desk32::engine::dsp::DspCommandType::SetGlobalBypass ||
                        nextCmd.subIndex != 1 ||
                        nextCmd.index != cmd.index) {
                        atomicityViolation.store(true, std::memory_order_relaxed);
                    }
                }
            }
        });

        start.store(true, std::memory_order_release);
        for (auto& p : producers) p.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        done.store(true, std::memory_order_release);
        consumer.join();

        QVERIFY(pushTwoSuccessCount.load() > 50);
        QCOMPARE(atomicityViolation.load(), false);
    }

    // Case 5: Queue drains and resync succeeds
    while (pipe->paramQueue().pop(dummy)) {}
    QCOMPARE(pipe->paramQueue().hasSpace(2), true);

    bool resyncOk = controller.resyncGlobalDspBypass();
    QCOMPARE(resyncOk, true);
    QCOMPARE(controller.isGlobalDspBypassDesynced(), false);

    // Clean up
    while (pipe->paramQueue().pop(dummy)) {}
    controller.setGlobalDspBypass(false);
    while (pipe->paramQueue().pop(dummy)) {}
}

void TestPhase3ExternalInsert::testDspNodeBypassQueueFailureAndConsistency() {
    EngineController& controller = EngineController::instance();
    auto pipe = controller.pipeline();
    QVERIFY(pipe != nullptr);
    desk32::engine::dsp::DspCommand dummy;

    // Initial state: ensure Filter is not bypassed (both Accepted and Applied in Audio Thread)
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

    // 2. Not yet Applied in Audio Thread (pipeline has not drained parameter command queue yet)
    QCOMPARE(controller.isDspNodeAppliedBypassed("Filter"), false);

    // 3. Audio Thread cycle runs (renderOfflineBlock drains parameter queue into active DSP nodes)
    std::vector<float> pcm(256 * 2, 0.0f);
    controller.renderOfflineBlock(pcm.data(), 256);

    // 4. Applied: DSP node in Audio Thread now confirms bypass engaged
    QCOMPARE(controller.isDspNodeAppliedBypassed("Filter"), true);

    // Toggle back to false
    filterSpy.clear();
    controller.setDspNodeBypass("Filter", false);
    QCOMPARE(controller.isDspNodeBypassed("Filter"), false); // Accepted
    QCOMPARE(controller.isDspNodeAppliedBypassed("Filter"), true); // Still true until Audio Thread runs
    controller.renderOfflineBlock(pcm.data(), 256); // Audio Thread cycle
    QCOMPARE(controller.isDspNodeAppliedBypassed("Filter"), false); // Applied
    QVERIFY(filterSpy.count() > 0);

    // Case 2: Parameter Queue Full -> Failure handling
    for (size_t i = 0; i < desk32::engine::dsp::ParameterCommandQueue::CAPACITY; ++i) {
        desk32::engine::dsp::DspCommand fillCmd;
        fillCmd.type = desk32::engine::dsp::DspCommandType::None;
        pipe->paramQueue().push(fillCmd);
    }

    QSignalSpy overflowSpy(&controller, &EngineController::dspQueueOverflowWarning);
    filterSpy.clear();

    // Attempt to bypass Filter while queue is full
    controller.setDspNodeBypass("Filter", true);

    // AC-04: UI and DSP state must remain consistent on failure: state must stay false!
    QCOMPARE(controller.isDspNodeBypassed("Filter"), false);
    QCOMPARE(controller.isDspNodeAppliedBypassed("Filter"), false);
    QVERIFY(overflowSpy.count() > 0);
    // UI signal re-emitted so UI reverts to false
    QVERIFY(filterSpy.count() > 0);

    // Drain queue
    while (pipe->paramQueue().pop(dummy)) {}

    // Case 3: Resynchronization
    bool resyncOk = controller.resyncDspNodeBypass("Filter");
    QCOMPARE(resyncOk, true);
    while (pipe->paramQueue().pop(dummy)) {}

    // Case 4: Node bypass does not affect Global DSP Bypass
    controller.setGlobalDspBypass(false);
    controller.setDspNodeBypass("Compressor", true);
    QCOMPARE(controller.isDspNodeBypassed("Compressor"), true);
    QCOMPARE(controller.globalDspBypass(), false);

    controller.setDspNodeBypass("Compressor", false);
    while (pipe->paramQueue().pop(dummy)) {}
}

QTEST_GUILESS_MAIN(TestPhase3ExternalInsert)
#include "test_phase3_external_insert.moc"
