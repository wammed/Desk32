#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QVariantMap>

#include "core/EngineController.hpp"
#include "core/LanguageManager.hpp"
#include "core/SettingsManager.hpp"
#include "core/log/LogService.hpp"
#include "engine/backend/AudioControlManager.hpp"
#include "engine/backend/JackAudioOutput.hpp"
#include "engine/backend/JackControlAdapter.hpp"
#include "engine/insert/ExternalInsertBridge.hpp"
#include "engine/insert/JackInsertTransport.hpp"
#include "engine/pipeline/AudioPipeline.hpp"
#include "engine/preset/PresetManager.hpp"
#include "engine/render/RenderEngine.hpp"
#include "engine/render/RenderSequenceManager.hpp"
#include "engine/workspace/WorkspaceManager.hpp"

using namespace desk32::engine::backend;
using namespace desk32::engine::insert;
using namespace desk32::engine::pipeline;
using desk32::core::EngineController;
using desk32::core::SettingsManager;
using desk32::core::log::LogService;
using desk32::engine::preset::PresetManager;
using desk32::engine::render::RenderEngine;
using desk32::engine::render::RenderSequenceManager;
using desk32::engine::workspace::WorkspaceManager;

class TestJackOutput : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void testJackAudioOutputAndCapabilities();
    void testJackInsertTransportStrictLiveAndSimulation();
    void testEngineControllerBackendSwitchingAndUiPanel();

private:
    std::unique_ptr<QTemporaryDir> m_tempDir;
};

void TestJackOutput::initTestCase()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    m_tempDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tempDir->isValid());
    LogService::instance().start(m_tempDir->path() + "/test_jack_output_log.db");
    PresetManager::instance().setCustomRootPath(m_tempDir->path() + "/presets");
    WorkspaceManager::instance().setConfigFilePathForTesting(m_tempDir->path() + "/workspaces.json");
    WorkspaceManager::instance().resetForTesting();
}

void TestJackOutput::cleanupTestCase()
{
    if (auto* out = EngineController::instance().audioOutput()) {
        out->shutdown();
    }
    RenderEngine::instance().stop();
    WorkspaceManager::instance().resetForTesting();
    RenderSequenceManager::instance().close();
    LogService::instance().stop();
    SettingsManager::instance().resetForTesting();
    m_tempDir.reset();
}

void TestJackOutput::testJackAudioOutputAndCapabilities()
{
    JackAudioOutput output;
    AudioControlManager mgr(std::make_unique<JackControlAdapter>(&output));

    // Pre-init capabilities: honest Unavailable + Unsupported (never fake Supported)
    const auto preInit = mgr.queryCapabilities(false, false);
    QCOMPARE(preInit.backendName, std::string("JACK"));
    QCOMPARE(preInit.exclusiveAccess, CapabilityAvailability::Unsupported);
    QCOMPARE(preInit.formatSelection, CapabilityAvailability::Unsupported);
    QCOMPARE(preInit.hardwareBitPerfect, CapabilityAvailability::Unknown);
    QCOMPARE(preInit.directMode, CapabilityAvailability::Unavailable);
    QCOMPARE(preInit.sampleRateFollowing, CapabilityAvailability::Unavailable);
    QCOMPARE(preInit.deviceSwitching, CapabilityAvailability::Unavailable);
    QVERIFY(!preInit.formatSelectionReason.empty());
    QVERIFY(!preInit.directModeReason.empty());

    // Offline simulation verification
    output.setOfflineSimulationMode(true);
    AudioOutputDeviceInfo jackSink1;
    jackSink1.nodeId = 1001;
    jackSink1.name = "Built-in Audio Analog Stereo";
    jackSink1.description = "Built-in Audio Analog Stereo (JACK Physical Sink)";
    jackSink1.mediaClass = "JACK/PhysicalSink";
    jackSink1.isDefault = true;
    jackSink1.sampleRate = 48000;
    jackSink1.channels = 2;
    jackSink1.nativeFormat = OutputPcmFormat::F32LE;

    AudioOutputDeviceInfo jackSink2;
    jackSink2.nodeId = 1002;
    jackSink2.name = "USB Reference DAC";
    jackSink2.description = "USB Reference DAC (JACK Physical Sink)";
    jackSink2.mediaClass = "JACK/PhysicalSink";
    jackSink2.sampleRate = 96000;
    jackSink2.channels = 2;
    jackSink2.nativeFormat = OutputPcmFormat::F32LE;

    output.setSimulatedOutputDevices({jackSink1, jackSink2});
    QVERIFY(output.initialize(48000, 256));
    QVERIFY(output.start());

    // JACK only supports F32LE
    QVERIFY(mgr.setPreferredPcmFormat(OutputPcmFormat::F32LE));
    QVERIFY(!mgr.setPreferredPcmFormat(OutputPcmFormat::S24_32LE));
    QVERIFY(!mgr.setPreferredPcmFormat(OutputPcmFormat::S16LE));
    QCOMPARE(output.negotiatedFormat(), OutputPcmFormat::F32LE);

    // Device enumeration & switching
    const auto devices = mgr.enumerateOutputDevices();
    QCOMPARE(devices.size(), size_t(2));
    QVERIFY(mgr.selectOutputDevice("USB Reference DAC"));
    QCOMPARE(mgr.currentTargetDevice(), std::string("USB Reference DAC"));
    std::string errMsg;
    QVERIFY(!mgr.selectOutputDevice("Missing_JACK_Port", &errMsg));
    QVERIFY(!errMsg.empty());

    // Quantum / buffer size update
    QVERIFY(mgr.applyLatencySetting(512, 48000.0));
    QCOMPARE(output.diagnostics().negotiatedQuantum, uint32_t(512));

    // Sample rate renegotiation in simulation
    double silenceMs = 0.0;
    QVERIFY(mgr.renegotiateSampleRate(96000, silenceMs));
    QCOMPARE(output.diagnostics().sampleRate, uint32_t(96000));
    const auto activeCaps = mgr.queryCapabilities(true, true);
    QCOMPARE(activeCaps.directMode, CapabilityAvailability::Supported);
    QCOMPARE(activeCaps.sampleRateFollowing, CapabilityAvailability::Supported);
    QCOMPARE(activeCaps.formatSelection, CapabilityAvailability::Unsupported);

    // Render callback via AudioPipeline
    AudioPipeline pipeline(96000.0);
    output.setRawPcmAudioCallback([&](void* outBuf, size_t nFrames, OutputPcmFormat fmt) {
        QCOMPARE(fmt, OutputPcmFormat::F32LE);
        std::vector<double> l(nFrames, 0.25), r(nFrames, -0.25);
        pipeline.process(l.data(), r.data(), nFrames, outBuf, SampleFormat::Float32, false);
    });
    std::vector<float> pcm(512 * 2, 0.0f);
    OutputPcmFormat usedFmt = OutputPcmFormat::S16LE;
    output.renderOfflineRawBlock(pcm.data(), 512, &usedFmt);
    QCOMPARE(usedFmt, OutputPcmFormat::F32LE);
    QCOMPARE(pcm[0], 0.25f);
    QCOMPARE(pcm[1], -0.25f);

    output.stop();
    output.shutdown();
    QCOMPARE(output.state(), StreamState::Unconnected);
}

