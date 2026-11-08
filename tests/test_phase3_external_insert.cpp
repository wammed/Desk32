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
using namespace desk32::core;

// Real-time heap allocation tracking for Audio Thread safety verification
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

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

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

    // Enable heap allocation tracking
    s_allocationCount.store(0, std::memory_order_relaxed);
    s_trackAllocations.store(true, std::memory_order_release);

    // Execute 1,000 real-time audio blocks
    for (size_t block = 0; block < 1000; ++block) {
        node.process(left.data(), right.data(), 256);
    }

    s_trackAllocations.store(false, std::memory_order_release);
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

QTEST_GUILESS_MAIN(TestPhase3ExternalInsert)
#include "test_phase3_external_insert.moc"
