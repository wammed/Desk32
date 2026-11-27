#include <QtTest/QtTest>
#include <QTemporaryDir>

#include "core/EngineController.hpp"
#include "core/SettingsManager.hpp"
#include "core/log/LogService.hpp"
#include "engine/preset/BuiltInDspPresets.hpp"
#include "engine/preset/PresetManager.hpp"
#include "engine/preset/PresetStateTracker.hpp"
#include "engine/render/RenderEngine.hpp"
#include "engine/render/RenderSequenceManager.hpp"
#include "engine/workspace/WorkspaceManager.hpp"

#include <memory>

using namespace desk32::core;
using namespace desk32::engine::preset;

class TestDspPresetState : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testPureTrackerTransitions();
    void testControllerFullStateTransitions();
    void testNodeBypassDoesNotChangeIndicatorsButUpdatesModified();
    void testRevertManualEditClearsModifiedWhileIndicatorsStayOff();
    void testExclusionsDoNotAffectIndicatorsOrModified();

private:
    std::unique_ptr<QTemporaryDir> m_tempDir;
};

void TestDspPresetState::initTestCase() {
    m_tempDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tempDir->isValid());
    desk32::core::log::LogService::instance().start(m_tempDir->filePath(QStringLiteral("test_dsp_preset_state_log.db")));
    PresetManager::instance().setCustomRootPath(m_tempDir->filePath(QStringLiteral("presets_state")));
    desk32::engine::workspace::WorkspaceManager::instance().setConfigFilePathForTesting(
        m_tempDir->filePath(QStringLiteral("workspaces.json")));
    desk32::engine::workspace::WorkspaceManager::instance().resetForTesting();
}

void TestDspPresetState::cleanupTestCase() {
    desk32::engine::render::RenderEngine::instance().stop();
    desk32::engine::workspace::WorkspaceManager::instance().resetForTesting();
    desk32::engine::render::RenderSequenceManager::instance().close();
    desk32::core::log::LogService::instance().stop();
    SettingsManager::instance().resetForTesting();
    m_tempDir.reset();
}

void TestDspPresetState::testPureTrackerTransitions() {
    PresetStateTracker tracker;
    const auto vocal = BuiltInDspPresets::allPresets().at(1); // Vocal Forward
    GlobalDspPresetData cur = vocal.global;

    // 1. Global apply -> all 7 indicators ON, modified = false
    tracker.onGlobalPresetApplied(vocal.id, vocal.name, vocal.global, cur);
    QCOMPARE(tracker.activeGlobalName(), QStringLiteral("Vocal Forward"));
    QCOMPARE(tracker.isGlobalModified(), false);
    for (size_t i = 0; i < kDspPresetTargetCount; ++i) {
        QVERIFY(tracker.isDspPresetApplied(static_cast<DspPresetTarget>(i)));
    }

    // 2. Manual parameter edit -> all 7 indicators OFF, Global name retained, modified = true
    cur.compressor.thresholdDb = -20.0;
    tracker.onManualParameterChanged(cur);
    QCOMPARE(tracker.activeGlobalName(), QStringLiteral("Vocal Forward"));
    QCOMPARE(tracker.isGlobalModified(), true);
    for (size_t i = 0; i < kDspPresetTargetCount; ++i) {
        QVERIFY(!tracker.isDspPresetApplied(static_cast<DspPresetTarget>(i)));
    }

    // 3. Per-DSP GEQ apply -> ONLY GEQ ON, other 6 OFF, Global name retained, modified = true
    cur.geq.gainsDb[10] = 2.0;
    tracker.onPerDspPresetApplied(DspPresetTarget::Geq, cur);
    QCOMPARE(tracker.activeGlobalName(), QStringLiteral("Vocal Forward"));
    QCOMPARE(tracker.isGlobalModified(), true);
    for (size_t i = 0; i < kDspPresetTargetCount; ++i) {
        const auto t = static_cast<DspPresetTarget>(i);
        QCOMPARE(tracker.isDspPresetApplied(t), t == DspPresetTarget::Geq);
    }

    // 4. Node ON/OFF toggle -> indicators MUST NOT change, modified updated
    cur = vocal.global;
    tracker.onGlobalPresetApplied(vocal.id, vocal.name, vocal.global, cur);
    QCOMPARE(tracker.isGlobalModified(), false);
    cur.setNodeEnabled(DspPresetTarget::Reverb, true);
    tracker.onDspNodeBypassToggled(cur);
    QCOMPARE(tracker.isGlobalModified(), true);
    for (size_t i = 0; i < kDspPresetTargetCount; ++i) {
        QVERIFY(tracker.isDspPresetApplied(static_cast<DspPresetTarget>(i)));
    }
}

