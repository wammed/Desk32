#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <unistd.h>

#include "core/EngineController.hpp"
#include "core/LanguageManager.hpp"
#include "core/SettingsManager.hpp"
#include "core/log/LogService.hpp"
#include "engine/backend/AudioControlTypes.hpp"
#include "engine/backend/PipeWireAudioOutput.hpp"
#include "engine/preset/PresetManager.hpp"
#include "engine/render/RenderEngine.hpp"
#include "engine/render/RenderSequenceManager.hpp"
#include "engine/workspace/WorkspaceManager.hpp"

using desk32::core::EngineController;
using desk32::core::log::LogService;
using desk32::engine::backend::AudioOutputDeviceInfo;
using desk32::engine::backend::OutputPcmFormat;
using desk32::engine::preset::PresetManager;
using desk32::engine::render::RenderEngine;
using desk32::engine::render::RenderSequenceManager;
using desk32::engine::workspace::WorkspaceManager;

class TestOutputDeviceUi : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void testOutputDeviceEnumerationAndSelectionPanel();
    void testWorkspaceImportFailureUiDisplay();
    void testSettingsManagerPersistenceAndIntegration();
    void testDeckStateContractFolderSessionAndSeek();
    void testWorkspaceViewDeckSelectionLockAndFolderDiscard();
    void testMainQmlCompilesWithOutputDevicePanel();

private:
    std::unique_ptr<QTemporaryDir> m_tempDir;
    std::unique_ptr<QQmlEngine> m_qmlEngine;
};

void TestOutputDeviceUi::initTestCase()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    m_tempDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tempDir->isValid());
    LogService::instance().start(m_tempDir->path() + "/test_output_device_ui_log.db");
    PresetManager::instance().setCustomRootPath(m_tempDir->path() + "/presets");
    WorkspaceManager::instance().setConfigFilePathForTesting(m_tempDir->path() + "/workspaces.json");
    WorkspaceManager::instance().resetForTesting();

    auto& ctrl = EngineController::instance();
    QVERIFY(ctrl.initialize(true));

    AudioOutputDeviceInfo dac1;
    dac1.nodeId = 101;
    dac1.name = "alsa_output.usb-dac";
    dac1.description = "Reference USB DAC";
    dac1.mediaClass = "Audio/Sink";
    dac1.isDefault = true;
    dac1.sampleRate = 96000;
    dac1.channels = 2;
    dac1.nativeFormat = OutputPcmFormat::S24_32LE;

    AudioOutputDeviceInfo dac2;
    dac2.nodeId = 102;
    dac2.name = "alsa_output.pci-builtin";
    dac2.description = "Built-in Analog Stereo";
    dac2.mediaClass = "Audio/Sink";
    dac2.isDefault = false;
    dac2.sampleRate = 48000;
    dac2.channels = 2;
    dac2.nativeFormat = OutputPcmFormat::F32LE;

    ctrl.audioOutput()->setSimulatedOutputDevices({dac1, dac2});

    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "SettingsManager",
                                 &desk32::core::SettingsManager::instance());
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "LanguageManager",
                                 &LanguageManager::instance());
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "EngineController",
                                 &EngineController::instance());
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "LogService",
                                 &LogService::instance());
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "WorkspaceManager",
                                 &WorkspaceManager::instance());

    // Share one QQmlEngine across test slots (mirrors main.cpp and avoids repeated
    // QQmlThread creation/teardown across slots within a single test process).
    m_qmlEngine = std::make_unique<QQmlEngine>();
}

void TestOutputDeviceUi::cleanupTestCase()
{
    m_qmlEngine.reset();
    if (auto* out = EngineController::instance().audioOutput()) {
        out->shutdown();
    }
    RenderEngine::instance().stop();
    WorkspaceManager::instance().resetForTesting();
    RenderSequenceManager::instance().close();
    LogService::instance().stop();
    desk32::core::SettingsManager::instance().resetForTesting();
    m_tempDir.reset();
}

