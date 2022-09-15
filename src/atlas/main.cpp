#include "../version/version.h"
#include "zapplication.h"
#include "zcpuinfo.h"
#include "zexception.h"
#include "zglobalinit.h"
#include "zlog.h"
#include "zmainwindow.h"
#include "zservicemanager.h"
#include "zsysteminfo.h"
#include "ztheme.h"
#include "zrunbenchmark.h"

#include <QSurfaceFormat>
// #include <QOpenGLContext>
// #include <QOffscreenSurface>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <folly/ScopeGuard.h>
#include <gflags/gflags.h>
#include <iostream>

DEFINE_bool(run_benchmarks, false, "run benchmarks");
DECLARE_string(flagfile);

using namespace nim;

// force NVidia Optimus to used dedicated graphics
#ifdef _WIN32
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
}
#endif

int main(int argc, char* argv[])
{
  QCoreApplication::setOrganizationName("fenglab");
  // On macOS and iOS, if both a name and an Internet domain are specified for the organization, the domain
  //  is preferred over the name. On other platforms, the name is preferred over the domain.
#ifndef Q_OS_MACOS
  QCoreApplication::setOrganizationDomain("fenglab.xyz");
#endif
  QCoreApplication::setApplicationName("Atlas");
  try {
    if (QString setting_filename = "user_settings_flagfile.txt"; ZSystemInfo::configDir().exists(setting_filename)) {
      FLAGS_flagfile = QFile::encodeName(ZSystemInfo::configDir().absoluteFilePath(setting_filename)).constData();
    }
    std::string usage("Atlas is a brain map platform.  Usage:\n");
    usage += std::string(argv[0]) + "";
    gflags::SetUsageMessage(usage);
    gflags::SetVersionString(GIT_VERSION);
    gflags::ParseCommandLineFlags(&argc, &argv, false);

    QSurfaceFormat format;
#if defined(__APPLE__) && defined(ATLAS_USE_CORE_PROFILE)
    format.setVersion(3, 2);
    format.setProfile(QSurfaceFormat::CoreProfile);
#endif
    // format.setStereo(true);
    QSurfaceFormat::setDefaultFormat(format);

    if (FLAGS_run_benchmarks) {
      QCoreApplication app(argc, argv);
#ifdef _WIN32
      QString resourcesDIR = QCoreApplication::applicationDirPath() + u"/Resources";
      QString jdkDIR = QCoreApplication::applicationDirPath() + u"/Resources/jdk";
      QString jarsDIR = QCoreApplication::applicationDirPath() + u"/Resources/jars";
#elif defined(__APPLE__)
      QString resourcesDIR = QCoreApplication::applicationDirPath() + u"/../Resources";
      QString jdkDIR = QCoreApplication::applicationDirPath() + u"/../Resources/jdk";
      QString jarsDIR = QCoreApplication::applicationDirPath() + u"/../Resources/jars";
#else
      QString resourcesDIR = QCoreApplication::applicationDirPath() + u"/Resources";
      QString jdkDIR = QCoreApplication::applicationDirPath() + u"/Resources/jdk";
      QString jarsDIR = QCoreApplication::applicationDirPath() + u"/Resources/jars";
#endif
      initImgLib(argv[0], resourcesDIR, jdkDIR, jarsDIR, "", false);
      [[maybe_unused]] auto guardimglib = folly::makeGuard([]() { nim::shutdownImgLib(false); });
      if (FLAGS_run_benchmarks) {
        return ZRunBenchmark::run();
      }
    }

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
    nim::ZApplication app(argc, argv);

    if (!nim::ZCpuInfo::instance().bAVX) {
      QMessageBox::critical(nullptr,
                            QCoreApplication::applicationName(),
                            "CPU not supported.\nThis program requires CPU with AVX support. Click OK to exit.");
      LOG(ERROR) << "CPU not supported";
      return 1;
    }

    // init the logging mechanism
    QDir logDir = nim::ZSystemInfo::logDir();
    nim::ZSystemInfo::removeOldLogs();

    QString jdkDIR = ZApplication::resourcesDirPath() + u"/jdk";
    QString jarsDIR = ZApplication::resourcesDirPath() + u"/jars";
    initImgLib(argv[0], ZApplication::resourcesDirPath(), jdkDIR, jarsDIR, logDir.filePath("atlas"));
    [[maybe_unused]] auto guardimglib = folly::makeGuard([]() { nim::shutdownImgLib(); });

    if (!FLAGS_flagfile.empty()) {
      LOG(INFO) << "user setting file loaded: " << FLAGS_flagfile;
    } else {
      LOG(INFO) << "no user setting file";
    }
    LOG(INFO) << "current settings: \n" << gflags::CommandlineFlagsIntoString();

    ZTheme::instance();

    nim::ZSystemInfo::instance().logOSInfo();

    // ZServiceManager sm;

    //    // initialize OpenGL
    //    QOpenGLContext context;
    //    context.setFormat(format);
    //    context.create();
    //    if (!context.isValid()) {
    //      LOG(ERROR) << "Can not create OpenGL context";
    //    }
    //
    //    QOffscreenSurface surface;
    //    surface.setFormat(format);
    //    surface.create();
    //    if(!surface.isValid()) {
    //      LOG(ERROR) << "Can not create OpenGL Offscreen surface";
    //    }
    //    context.makeCurrent(&surface);
    //
    //    if (!ZSystemInfo::instance().initializeGL()) {
    //      QString msg = ZSystemInfo::instance().errorMessage();
    //      msg += ". 3D functions will be disabled.";
    //      QMessageBox::warning(nullptr, QApplication::applicationName(), "OpenGL Initialization.\n" + msg);
    //    }
    //    ZSystemInfo::instance().setStereoSupported(context.format().stereo());

    // ZMainWindow has Qt::WA_DeleteOnClose attribute
    auto mainWin = new nim::ZMainWindow(GIT_VERSION);
    QObject::connect(&app, &nim::ZApplication::fileOpenRequest, mainWin, &nim::ZMainWindow::loadUrls);
    mainWin->show();
    mainWin->initOpenglContext();

    return ZApplication::exec();
  }
  catch (const nim::ZException& e) {
    LOG(FATAL) << "exit with " << typeid(e).name() << ": " << e.what();
  }
  catch (const std::exception& e) {
    LOG(FATAL) << "exit with " << typeid(e).name() << ": " << e.what();
  }
  catch (...) {
    LOG(FATAL) << "exit with unknown exception";
  }
}