void TestDspPresetState::testControllerFullStateTransitions() {
    EngineController ctrl;
    QVERIFY(ctrl.initialize(true));

    const QStringList targets = {
        QStringLiteral("peq"), QStringLiteral("geq"), QStringLiteral("hpf"),
        QStringLiteral("lpf"), QStringLiteral("compressor"), QStringLiteral("limiter"),
        QStringLiteral("reverb")
    };

    // 1. Apply Global "Rock / Pop" -> all 7 indicators ON, modified = false
    QVERIFY(ctrl.applyGlobalDspPreset(QStringLiteral("Rock / Pop")));
    QCOMPARE(ctrl.activeGlobalPresetName(), QStringLiteral("Rock / Pop"));
    QCOMPARE(ctrl.isGlobalPresetModified(), false);
    for (const auto& t : targets) {
        QVERIFY2(ctrl.isDspPresetApplied(t), qPrintable(t));
    }

    // 2. Manual GEQ band tweak -> all 7 indicators OFF, Global name kept, modified = true
    ctrl.setGeqBandGain(18, 2.5);
    QCOMPARE(ctrl.activeGlobalPresetName(), QStringLiteral("Rock / Pop"));
    QCOMPARE(ctrl.isGlobalPresetModified(), true);
    for (const auto& t : targets) {
        QVERIFY2(!ctrl.isDspPresetApplied(t), qPrintable(t));
    }

    // 3. Apply Per-DSP Limiter "Broadcast" -> ONLY limiter ON, other 6 OFF, Global name kept
    QVERIFY(ctrl.applyPerDspPreset(QStringLiteral("limiter"), QStringLiteral("Broadcast")));
    QCOMPARE(ctrl.activeGlobalPresetName(), QStringLiteral("Rock / Pop"));
    QCOMPARE(ctrl.isGlobalPresetModified(), true);
    for (const auto& t : targets) {
        QCOMPARE(ctrl.isDspPresetApplied(t), t == QLatin1String("limiter"));
    }

    // 4. Apply Per-DSP PEQ "BASS BOOST" -> ONLY peq ON, limiter now OFF
    QVERIFY(ctrl.applyPerDspPreset(QStringLiteral("peq"), QStringLiteral("BASS BOOST")));
    for (const auto& t : targets) {
        QCOMPARE(ctrl.isDspPresetApplied(t), t == QLatin1String("peq"));
    }
}

void TestDspPresetState::testNodeBypassDoesNotChangeIndicatorsButUpdatesModified() {
    EngineController ctrl;
    QVERIFY(ctrl.initialize(true));

    QVERIFY(ctrl.applyGlobalDspPreset(QStringLiteral("Chamber")));
    QCOMPARE(ctrl.isGlobalPresetModified(), false);
    QVERIFY(ctrl.isDspPresetApplied(QStringLiteral("peq")));
    QVERIFY(ctrl.isDspPresetApplied(QStringLiteral("reverb")));

    // Toggle Compressor node ON (was OFF in Chamber)
    ctrl.setDspNodeBypass(QStringLiteral("Compressor"), false);
    QCOMPARE(ctrl.isGlobalPresetModified(), true);
    // All 7 indicators remain ON per §11.1
    QVERIFY(ctrl.isDspPresetApplied(QStringLiteral("peq")));
    QVERIFY(ctrl.isDspPresetApplied(QStringLiteral("compressor")));
    QVERIFY(ctrl.isDspPresetApplied(QStringLiteral("reverb")));

    // Toggle Compressor node back OFF -> modified returns to false
    ctrl.setDspNodeBypass(QStringLiteral("Compressor"), true);
    QCOMPARE(ctrl.isGlobalPresetModified(), false);

    // Toggle HPF node ON via setHpfNodeEnabled -> indicators stay ON, modified becomes true
    ctrl.setHpfNodeEnabled(true);
    QCOMPARE(ctrl.isGlobalPresetModified(), true);
    QVERIFY(ctrl.isDspPresetApplied(QStringLiteral("hpf")));
    ctrl.setHpfNodeEnabled(false);
    QCOMPARE(ctrl.isGlobalPresetModified(), false);
}

