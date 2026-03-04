#include "../version/version.h"
#include "zapplication.h"
#include "zcpuinfo.h"
#include "zexception.h"
#include "zimginit.h"
#include "zlog.h"
#include "zlogcache.h"
#include "zmainwindow.h"
#include "zservicemanager.h"
#include "zrpcservice.h"
#include "zsysteminfo.h"
#include "ztheme.h"

#include "zrunexport3danimation.h"
#include "zrunneutucommand2.h"
#include "zrundumpanimation3dschema.h"
#include "zwindowsheader.h"

#include <QSurfaceFormat>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QTimer>
#include <QUrl>
#include <folly/ScopeGuard.h>
#include <gflags/gflags.h>

DECLARE_string(flagfile);
DECLARE_bool(run_export_3d_animation);
DECLARE_bool(run_dump_animation3d_schema);
// Linux EGL headless switch for OpenGL context
#if defined(__linux__)
DECLARE_bool(__use_EGL);
#endif
DECLARE_bool(run_scene_server);

#ifdef _WIN32
extern "C" {
// force NVidia Optimus to used dedicated graphics
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
}
#endif

using namespace nim;

#if !defined(ATLAS_SANITIZE_ADDRESS)
extern "C" {
const char* __asan_default_options()
{
  return "detect_leaks=0";
}
}
#else
extern "C" {
const char* __asan_default_options()
{
  return "detect_leaks=1:log_path=asan.log";
}
}
#endif

#if defined(Q_OS_MAC)
#include <mach-o/dyld.h>
#elif defined(Q_OS_LINUX)
#include <unistd.h>
#include <limits.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

QString getExecutablePath()
{
#ifdef Q_OS_WIN
  wchar_t path[MAX_PATH] = {0};
  GetModuleFileNameW(NULL, path, MAX_PATH);
  return QString::fromWCharArray(path);
#elif defined(Q_OS_MAC)
  char path[1024];
  uint32_t size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) == 0) {
    return QString::fromUtf8(path);
  }
  return {};
#elif defined(Q_OS_LINUX)
  char result[PATH_MAX];
  ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
  return QString::fromUtf8(result, (count > 0) ? count : 0);
#else
  return {};
#endif
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    std::ignore = freopen("CONOUT$", "w", stdout);
    std::ignore = freopen("CONOUT$", "w", stderr);
  }
#endif

#ifndef __APPLE__
  // Construct the Vulkan layers path
  // VLOG(1) << getExecutablePath();
#ifdef Q_OS_WIN
  QString vulkanLayerPath = QDir(QFileInfo(getExecutablePath()).absolutePath()).filePath("vulkan");
#elif defined(Q_OS_LINUX)
  QString vulkanLayerPath =
    QDir(QFileInfo(getExecutablePath()).absolutePath()).filePath("Resources/vulkan/explicit_layer.d");
#endif
  // Set the environment variable
  qputenv("VK_ADD_LAYER_PATH", vulkanLayerPath.toUtf8());
  qputenv("VK_LAYER_PATH", vulkanLayerPath.toUtf8()); // should remove in later version
#endif

  QCoreApplication::setOrganizationName("fenglab");
  // On macOS and iOS, if both a name and an Internet domain are specified for the organization, the domain
  //  is preferred over the name. On other platforms, the name is preferred over the domain.
#ifndef __APPLE__
  QCoreApplication::setOrganizationDomain("fenglab.xyz");
#endif
  QCoreApplication::setApplicationName("Atlas");

  if (argc > 1 && strcmp(argv[1], "--command") == 0) {
    ZLogInit::instance("Atlas"s);

    LOG(INFO) << "Version: " << GIT_VERSION;

    // CLI mode: prefer a Core-only application to allow headless execution on servers/CI.
    // (QApplication requires a platform plugin and can abort in environments without a GUI.)
    QCoreApplication app(argc, argv);

    ZImgInit::instance(ZSystemInfo::resourcesDirPath(),
                       ZCpuInfo::instance().isX86_64 ? ZSystemInfo::jreDirPath() : ZSystemInfo::jreArmDirPath(),
                       ZSystemInfo::jarsDirPath(),
                       true);

    const std::string jsonDirPath = ZSystemInfo::jsonDirPath().toUtf8().toStdString();
    // neuTube CLI runner (modernized, src/img).
    return ZRunNeuTuCommand2().run(argc, argv, jsonDirPath);
  }

  QSurfaceFormat format;
#if defined(__APPLE__) && defined(ATLAS_USE_CORE_PROFILE)
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
#endif
  // format.setStereo(true);
  QSurfaceFormat::setDefaultFormat(format);

  QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);
#ifdef Q_OS_LINUX
  QCoreApplication::setAttribute(Qt::AA_DontUseNativeMenuBar, true); // do not use linux global menu bar
#endif
#ifdef Q_OS_MACOS
  QCoreApplication::setAttribute(Qt::AA_DontShowIconsInMenus, true);