void TestOutputDeviceUi::testOutputDeviceEnumerationAndSelectionPanel()
{
    QVERIFY(m_qmlEngine != nullptr);
    const QUrl panelUrl = QUrl::fromLocalFile(
        QStringLiteral(DESK32_SOURCE_DIR "/qml/OutputDevicePanel.qml"));
    QQmlComponent component(m_qmlEngine.get(), panelUrl);
    if (component.isError()) {
        qWarning() << component.errors();
    }
    QVERIFY2(component.status() == QQmlComponent::Ready,
             qPrintable(component.errorString()));

    std::unique_ptr<QObject> panel(component.create());
    QVERIFY2(panel != nullptr, qPrintable(component.errorString()));

    // Verify initial enumeration populates both simulated sinks
    const QVariantList initialDevices = panel->property("devicesList").toList();
    QCOMPARE(initialDevices.size(), 2);
    QCOMPARE(initialDevices[0].toMap().value("name").toString(),
             QStringLiteral("alsa_output.usb-dac"));
    QCOMPARE(initialDevices[1].toMap().value("name").toString(),
             QStringLiteral("alsa_output.pci-builtin"));
    QCOMPARE(panel->property("isDefaultTargetSelected").toBool(), true);

    // Select a specific sink via panel QML method
    QVERIFY(QMetaObject::invokeMethod(
        panel.get(), "selectDevice",
        Q_ARG(QVariant, QStringLiteral("alsa_output.usb-dac")),
        Q_ARG(QVariant, QStringLiteral("Reference USB DAC"))));

    QCOMPARE(EngineController::instance().currentOutputDevice(),
             QStringLiteral("alsa_output.usb-dac"));
    QCOMPARE(panel->property("isDefaultTargetSelected").toBool(), false);
    QCOMPARE(panel->property("activeTargetId").toString(),
             QStringLiteral("alsa_output.usb-dac"));
    QCOMPARE(panel->property("statusIsError").toBool(), false);

    QVariant displayName;
    QVERIFY(QMetaObject::invokeMethod(panel.get(), "activeDeviceDisplayName",
                                      Q_RETURN_ARG(QVariant, displayName)));
    QCOMPARE(displayName.toString(), QStringLiteral("Reference USB DAC"));

    // Select second sink
    QVERIFY(QMetaObject::invokeMethod(
        panel.get(), "selectDevice",
        Q_ARG(QVariant, QStringLiteral("alsa_output.pci-builtin")),
        Q_ARG(QVariant, QStringLiteral("Built-in Analog Stereo"))));
    QCOMPARE(EngineController::instance().currentOutputDevice(),
             QStringLiteral("alsa_output.pci-builtin"));
    QCOMPARE(panel->property("statusIsError").toBool(), false);

    // Attempt nonexistent device selection -> error banner state
    QVERIFY(QMetaObject::invokeMethod(
        panel.get(), "selectDevice",
        Q_ARG(QVariant, QStringLiteral("nonexistent.sink")),
        Q_ARG(QVariant, QStringLiteral("Missing Device"))));
    QCOMPARE(panel->property("statusIsError").toBool(), true);
    QCOMPARE(EngineController::instance().currentOutputDevice(),
             QStringLiteral("alsa_output.pci-builtin"));

    // Revert back to System Default
    QVERIFY(QMetaObject::invokeMethod(
        panel.get(), "selectDevice",
        Q_ARG(QVariant, QStringLiteral("default")),
        Q_ARG(QVariant, QStringLiteral("System Default (PipeWire Auto)"))));
    QCOMPARE(panel->property("isDefaultTargetSelected").toBool(), true);
    QCOMPARE(panel->property("statusIsError").toBool(), false);
    QCOMPARE(EngineController::instance().getAudioCapabilities().value("targetDeviceId").toString(),
             QString());
}