void TestDspPresetState::testRevertManualEditClearsModifiedWhileIndicatorsStayOff() {
    EngineController ctrl;
    QVERIFY(ctrl.initialize(true));

    QVERIFY(ctrl.applyGlobalDspPreset(QStringLiteral("Orchestra Hall")));
    QCOMPARE(ctrl.isGlobalPresetModified(), false);
    QVERIFY(ctrl.isDspPresetApplied(QStringLiteral("limiter")));

    // Manually change Limiter ceiling from -1.5 to -2.0 -> indicators OFF, modified = true
    ctrl.setLimiterParams(-2.0, 150.0, 1.0);
    QVERIFY(!ctrl.isDspPresetApplied(QStringLiteral("limiter")));
    QCOMPARE(ctrl.isGlobalPresetModified(), true);

    // Manually revert Limiter ceiling back to -1.5 -> modified becomes false, indicators STAY OFF
    ctrl.setLimiterParams(-1.5, 150.0, 1.0);
    QVERIFY(!ctrl.isDspPresetApplied(QStringLiteral("limiter")));
    QCOMPARE(ctrl.isGlobalPresetModified(), false);
}

void TestDspPresetState::testExclusionsDoNotAffectIndicatorsOrModified() {
    EngineController ctrl;
    QVERIFY(ctrl.initialize(true));

    QVERIFY(ctrl.applyGlobalDspPreset(QStringLiteral("Big Band")));
    QCOMPARE(ctrl.isGlobalPresetModified(), false);
    QVERIFY(ctrl.isDspPresetApplied(QStringLiteral("geq")));

    // 1. Listening Correction preset & band edit must not touch chain indicators or modified
    ctrl.setListeningPreset(QStringLiteral("Open-Back Headphone"));
    ctrl.setListeningBand(false, 2, 120.0, 2.0, 1.2, 0, true);
    QCOMPARE(ctrl.isGlobalPresetModified(), false);
    QVERIFY(ctrl.isDspPresetApplied(QStringLiteral("peq")));
    QVERIFY(ctrl.isDspPresetApplied(QStringLiteral("geq")));

    // 2. GEQ Global Bypass (temporary bypass) must not touch indicators or modified
    ctrl.setGeqGlobalBypass(true);
    QCOMPARE(ctrl.isGlobalPresetModified(), false);
    QVERIFY(ctrl.isDspPresetApplied(QStringLiteral("geq")));
    ctrl.setGeqGlobalBypass(false);
    QCOMPARE(ctrl.isGlobalPresetModified(), false);

    // 3. Trim & Master controls must not touch indicators or modified
    ctrl.setDspTrim(QStringLiteral("geq"), false, 3.0);
    ctrl.setDspTrim(QStringLiteral("limiter"), true, -2.0);
    ctrl.setMasterGain(0.5);
    ctrl.setMasterMute(true);
    ctrl.setOutputRoutingMode(1);
    QCOMPARE(ctrl.isGlobalPresetModified(), false);
    QVERIFY(ctrl.isDspPresetApplied(QStringLiteral("geq")));
    QVERIFY(ctrl.isDspPresetApplied(QStringLiteral("limiter")));
}

QTEST_MAIN(TestDspPresetState)
#include "test_dsp_preset_state.moc"
