#include "core/EngineController.hpp"
#include "core/LanguageManager.hpp"
#include "core/SettingsManager.hpp"
#include "core/log/LogErrorCodes.hpp"
#include "core/log/LogService.hpp"
#include "engine/render/RenderEngine.hpp"
#include "engine/render/RenderSequenceManager.hpp"
#include "engine/workspace/WorkspaceManager.hpp"
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <QVulkanFunctions>
#include <QVulkanInstance>
#include <vector>

namespace {
struct VulkanProbeResult {
  bool available = false;
  QString reason;
  int vkResult = 0;
};

VulkanProbeResult probeVulkanAvailable() {
  QVulkanInstance probeInst;
  if (!probeInst.create()) {
    return {false, QStringLiteral("instance_create_failed"),
            static_cast<int>(probeInst.errorCode())};
  }
  QVulkanFunctions *funcs = probeInst.functions();
  if (!funcs) {
    return {false, QStringLiteral("functions_unavailable"), 0};
  }
  uint32_t devCount = 0;
  const VkResult res = funcs->vkEnumeratePhysicalDevices(probeInst.vkInstance(),
                                                         &devCount, nullptr);
  if (res != VK_SUCCESS || devCount == 0) {
    return {false, QStringLiteral("no_physical_devices"),
            static_cast<int>(res)};
  }
  return {true, QString(), static_cast<int>(res)};
}

QString graphicsApiToString(QSGRendererInterface::GraphicsApi api) {
  switch (api) {
  case QSGRendererInterface::Software:
    return QStringLiteral("Software");
  case QSGRendererInterface::OpenVG:
    return QStringLiteral("OpenVG");
  case QSGRendererInterface::OpenGL:
    return QStringLiteral("OpenGL");
  case QSGRendererInterface::Direct3D11:
    return QStringLiteral("Direct3D11");
  case QSGRendererInterface::Vulkan:
    return QStringLiteral("Vulkan");
  case QSGRendererInterface::Metal:
    return QStringLiteral("Metal");
  case QSGRendererInterface::Direct3D12:
    return QStringLiteral("Direct3D12");
  default:
    return QStringLiteral("Unknown");
  }
}

QString detectGpuName(QQuickWindow *win,
                      QSGRendererInterface::GraphicsApi api) {
  if (api == QSGRendererInterface::Vulkan) {
    QVulkanInstance *vkInst = win ? win->vulkanInstance() : nullptr;
    if (!vkInst && win && win->rendererInterface()) {
      vkInst =
          static_cast<QVulkanInstance *>(win->rendererInterface()->getResource(
              win, QSGRendererInterface::VulkanInstanceResource));
    }
    if (vkInst && vkInst->isValid()) {
      if (QVulkanFunctions *funcs = vkInst->functions()) {
        VkPhysicalDevice physDev = VK_NULL_HANDLE;
        if (win && win->rendererInterface()) {
          if (void *rawPhys = win->rendererInterface()->getResource(
                  win, QSGRendererInterface::PhysicalDeviceResource)) {
            physDev = *static_cast<VkPhysicalDevice *>(rawPhys);
          }
        }
        if (physDev == VK_NULL_HANDLE &&
            vkInst->vkInstance() != VK_NULL_HANDLE) {
          uint32_t devCount = 0;
          if (funcs->vkEnumeratePhysicalDevices(vkInst->vkInstance(), &devCount,
                                                nullptr) == VK_SUCCESS &&
              devCount > 0) {
            std::vector<VkPhysicalDevice> devs(devCount, VK_NULL_HANDLE);
            if (funcs->vkEnumeratePhysicalDevices(vkInst->vkInstance(),
                                                  &devCount,
                                                  devs.data()) == VK_SUCCESS &&
                !devs.empty()) {
              physDev = devs.front();
            }
          }
        }
        if (physDev != VK_NULL_HANDLE) {
          VkPhysicalDeviceProperties props{};
          funcs->vkGetPhysicalDeviceProperties(physDev, &props);
          const QString name = QString::fromUtf8(props.deviceName).trimmed();
          if (!name.isEmpty()) {
            return name;
          }
        }
      }
    }
  } else if (api == QSGRendererInterface::OpenGL) {
    if (auto *ctx = QOpenGLContext::currentContext()) {
      if (auto *funcs = ctx->functions()) {
        const GLubyte *renderer = funcs->glGetString(GL_RENDERER);
        if (renderer) {
          return QString::fromUtf8(reinterpret_cast<const char *>(renderer));
        }
      }
    }
  }
  // Lightweight DRM uevent fallback on Linux (zero external dependencies)
  for (int cardIdx = 0; cardIdx < 4; ++cardIdx) {
    QFile uevent(
        QStringLiteral("/sys/class/drm/card%1/device/uevent").arg(cardIdx));
    if (uevent.open(QIODevice::ReadOnly | QIODevice::Text)) {
      QString driver;
      QString pciId;
      const QList<QByteArray> lines = uevent.readAll().split('\n');
      for (const QByteArray &rawLine : lines) {
        const QString line = QString::fromUtf8(rawLine).trimmed();
        if (line.startsWith(QStringLiteral("DRIVER=")))
          driver = line.mid(7);
        else if (line.startsWith(QStringLiteral("PCI_ID=")))
          pciId = line.mid(7);
      }
      if (!driver.isEmpty() || !pciId.isEmpty()) {
        return QStringLiteral("%1 (%2)").arg(
            driver.isEmpty() ? QStringLiteral("drm") : driver,
            pciId.isEmpty() ? QStringLiteral("unknown") : pciId);
      }
    }
  }
  return QStringLiteral("unknown");
}
} // namespace