void TestJackOutput::testJackInsertTransportStrictLiveAndSimulation()
{
    // 1. Live mode with no matching target must NEVER fake Connected (§9 false-positive lesson)
    auto liveTr = std::make_shared<JackInsertTransport>();
    liveTr->setSimulationMode(false);
    ExternalInsertBridge strictBridge;
    strictBridge.setTransport(liveTr);
    strictBridge.setRequireLiveTransport(true);

    ExternalPortInfo bogusCfg{"NonExistentJackHost_999", "nx:in_1", "nx:in_2", "nx:out_1", "nx:out_2", 48000.0, 256, 0};
    QVERIFY(!strictBridge.configure(bogusCfg, 48000.0, 256));
    QVERIFY(strictBridge.state() != InsertState::Connected && strictBridge.state() != InsertState::Active);
    QVERIFY(strictBridge.isBypassed());
    QVERIFY(!strictBridge.isTransportLiveConnected());

    // 2. Simulation loopback, disconnect auto-bypass, and rate/buffer mismatch handling
    auto simTr = std::make_shared<JackInsertTransport>();
    simTr->setSimulationMode(true);
    simTr->setSimulationLoopbackLatencyFrames(64);

    ExternalInsertBridge simBridge;
    simBridge.setTransport(simTr);
    simBridge.setRequireLiveTransport(true);
    ExternalPortInfo okCfg{"CarlaJack", "CarlaJack:in_1", "CarlaJack:in_2", "CarlaJack:out_1", "CarlaJack:out_2", 48000.0, 256, 0, false};
    QVERIFY(simBridge.configure(okCfg, 48000.0, 256));
    QVERIFY(simBridge.isTransportLiveConnected());
    QCOMPARE(simBridge.transportDiagnostics().activeLinksCreated, uint32_t(4));
    QCOMPARE(simBridge.reportedLatencyText(), std::string("Not Reported"));

    simBridge.triggerLatencyMeasurement();
    std::vector<double> l(256, 0.0), r(256, 0.0);
    simBridge.process(l.data(), r.data(), 256);
    QCOMPARE(simBridge.measuredLatencyFrames(), int64_t(64));

    // Port disappeared / client quit -> Disconnected + auto-bypass
    simTr->simulateGraphNodeRemoved();
    std::fill(l.begin(), l.end(), 0.2);
    std::fill(r.begin(), r.end(), 0.2);
    simBridge.process(l.data(), r.data(), 256);
    QCOMPARE(simBridge.state(), InsertState::Disconnected);
    QVERIFY(simBridge.isBypassed());

    // Rate & buffer mismatches
    QVERIFY(simBridge.configure(okCfg, 48000.0, 256));
    simTr->simulateRateMismatch(96000);
    simBridge.process(l.data(), r.data(), 256);
    QCOMPARE(simBridge.state(), InsertState::RateMismatch);
    QVERIFY(simBridge.isBypassed());

    QVERIFY(simBridge.configure(okCfg, 48000.0, 256));
    simTr->simulateBufferMismatch(512);
    simBridge.process(l.data(), r.data(), 256);
    QCOMPARE(simBridge.state(), InsertState::BufferMismatch);
    QVERIFY(simBridge.isBypassed());
}