void TestOutputDeviceUi::testWorkspaceImportFailureUiDisplay()
{
    auto& ws = WorkspaceManager::instance();
    const QString wsDir = m_tempDir->path() + "/ui_import_ws";
    const QString srcDir = m_tempDir->path() + "/ui_import_src";
    QVERIFY(QDir().mkpath(wsDir));
    QVERIFY(QDir().mkpath(srcDir));

    QString wsErr;
    QVERIFY2(ws.registerAndOpenWorkspace(wsDir, wsErr), qPrintable(wsErr));

    // Create a valid stereo WAV and a mono WAV for import testing
    const QString stereoWav = srcDir + "/stereo_ok.wav";
    const QString monoWav = srcDir + "/mono_bad.wav";
    const QString symWav = srcDir + "/sym_link.wav";
    const QString hardWav = srcDir + "/hard_link.wav";

    QProcess ffStereo;
    ffStereo.start("ffmpeg", {"-y", "-f", "lavfi", "-i", "sine=frequency=440:duration=0.2",
                              "-ar", "48000", "-ac", "2", stereoWav});
    QVERIFY(ffStereo.waitForFinished(10000) && ffStereo.exitCode() == 0);

    QProcess ffMono;
    ffMono.start("ffmpeg", {"-y", "-f", "lavfi", "-i", "sine=frequency=440:duration=0.2",
                            "-ar", "48000", "-ac", "1", monoWav});
    QVERIFY(ffMono.waitForFinished(10000) && ffMono.exitCode() == 0);

    QVERIFY(::symlink(stereoWav.toUtf8().constData(), symWav.toUtf8().constData()) == 0);
    QVERIFY(::link(stereoWav.toUtf8().constData(), hardWav.toUtf8().constData()) == 0);

    QVERIFY(m_qmlEngine != nullptr);
    const QUrl wsPanelUrl = QUrl::fromLocalFile(
        QStringLiteral(DESK32_SOURCE_DIR "/qml/WorkspacePanel.qml"));
    QQmlComponent component(m_qmlEngine.get(), wsPanelUrl);
    if (component.isError()) {
        qWarning() << component.errors();
    }
    QVERIFY2(component.status() == QQmlComponent::Ready,
             qPrintable(component.errorString()));

    std::unique_ptr<QObject> panel(component.create());
    QVERIFY2(panel != nullptr, qPrintable(component.errorString()));

    // 1. Symbolic link rejection -> error severity + feedback item populated
    const QVariantMap symRes = ws.importFileFromUi(symWav, QString());
    QVERIFY(QMetaObject::invokeMethod(
        panel.get(), "handleImportFileResult",
        Q_ARG(QVariant, symRes),
        Q_ARG(QVariant, false)));
    QCOMPARE(panel->property("statusSeverity").toString(), QStringLiteral("error"));
    QVERIFY(panel->property("statusNotice").toString().contains("Symbolic links"));
    QCOMPARE(panel->property("importFeedbackItems").toList().size(), 1);

    // 2. Hardlink rejection -> error severity + feedback item populated
    const QVariantMap hardRes = ws.importFileFromUi(hardWav, QString());
    QVERIFY(QMetaObject::invokeMethod(
        panel.get(), "handleImportFileResult",
        Q_ARG(QVariant, hardRes),
        Q_ARG(QVariant, false)));
    QCOMPARE(panel->property("statusSeverity").toString(), QStringLiteral("error"));
    QVERIFY(panel->property("statusNotice").toString().contains("Hard-linked"));
    QCOMPARE(panel->property("importFeedbackItems").toList().size(), 1);

    // 3. Non-stereo rejection -> error severity
    const QVariantMap monoRes = ws.importFileFromUi(monoWav, QString());
    QVERIFY(QMetaObject::invokeMethod(
        panel.get(), "handleImportFileResult",
        Q_ARG(QVariant, monoRes),
        Q_ARG(QVariant, false)));
    QCOMPARE(panel->property("statusSeverity").toString(), QStringLiteral("error"));
    QVERIFY(panel->property("statusNotice").toString().contains("Non-stereo") ||
            panel->property("statusNotice").toString().contains("Stereo"));
    QCOMPARE(panel->property("importFeedbackItems").toList().size(), 1);

    // Remove hardlink before importing stereo_ok.wav (so link count returns to 1)
    QVERIFY(QFile::remove(hardWav));
    const QVariantMap okRes = ws.importFileFromUi(stereoWav, QString());
    QVERIFY(QMetaObject::invokeMethod(
        panel.get(), "handleImportFileResult",
        Q_ARG(QVariant, okRes),
        Q_ARG(QVariant, false)));
    QCOMPARE(panel->property("statusSeverity").toString(), QStringLiteral("success"));
    QCOMPARE(panel->property("importFeedbackItems").toList().size(), 0);

    // 4. Folder import with mixed valid + mono failure -> error severity & failure detail list
    const QVariantList folderRes = ws.importFolderFromUi(srcDir, QString());
    QVERIFY(QMetaObject::invokeMethod(
        panel.get(), "handleImportFolderResults",
        Q_ARG(QVariant, folderRes),
        Q_ARG(QVariant, false)));
    QCOMPARE(panel->property("statusSeverity").toString(), QStringLiteral("error"));
    QVERIFY(!panel->property("importFeedbackItems").toList().isEmpty());

    // 5. Deck ->WS copy failure emits deckWorkspaceImportFinished with ok=false and reason
    auto& ctrl = EngineController::instance();
    QVERIFY(ctrl.loadTrack("A", wsDir + "/stereo_ok.wav"));
    QSignalSpy copySpy(&ctrl, &EngineController::deckWorkspaceImportFinished);
    QVERIFY(!ctrl.copyDeckTrackToWorkspace("A"));
    QCOMPARE(copySpy.count(), 1);
    const QList<QVariant> args = copySpy.takeFirst();
    QCOMPARE(args[0].toString(), QStringLiteral("A"));
    QCOMPARE(args[1].toBool(), false);
    QCOMPARE(args[2].toString(), QStringLiteral("SelfCopy"));
    QVERIFY(!args[3].toString().isEmpty());

    ws.resetForTesting();
}