int main(int argc, char *argv[]) {
  // High DPI scaling is handled automatically by Qt 6
  QGuiApplication app(argc, argv);

  app.setApplicationName("Desk32");
  app.setApplicationDisplayName("Desk32");
  app.setOrganizationName("wammed");
  app.setWindowIcon(QIcon(":/Desk32/images/Desk32-Icon.svg"));

  const bool hasEnvBackendOverride =
      !qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND") ||
      !qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND");
  VulkanProbeResult vkProbe{};
  bool fellBackToOpenGL = false;
  if (!hasEnvBackendOverride) {
    vkProbe = probeVulkanAvailable();
    if (vkProbe.available) {
      QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
    } else {
      QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
      fellBackToOpenGL = true;
    }
  }

  QQuickStyle::setStyle("Basic");

  QQmlApplicationEngine engine;

  // Load unified XDG settings ($XDG_CONFIG_HOME/desk32/settings.json)
  desk32::core::SettingsManager::instance().load();

  // Initialize bilingual language manager (honors SettingsManager preference)
  LanguageManager::instance().init(&app, &engine);

  const QStringList appArgs = QCoreApplication::arguments();
  const bool prototypeMode = appArgs.contains(QStringLiteral("--ui-prototype"));
  QString screenshotPath;
  const qsizetype ssIdx = appArgs.indexOf(QStringLiteral("--screenshot"));
  if (ssIdx >= 0 && ssIdx + 1 < appArgs.size()) {
    screenshotPath = appArgs.at(ssIdx + 1);
  }
  const bool offlineMode =
      appArgs.contains(QStringLiteral("--offline")) || !screenshotPath.isEmpty();

  // Initialize Engine Controller & Subsystems (skip audio init during headless prototype screenshot)
  if (!(prototypeMode && !screenshotPath.isEmpty())) {
    desk32::core::EngineController::instance().initialize(offlineMode);
    if (screenshotPath.isEmpty() && !prototypeMode &&
        desk32::core::SettingsManager::instance().restoreSessionOnStartup()) {
      desk32::core::EngineController::instance().restoreSessionState();
    }
  }

  if (fellBackToOpenGL) {
    QJsonObject det;
    det.insert(QStringLiteral("stage"), QStringLiteral("vulkan_probe"));
    det.insert(QStringLiteral("reason"), vkProbe.reason);
    det.insert(QStringLiteral("vk_result"), vkProbe.vkResult);
    det.insert(QStringLiteral("fallback_backend"), QStringLiteral("OpenGL"));
    desk32::core::log::LogService::instance().log(
        QStringLiteral("Session"), QStringLiteral("GraphicsFallback"),
        QStringLiteral("Warning"),
        QStringLiteral("Vulkan unavailable (%1); falling back to OpenGL.")
            .arg(vkProbe.reason),
        QStringLiteral("System"), QStringLiteral("Desk32"),
        QString::fromUtf8(QJsonDocument(det).toJson(QJsonDocument::Compact)),
        QString(), QString(),
        desk32::core::log::ErrorCode::VulkanFallbackToOpenGL);
  }

  // Expose Singletons to QML
  qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "SettingsManager",
                               &desk32::core::SettingsManager::instance());
  qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "LanguageManager",
                               &LanguageManager::instance());
  qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "EngineController",
                               &desk32::core::EngineController::instance());
  qmlRegisterSingletonInstance("Desk32.Core", 1, 0, "LogService",
                               &desk32::core::log::LogService::instance());
  qmlRegisterSingletonInstance(
      "Desk32.Core", 1, 0, "WorkspaceManager",
      &desk32::engine::workspace::WorkspaceManager::instance());

  const QUrl url(prototypeMode
                     ? QStringLiteral("qrc:/Desk32/qml/PrototypeGallery.qml")
                     : QStringLiteral("qrc:/Desk32/qml/Main.qml"));
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreated, &app,
      [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
          QCoreApplication::exit(-1);
      },
      Qt::QueuedConnection);

  engine.load(url);

  if (!engine.rootObjects().isEmpty()) {
    if (auto *win =
            qobject_cast<QQuickWindow *>(engine.rootObjects().first())) {
      if (!screenshotPath.isEmpty()) {
        std::fprintf(stderr, "[desk32] scheduling screenshot capture...\n");
        std::fflush(stderr);
        QTimer::singleShot(300, &app, [win, screenshotPath]() {
          std::fprintf(stderr, "[desk32] timer fired, calling grabToImage\n");
          std::fflush(stderr);
          if (auto *item = win->contentItem()) {
            auto res = item->grabToImage(QSize(2560, 1440));
            if (res) {
              QObject::connect(
                  res.data(), &QQuickItemGrabResult::ready,
                  [res, screenshotPath]() {
                    const QImage img = res->image();
                    const bool saved = !img.isNull() && img.save(screenshotPath);
                    std::fprintf(stderr,
                                 "[desk32] grabToImage ready (%dx%d, ok=%d)\n",
                                 img.width(), img.height(), saved ? 1 : 0);
                    std::fflush(stderr);
                    QCoreApplication::exit(saved ? 0 : 3);
                  });
              return;
            }
          }
          QImage img = win->grabWindow();
          const bool saved = !img.isNull() && img.save(screenshotPath);
          std::fprintf(stderr, "[desk32] grabWindow fallback (%dx%d, ok=%d)\n",
                       img.width(), img.height(), saved ? 1 : 0);
          std::fflush(stderr);
          QCoreApplication::exit(saved ? 0 : 3);
        });
      }
      QObject::connect(
          win, &QQuickWindow::sceneGraphInitialized, &app,
          [win]() {
            const auto api = win->rendererInterface()->graphicsApi();
            const QString backend = graphicsApiToString(api);
            const QString gpu = detectGpuName(win, api);
            QMetaObject::invokeMethod(
                &desk32::core::EngineController::instance(),
                [backend, gpu]() {
                  desk32::core::EngineController::instance().recordSessionStart(
                      backend, gpu);
                },
                Qt::QueuedConnection);
          },
          Qt::DirectConnection);

      QObject::connect(
          win, &QQuickWindow::sceneGraphError, &app,
          [](QQuickWindow::SceneGraphError error, const QString &message) {
            QJsonObject det;
            det.insert(QStringLiteral("stage"),
                       QStringLiteral("scene_graph_init"));
            det.insert(QStringLiteral("qt_scene_graph_error"),
                       static_cast<int>(error));
            det.insert(QStringLiteral("error_message"), message);
            desk32::core::log::LogService::instance().log(
                QStringLiteral("Session"), QStringLiteral("SceneGraphError"),
                QStringLiteral("Failed"),
                QStringLiteral("Scene graph initialization failed: %1")
                    .arg(message),
                QStringLiteral("System"), QStringLiteral("Desk32"),
                QString::fromUtf8(
                    QJsonDocument(det).toJson(QJsonDocument::Compact)),
                QString(), QString(),
                desk32::core::log::ErrorCode::SceneGraphInitError);
            desk32::core::log::LogService::instance().flush();
          });
    }
  }

  QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [screenshotPath, prototypeMode]() {
    if (screenshotPath.isEmpty() && !prototypeMode) {
      if (desk32::core::SettingsManager::instance().isLoaded()) {
        desk32::core::SettingsManager::instance().save();
      }
      desk32::core::EngineController::instance().saveSessionState();
    }
    desk32::engine::render::RenderEngine::instance().stop();
    desk32::engine::render::RenderSequenceManager::instance().close();
    desk32::engine::workspace::WorkspaceManager::instance()
        .closeWorkspaceForShutdown();
    desk32::core::EngineController::instance().recordSessionEnd(
        QStringLiteral("normal_quit"));
    desk32::core::log::LogService::instance().stop();
  });

  if (!screenshotPath.isEmpty()) {
    std::fprintf(stderr, "[desk32] entering app.exec()\n");
    std::fflush(stderr);
  }
  return app.exec();
}