void TestJackOutput::testEngineControllerBackendSwitchingAndUiPanel()
{
    auto& sm = SettingsManager::instance();
    const QString cfgPath = m_tempDir->path() + "/jack_settings/settings.json";
    sm.setConfigFilePathForTesting(cfgPath);
    QVERIFY(sm.load());
    QCOMPARE(sm.audioBackend(), QStringLiteral("PipeWire"));

    auto& ctrl = EngineController::instance();
    QVERIFY(ctrl.initialize(true));
    QCOMPARE(ctrl.availableAudioBackends(), QStringList({QStringLiteral("PipeWire"), QStringLiteral("JACK")}));
    QCOMPARE(ctrl.audioBackend(), QStringLiteral("PipeWire"));

    // Register QML singletons and verify OutputDevicePanel switches backend cleanly
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "SettingsManager", &sm);
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "LanguageManager", &LanguageManager::instance());
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "EngineController", &ctrl);
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "LogService", &LogService::instance());
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "WorkspaceManager", &WorkspaceManager::instance());

    QQmlEngine qmlEngine;
    QQmlComponent panelComp(&qmlEngine, QUrl::fromLocalFile(QStringLiteral(DESK32_SOURCE_DIR "/qml/OutputDevicePanel.qml")));
    QVERIFY2(panelComp.status() == QQmlComponent::Ready, qPrintable(panelComp.errorString()));
    std::unique_ptr<QObject> panel(panelComp.create());
    QVERIFY(panel != nullptr);

    QSignalSpy backendSpy(&ctrl, &EngineController::audioBackendChanged);
    QVERIFY(QMetaObject::invokeMethod(panel.get(), "switchBackend", Q_ARG(QVariant, QStringLiteral("JACK"))));
    QCOMPARE(ctrl.audioBackend(), QStringLiteral("JACK"));
    QVERIFY(backendSpy.count() >= 1);
    QCOMPARE(sm.audioBackend(), QStringLiteral("JACK"));
    QCOMPARE(ctrl.getAudioCapabilities().value("backend").toString(), QStringLiteral("JACK"));
    QCOMPARE(ctrl.getAudioCapabilities().value("formatSelection").toString(), QStringLiteral("Unsupported"));
    QCOMPARE(ctrl.outputPcmFormat(), QStringLiteral("F32LE"));

    // Verify External Insert calibration works under JACK backend in offline simulation
    ctrl.insertTransport()->setSimulationLoopbackLatencyFrames(32);
    QVERIFY(ctrl.configureExternalInsert("JackRack", "JackRack:in_1", "JackRack:in_2",
                                         "JackRack:out_1", "JackRack:out_2", 48000.0, 256, 0));
    std::vector<float> buf(256 * 2, 0.0f);
    ctrl.renderOfflineBlock(buf.data(), 256);
    QCOMPARE(ctrl.isDspNodeAppliedBypassed("ExternalInsert"), false);
    ctrl.calibrateInsertLatency();
    QCOMPARE(ctrl.externalInsertMeasuredLatency(), 32);
    QVERIFY(ctrl.confirmRecommendedInsertLatency(false));
    QCOMPARE(ctrl.insertDelayCompensation(), 32);

    // Switch back to PipeWire via AudioDevice preset JSON
    const QVariantMap snap = ctrl.captureCurrentStateAsPreset(desk32::engine::preset::PresetCategories::AudioDevice);
    QCOMPARE(snap.value("backend").toString(), QStringLiteral("JACK"));

    QVERIFY(QMetaObject::invokeMethod(panel.get(), "switchBackend", Q_ARG(QVariant, QStringLiteral("PipeWire"))));
    QCOMPARE(ctrl.audioBackend(), QStringLiteral("PipeWire"));
    QCOMPARE(sm.audioBackend(), QStringLiteral("PipeWire"));
    // External Insert state must cleanly disconnect rather than staying falsely Connected across backend switch
    QVERIFY(ctrl.externalInsertStateText() != QStringLiteral("Active") &&
            ctrl.externalInsertStateText() != QStringLiteral("Connected"));
}

QTEST_MAIN(TestJackOutput)
#include "test_jack_output.moc"
