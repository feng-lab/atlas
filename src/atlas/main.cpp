#include "../version/version.h"
#include "zapplication.h"
#include "zcpuinfo.h"
#include "zexception.h"
#include "zimginit.h"
#include "zlog.h"
#include "zmainwindow.h"
#include "zservicemanager.h"
#include "zsysteminfo.h"
#include "ztheme.h"

#include "zrunbenchmark.h"
#include "zrunexport3danimation.h"
#include "zrunneutucommand.h"
#include "zwindowsheader.h"
#include "zvulkan.h"

#include <QSurfaceFormat>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <folly/ScopeGuard.h>
#include <gflags/gflags.h>

DECLARE_string(flagfile);
DEFINE_bool(run_benchmarks, false, "run benchmarks");
DECLARE_bool(run_export_3d_animation);

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

int main(int argc, char* argv[])
{
#ifdef _WIN32
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    std::ignore = freopen("CONOUT$", "w", stdout);
    std::ignore = freopen("CONOUT$", "w", stderr);
  }
#endif

  QCoreApplication::setOrganizationName("fenglab");
  // On macOS and iOS, if both a name and an Internet domain are specified for the organization, the domain
  //  is preferred over the name. On other platforms, the name is preferred over the domain.
#ifndef __APPLE__
  QCoreApplication::setOrganizationDomain("fenglab.xyz");
#endif
  QCoreApplication::setApplicationName("Atlas");
  try {
    if (argc > 1 && strcmp(argv[1], "--command") == 0) {
      ZApplication app(argc, argv);

      initImgLib(argv[0],
                 ZSystemInfo::resourcesDirPath(),
                 ZCpuInfo::instance().isX86_64 ? ZSystemInfo::jreDirPath() : ZSystemInfo::jreArmDirPath(),
                 ZSystemInfo::jarsDirPath(),
                 "",
                 true,
                 false);
      [[maybe_unused]] auto guardimglib = folly::makeGuard([]() {
        shutdownImgLib();
      });

      LOG(INFO) << "Version: " << GIT_VERSION;

      return ZRunNeuTuCommand().run(argc, argv);
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

    // init the logging mechanism
    QDir logDir = ZSystemInfo::logDir();
    ZSystemInfo::removeOldLogs();

    bool isGUIMode = !(FLAGS_run_benchmarks || FLAGS_run_export_3d_animation);
    initImgLib(argv[0],
               ZSystemInfo::resourcesDirPath(),
               ZCpuInfo::instance().isX86_64 ? ZSystemInfo::jreDirPath() : ZSystemInfo::jreArmDirPath(),
               ZSystemInfo::jarsDirPath(),
               logDir.filePath("atlas"),
               true,
               isGUIMode);
    [[maybe_unused]] auto guardimglib = folly::makeGuard([]() {
      shutdownImgLib();
    });

    LOG(INFO) << "Version: " << GIT_VERSION;

    LOG(INFO) << "log location: " << logDir.absolutePath();

    if (!FLAGS_flagfile.empty()) {
      LOG(INFO) << "user setting file loaded: " << FLAGS_flagfile;
    } else {
      LOG(INFO) << "no user setting file";
    }
    LOG(INFO) << "current settings: \n" << gflags::CommandlineFlagsIntoString();

    ZSystemInfo::instance().logOSInfo();

#ifdef _WIN32
    logActiveCodePage();
#endif

    LOG(INFO) << "ASAN_OPTIONS: " << __asan_default_options();

    test();

    // ZServiceManager sm;

    if (isGUIMode) {
      // start GUI version...
      LOG(INFO) << "GUI mode";
      ZTheme::instance();

      // ZMainWindow has Qt::WA_DeleteOnClose attribute
      auto mainWin = new ZMainWindow(GIT_VERSION);
      QObject::connect(&app, &ZApplication::fileOpenRequest, mainWin, &ZMainWindow::loadUrls);
      mainWin->show();
    } else {
      // start non-GUI version...
      try {
        LOG(INFO) << "console mode";
        if (FLAGS_run_benchmarks) {
          return ZRunBenchmark::run();
        }
        if (FLAGS_run_export_3d_animation) {
          ZRunExport3DAnimation rea;
          return rea.run();
        }
      }
      catch (const ZException& e) {
        LOG(ERROR) << "exit with " << typeid(e).name() << ": " << e.what();
        return 1;
      }
      catch (const std::exception& e) {
        LOG(ERROR) << "exit with " << typeid(e).name() << ": " << e.what();
        return 1;
      }
    }

    return app.exec();
  }
  catch (const ZException& e) {
    QMessageBox::critical(nullptr, QCoreApplication::applicationName(), e.what());
    LOG(FATAL) << "exit with " << typeid(e).name() << ": " << e.what();
  }
  catch (const std::exception& e) {
    LOG(FATAL) << "exit with " << typeid(e).name() << ": " << e.what();
  }
  catch (...) {
    LOG(FATAL) << "exit with unknown exception";
  }
}
