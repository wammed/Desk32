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

#include "core/EngineController.hpp"
#include "core/LanguageManager.hpp"
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
    void testMainQmlCompilesWithOutputDevicePanel();

private:
    std::unique_ptr<QTemporaryDir> m_tempDir;
};

void TestOutputDeviceUi::initTestCase()
{
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    m_tempDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_tempDir->isValid());
    LogService::instance().start(m_tempDir->path() + "/test_output_device_ui_log.db");
    PresetManager::instance().setCustomRootPath(m_tempDir->path() + "/presets");

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

    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "LanguageManager",
                                 &LanguageManager::instance());
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "EngineController",
                                 &EngineController::instance());
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "LogService",
                                 &LogService::instance());
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "WorkspaceManager",
                                 &WorkspaceManager::instance());
}

void TestOutputDeviceUi::cleanupTestCase()
{
    if (auto* out = EngineController::instance().audioOutput()) {
        out->shutdown();
    }
    RenderEngine::instance().stop();
    WorkspaceManager::instance().resetForTesting();
    RenderSequenceManager::instance().close();
    LogService::instance().stop();
    m_tempDir.reset();
}

void TestOutputDeviceUi::testOutputDeviceEnumerationAndSelectionPanel()
{
    QQmlEngine engine;
    const QUrl panelUrl = QUrl::fromLocalFile(
        QStringLiteral(DESK32_SOURCE_DIR "/qml/OutputDevicePanel.qml"));
    QQmlComponent component(&engine, panelUrl);
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

void TestOutputDeviceUi::testMainQmlCompilesWithOutputDevicePanel()
{
    QQmlEngine engine;
    const QUrl mainUrl = QUrl::fromLocalFile(
        QStringLiteral(DESK32_SOURCE_DIR "/qml/Main.qml"));
    QQmlComponent component(&engine, mainUrl);
    if (component.isError()) {
        qWarning() << component.errors();
    }
    QVERIFY2(component.status() == QQmlComponent::Ready,
             qPrintable(component.errorString()));
}

QTEST_MAIN(TestOutputDeviceUi)
#include "test_output_device_ui.moc"