void TestOutputDeviceUi::testSettingsManagerPersistenceAndIntegration()
{
    auto& sm = desk32::core::SettingsManager::instance();
    const QString cfgPath = m_tempDir->path() + "/desk32_cfg/settings.json";
    sm.setConfigFilePathForTesting(cfgPath);
    QVERIFY(sm.load());
    QVERIFY(QFile::exists(cfgPath));
    QCOMPARE(sm.audioBackend(), QStringLiteral("PipeWire"));
    QCOMPARE(sm.audioBufferFrames(), 256);
    QCOMPARE(sm.audioPcmFormat(), QStringLiteral("F32LE"));
    QCOMPARE(sm.fullscreenOnStartup(), true);

    // Extensible dotted-key support for upcoming JACK / ALSA / DSP settings
    sm.setValue(QStringLiteral("audio.jack.auto_connect"), false);
    sm.setValue(QStringLiteral("audio.alsa.device"), QStringLiteral("hw:1,0"));
    sm.setValue(QStringLiteral("audio.alsa.exclusive_hw_direct"), true);
    sm.setLanguagePreference(QStringLiteral("ja"));
    sm.setFullscreenOnStartup(false);

    // Modify via EngineController -> persisted to settings.json automatically
    auto& ctrl = EngineController::instance();
    QVERIFY(ctrl.selectOutputDevice(QStringLiteral("alsa_output.usb-dac")));
    QVERIFY(ctrl.setOutputBufferFrames(512));
    ctrl.setOutputPcmFormat(QStringLiteral("S24_32LE"));
    ctrl.setLiveDitherEnabled(false);
    QVERIFY(ctrl.setFixedGraphSampleRate(96000.0));
    ctrl.setListeningPreset(QStringLiteral("Acoustic Clarity"));
    ctrl.setExternalInsertPosition(2);
    ctrl.setExternalInsertDisconnectPolicy(1);

    // Reload from disk into fresh SettingsManager state
    sm.setConfigFilePathForTesting(cfgPath);
    QVERIFY(sm.load());
    QCOMPARE(sm.languagePreference(), QStringLiteral("ja"));
    QCOMPARE(sm.fullscreenOnStartup(), false);
    QCOMPARE(sm.audioTargetDevice(), QStringLiteral("alsa_output.usb-dac"));
    QCOMPARE(sm.audioBufferFrames(), 512);
    QCOMPARE(sm.audioPcmFormat(), QStringLiteral("S24_32LE"));
    QCOMPARE(sm.liveDitherEnabled(), false);
    QCOMPARE(sm.audioFixedSampleRate(), 96000.0);
    QCOMPARE(sm.listeningPreset(), QStringLiteral("Acoustic Clarity"));
    QCOMPARE(sm.externalInsertPosition(), 2);
    QCOMPARE(sm.externalInsertDisconnectPolicy(), 1);
    QCOMPARE(sm.value(QStringLiteral("audio.jack.auto_connect")).toBool(), false);
    QCOMPARE(sm.value(QStringLiteral("audio.alsa.device")).toString(), QStringLiteral("hw:1,0"));
    QCOMPARE(sm.value(QStringLiteral("audio.alsa.exclusive_hw_direct")).toBool(), true);

    // Corrupt JSON fallback safety
    {
        QFile badFile(cfgPath);
        QVERIFY(badFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        badFile.write("{ invalid json");
        badFile.close();
    }
    QVERIFY(!sm.load());
    QVERIFY(!sm.lastError().isEmpty());
    QCOMPARE(sm.audioBufferFrames(), 256);

    sm.resetForTesting();
    // Restore controller defaults so subsequent tests remain clean
    QVERIFY(ctrl.selectOutputDevice(QStringLiteral("default")));
    QVERIFY(ctrl.setOutputBufferFrames(256));
    ctrl.setOutputPcmFormat(QStringLiteral("F32LE"));
    ctrl.setLiveDitherEnabled(true);
    QVERIFY(ctrl.setFixedGraphSampleRate(48000.0));
    ctrl.setListeningPreset(QStringLiteral("Monitor Flat"));
    ctrl.setExternalInsertPosition(4);
    ctrl.setExternalInsertDisconnectPolicy(0);
}

void TestOutputDeviceUi::testDeckStateContractFolderSessionAndSeek()
{
    const QString folderDir = m_tempDir->path() + "/folder_session_test";
    QVERIFY(QDir().mkpath(folderDir));

    for (int i = 1; i <= 2; ++i) {
        QProcess ff;
        ff.start("ffmpeg", {"-y", "-f", "lavfi", "-i", "sine=frequency=440:duration=0.6",
                            "-ar", "48000", "-ac", "2",
                            QString("%1/0%2_track.wav").arg(folderDir).arg(i)});
        QVERIFY(ff.waitForFinished(10000) && ff.exitCode() == 0);
    }

    auto& ctrl = EngineController::instance();
    // Calls 2-arg form (using default empty QStringList fileNames) as fixed for QML
    QVERIFY(ctrl.startFolderSession(QStringLiteral("A"), folderDir));

    QVariantMap st = ctrl.getDeckState(QStringLiteral("A"));
    QVERIFY(st.contains(QStringLiteral("playhead")));
    QVERIFY(st.contains(QStringLiteral("positionFrames")));
    QVERIFY(st.contains(QStringLiteral("playing")));
    QVERIFY(st.contains(QStringLiteral("fileName")));
    QVERIFY(st.contains(QStringLiteral("hasError")));
    QCOMPARE(st.value(QStringLiteral("folderActive")).toBool(), true);
    QCOMPARE(st.value(QStringLiteral("folderTotalTracks")).toInt(), 2);
    QCOMPARE(st.value(QStringLiteral("playing")).toBool(), false);
    QCOMPARE(st.value(QStringLiteral("hasError")).toBool(), false);
    QCOMPARE(st.value(QStringLiteral("fileName")).toString(), QStringLiteral("01_track.wav"));
    QCOMPARE(st.value(QStringLiteral("playhead")).toLongLong(),
             st.value(QStringLiteral("positionFrames")).toLongLong());

    ctrl.seek(QStringLiteral("A"), 12000);
    QVariantMap stAfter = ctrl.getDeckState(QStringLiteral("A"));
    QCOMPARE(stAfter.value(QStringLiteral("playhead")).toLongLong(), 12000);
    QCOMPARE(stAfter.value(QStringLiteral("positionFrames")).toLongLong(), 12000);

    // Advance Folder Session to track 2 and verify waveform extraction triggers & completes
    QVERIFY(ctrl.advanceFolderNext(QStringLiteral("A")));
    QVariantMap stTrack2 = ctrl.getDeckState(QStringLiteral("A"));
    QCOMPARE(stTrack2.value(QStringLiteral("fileName")).toString(), QStringLiteral("02_track.wav"));
    QCOMPARE(stTrack2.value(QStringLiteral("folderCurrentIndex")).toInt(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(
        ctrl.getDeckWaveformPeaks(QStringLiteral("A"), 128).value(QStringLiteral("ready")).toBool(),
        5000);

    // Verify PEQ / GEQ response curves update live even while DSP node starts bypassed
    QVERIFY(ctrl.isDspNodeBypassed(QStringLiteral("PEQ")));
    ctrl.setPeqBand(false, 8, 1000.0, 6.0, 1.0, 0, true);
    const QVariantList peqCurve = ctrl.getPeqCurve(false, 120);
    double maxPeqDb = -99.0;
    for (const QVariant& ptVar : peqCurve) {
        maxPeqDb = std::max(maxPeqDb, ptVar.toMap().value(QStringLiteral("db")).toDouble());
    }
    QVERIFY2(maxPeqDb > 4.5, qPrintable(QString("Expected PEQ curve > 4.5 dB, got %1").arg(maxPeqDb)));
    ctrl.setPeqBand(false, 8, 1000.0, 0.0, 1.0, 0, true);
}

void TestOutputDeviceUi::testWorkspaceViewDeckSelectionLockAndFolderDiscard()
{
    auto& ws = WorkspaceManager::instance();
    const QString wsDir = m_tempDir->path() + "/ws_deck_load_test";
    const QString srcDir = m_tempDir->path() + "/ws_deck_load_src";
    const QString fldDir = m_tempDir->path() + "/ws_folder_b_src";
    QVERIFY(QDir().mkpath(wsDir));
    QVERIFY(QDir().mkpath(srcDir));
    QVERIFY(QDir().mkpath(fldDir));

    const QString wsWavSrc = srcDir + "/ws_master.wav";
    const QString fldWav1 = fldDir + "/f1.wav";
    const QString fldWav2 = fldDir + "/f2.wav";
    for (const QString& p : {wsWavSrc, fldWav1, fldWav2}) {
        QProcess ff;
        ff.start("ffmpeg", {"-y", "-f", "lavfi", "-i", "sine=frequency=440:duration=0.3",
                            "-ar", "48000", "-ac", "2", p});
        QVERIFY(ff.waitForFinished(10000) && ff.exitCode() == 0);
    }

    QString wsErr;
    QVERIFY2(ws.registerAndOpenWorkspace(wsDir, wsErr), qPrintable(wsErr));
    const QVariantMap impRes = ws.importFileFromUi(wsWavSrc, QString());
    QCOMPARE(impRes.value(QStringLiteral("status")).toInt(), 0);

    auto& ctrl = EngineController::instance();
    ctrl.setDeckLocked(QStringLiteral("C"), true);
    QVERIFY(ctrl.startFolderSession(QStringLiteral("B"), fldDir));
    QVERIFY(ctrl.getDeckState(QStringLiteral("B")).value(QStringLiteral("folderActive")).toBool());

    const QUrl wsViewUrl = QUrl::fromLocalFile(
        QStringLiteral(DESK32_SOURCE_DIR "/qml/views/WorkspaceView.qml"));
    QQmlComponent comp(m_qmlEngine.get(), wsViewUrl);
    if (comp.isError()) qWarning() << comp.errors();
    QVERIFY2(comp.status() == QQmlComponent::Ready, qPrintable(comp.errorString()));

    std::unique_ptr<QObject> wsView(comp.create());
    QVERIFY2(wsView != nullptr, qPrintable(comp.errorString()));

    // 1. Locked Deck C cannot be selected and exposes explicit lock rejection reason
    wsView->setProperty("targetDeckId", QStringLiteral("A"));
    QVERIFY(QMetaObject::invokeMethod(wsView.get(), "selectTargetDeck",
                                      Q_ARG(QVariant, QStringLiteral("C"))));
    QCOMPARE(wsView->property("targetDeckId").toString(), QStringLiteral("A"));
    QVERIFY(!wsView->property("lockRejectedMsg").toString().isEmpty());

    // Even if targetDeckId were C directly, loading rejects safely with reason
    wsView->setProperty("targetDeckId", QStringLiteral("C"));
    QVariant loadLockedOk;
    QVERIFY(QMetaObject::invokeMethod(wsView.get(), "loadTrackRelToTargetDeck",
                                      Q_RETURN_ARG(QVariant, loadLockedOk),
                                      Q_ARG(QVariant, QStringLiteral("ws_master.wav"))));
    QCOMPARE(loadLockedOk.toBool(), false);
    QVERIFY(!wsView->property("lockRejectedMsg").toString().isEmpty());

    // 2. Selecting Folder Session Deck B and loading terminates/discards session + sets visible screen notice
    QVERIFY(QMetaObject::invokeMethod(wsView.get(), "selectTargetDeck",
                                      Q_ARG(QVariant, QStringLiteral("B"))));
    QCOMPARE(wsView->property("targetDeckId").toString(), QStringLiteral("B"));
    QSignalSpy endSpy(&ctrl, &EngineController::folderSessionEnded);
    QVariant loadBOk;
    QVERIFY(QMetaObject::invokeMethod(wsView.get(), "loadTrackRelToTargetDeck",
                                      Q_RETURN_ARG(QVariant, loadBOk),
                                      Q_ARG(QVariant, QStringLiteral("ws_master.wav"))));
    QCOMPARE(loadBOk.toBool(), true);
    QCOMPARE(endSpy.count(), 1);
    QCOMPARE(endSpy.takeFirst()[1].toString(), QStringLiteral("track_replaced"));
    QCOMPARE(ctrl.getDeckState(QStringLiteral("B")).value(QStringLiteral("folderActive")).toBool(), false);
    const QVariantMap discarded = wsView->property("sessionDiscardedInfo").toMap();
    QCOMPARE(discarded.value(QStringLiteral("deckId")).toString(), QStringLiteral("B"));
    QCOMPARE(discarded.value(QStringLiteral("totalTracks")).toInt(), 2);
    QVERIFY(wsView->property("statusNotice").toString().contains(QStringLiteral("discarded"), Qt::CaseInsensitive));

    ctrl.setDeckLocked(QStringLiteral("C"), false);
    ws.resetForTesting();
}

void TestOutputDeviceUi::testMainQmlCompilesWithOutputDevicePanel()
{
    QVERIFY(m_qmlEngine != nullptr);
    const QUrl mainUrl = QUrl::fromLocalFile(
        QStringLiteral(DESK32_SOURCE_DIR "/qml/Main.qml"));
    QQmlComponent component(m_qmlEngine.get(), mainUrl);
    if (component.isError()) {
        qWarning() << component.errors();
    }
    QVERIFY2(component.status() == QQmlComponent::Ready,
             qPrintable(component.errorString()));
}

QTEST_MAIN(TestOutputDeviceUi)
#include "test_output_device_ui.moc"
