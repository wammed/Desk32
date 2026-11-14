#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QFile>
#include <QIcon>
#include <QQuickStyle>
#include "core/LanguageManager.hpp"
#include "core/EngineController.hpp"
#include "core/log/LogService.hpp"

namespace {
QString graphicsApiToString(QSGRendererInterface::GraphicsApi api) {
    switch (api) {
        case QSGRendererInterface::Software: return QStringLiteral("Software");
        case QSGRendererInterface::OpenVG:   return QStringLiteral("OpenVG");
        case QSGRendererInterface::OpenGL:   return QStringLiteral("OpenGL");
        case QSGRendererInterface::Direct3D11: return QStringLiteral("Direct3D11");
        case QSGRendererInterface::Vulkan:   return QStringLiteral("Vulkan");
        case QSGRendererInterface::Metal:    return QStringLiteral("Metal");
        case QSGRendererInterface::Direct3D12: return QStringLiteral("Direct3D12");
        default: return QStringLiteral("Unknown");
    }
}

QString detectGpuName(QSGRendererInterface::GraphicsApi api) {
    if (api == QSGRendererInterface::OpenGL) {
        if (auto* ctx = QOpenGLContext::currentContext()) {
            if (auto* funcs = ctx->functions()) {
                const GLubyte* renderer = funcs->glGetString(GL_RENDERER);
                if (renderer) {
                    return QString::fromUtf8(reinterpret_cast<const char*>(renderer));
                }
            }
        }
    }
    // Lightweight DRM uevent fallback on Linux (zero external dependencies)
    QFile uevent(QStringLiteral("/sys/class/drm/card0/device/uevent"));
    if (uevent.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString driver;
        QString pciId;
        while (!uevent.atEnd()) {
            const QString line = QString::fromUtf8(uevent.readLine()).trimmed();
            if (line.startsWith(QStringLiteral("DRIVER="))) driver = line.mid(7);
            else if (line.startsWith(QStringLiteral("PCI_ID="))) pciId = line.mid(7);
        }
        if (!driver.isEmpty() || !pciId.isEmpty()) {
            return QStringLiteral("%1 (%2)").arg(driver.isEmpty() ? QStringLiteral("drm") : driver,
                                                 pciId.isEmpty() ? QStringLiteral("unknown") : pciId);
        }
    }
    return QStringLiteral("unknown");
}
} // namespace

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

    if (!engine.rootObjects().isEmpty()) {
        if (auto* win = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) {
            QObject::connect(win, &QQuickWindow::sceneGraphInitialized, &app, [win]() {
                const auto api = win->rendererInterface()->graphicsApi();
                const QString backend = graphicsApiToString(api);
                const QString gpu = detectGpuName(api);
                QMetaObject::invokeMethod(&desk32::core::EngineController::instance(),
                                          [backend, gpu]() {
                    desk32::core::EngineController::instance().recordSessionStart(backend, gpu);
                }, Qt::QueuedConnection);
            }, Qt::DirectConnection);
        }
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, []() {
        desk32::core::EngineController::instance().recordSessionEnd(QStringLiteral("normal_quit"));
        desk32::core::log::LogService::instance().flush();
    });

    return app.exec();
}