#endif
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
#endif
  QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL, true);
  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts, true);
  QCoreApplication::setAttribute(Qt::AA_UseStyleSheetPropagationInWidgetStyles, true);
  QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, true);

  ZApplication app(argc, argv);

  if (QString setting_filename = "user_settings_flagfile.txt"; ZSystemInfo::configDir().exists(setting_filename)) {
    FLAGS_flagfile = QFile::encodeName(ZSystemInfo::configDir().absoluteFilePath(setting_filename)).constData();
  }
  std::string usage("Atlas is a brain mapping platform.  Usage:\n");
  usage += std::string(argv[0]) + "";
  gflags::SetUsageMessage(usage);
  gflags::SetVersionString(GIT_VERSION);
  gflags::ParseCommandLineFlags(&argc, &argv, false);

  bool isGUIMode = !(FLAGS_run_export_3d_animation || FLAGS_run_dump_animation3d_schema);

  // init the logging mechanism
  QDir logDir = ZSystemInfo::logDir();
  ZSystemInfo::removeOldLogs();
  ZLogInit::instance("Atlas"s, logDir.filePath("atlas"));
  if (isGUIMode) {
    addLogSink(&ZLogCache::instance());
  }

  try {
    LOG(INFO) << "Version: " << GIT_VERSION;
    LOG(INFO) << "log location: " << logDir.absolutePath();

    ZSystemInfo::instance().logOSInfo();
#ifdef _WIN32
    logActiveCodePage();
#endif
    LOG(INFO) << "ASAN_OPTIONS: " << __asan_default_options();

    if (!FLAGS_flagfile.empty()) {
      LOG(INFO) << "user setting file loaded: " << FLAGS_flagfile;
    } else {
      LOG(INFO) << "no user setting file";
    }
    LOG(INFO) << "current settings: \n" << gflags::CommandlineFlagsIntoString();

    ZImgInit::instance(ZSystemInfo::resourcesDirPath(),
                       ZCpuInfo::instance().isX86_64 ? ZSystemInfo::jreDirPath() : ZSystemInfo::jreArmDirPath(),
                       ZSystemInfo::jarsDirPath(),
                       true);

    if (!isGUIMode) {
      // start non-GUI version...
      LOG(INFO) << "console mode";
      if (FLAGS_run_export_3d_animation) {
        ZRunExport3DAnimation rea;
        return rea.run();
      } else if (FLAGS_run_dump_animation3d_schema) {
        nim::ZRunDumpAnimation3DSchema dumper;
        return dumper.run();
      }
    } else {
      // start GUI version...
      LOG(INFO) << "GUI mode";
      // Start RPC service manager only in GUI mode. Avoid spawning RPC threads
      // for console utilities (export/schema dump) to ensure deterministic
      // teardown without cross-thread shutdown requirements.
      ZServiceManager sm(GIT_VERSION);
      ZTheme::instance();

      // ZMainWindow has Qt::WA_DeleteOnClose attribute
      auto mainWin = new ZMainWindow(GIT_VERSION);
      sm.setMainWindow(mainWin);
      QObject::connect(&app, &ZApplication::fileOpenRequest, mainWin, &ZMainWindow::loadUrls);
      mainWin->show();

      // Support launching Atlas with a scene file (or directory of files) as a positional argument.
      // Example:
      //   Atlas /path/to/example.scene --atlas_default_render_backend=vulkan
      //
      // We intentionally ignore any args that look like flags ("-" / "--") and only attempt to load
      // paths that exist locally.
      QList<QUrl> startupUrls;
      const QStringList args = QCoreApplication::arguments();
      for (int i = 1; i < args.size(); ++i) {
        const QString& arg = args[i];
        if (arg.startsWith(u'-')) {
          continue;
        }
        QFileInfo pathInfo(arg);
        if (!pathInfo.exists()) {
          continue;
        }
        startupUrls.append(QUrl::fromLocalFile(pathInfo.absoluteFilePath()));
      }
      if (!startupUrls.isEmpty()) {
        // Defer until the event loop starts so any deferred initialization signals/events work normally.
        QTimer::singleShot(0, mainWin, [mainWin, startupUrls]() {
          LOG(INFO) << "Loading startup URLs: " << startupUrls.size();
          mainWin->loadUrls(startupUrls);
        });
      }

      return app.exec();
    }
  }
  catch (const ZException& e) {
    if (isGUIMode) {
      QMessageBox::critical(nullptr, QCoreApplication::applicationName(), e.what());
    }
    LOG(ERROR) << "exit with " << typeid(e).name() << ": " << e.what();
    return 1;
  }
  catch (const std::exception& e) {
    LOG(FATAL) << "exit with " << typeid(e).name() << ": " << e.what();
  }
  catch (...) {
    LOG(FATAL) << "exit with unknown exception";
  }
}
