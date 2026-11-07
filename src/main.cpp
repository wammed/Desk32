#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QIcon>
#include <QQuickStyle>
#include "core/LanguageManager.hpp"
#include "core/EngineController.hpp"
#include "core/log/LogService.hpp"

int main(int argc, char *argv[])
{
    // High DPI scaling is handled automatically by Qt 6
    QGuiApplication app(argc, argv);

    app.setApplicationName("Desk32");
    app.setApplicationDisplayName("Desk32");
    app.setOrganizationName("wammed");
    app.setWindowIcon(QIcon(":/Desk32/images/Desk32-Icon.svg"));

    QQuickStyle::setStyle("Basic");

    QQmlApplicationEngine engine;

    // Initialize bilingual language manager
    LanguageManager::instance().init(&app, &engine);

    // Initialize Engine Controller & Subsystems
    desk32::core::EngineController::instance().initialize();

    // Expose Singletons to QML
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "LanguageManager", &LanguageManager::instance());
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "EngineController", &desk32::core::EngineController::instance());
    qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "LogService", &desk32::core::log::LogService::instance());

    const QUrl url(QStringLiteral("qrc:/Desk32/qml/Main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    engine.load(url);

    return app.exec();
}
