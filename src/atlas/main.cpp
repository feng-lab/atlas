#include <QApplication>

#include "zmainwindow.h"

#include "zlog.h"
#include <iostream>
#include "zlogmodelsink.h"
#include <QDir>
#include <QFileInfo>
#include "zcpuinfo.h"
#include "zsysteminfo.h"
#include <fftw3.h>
#include <QMessageBox>
#include "zexception.h"

#ifdef _USE_MKL_
#include "mkl_service.h"
#endif

#ifdef _USE_IPP_
#include "ippcore.h"
#include "ippi.h"
#endif

#include <QSurfaceFormat>

#include <QStack>
#include <QPointer>
// thanks to Daniel Price for this workaround
struct MacEventFilter : public QObject
{
  QStack<QPointer<QWidget>> m_activationstack; // stack of widgets to re-active on dialog close.

  explicit MacEventFilter(QObject *parent = nullptr)
    : QObject(parent)
  {}

  virtual bool eventFilter(QObject *anObject, QEvent *anEvent)
  {
    switch (anEvent->type()) {
    case QEvent::Show: {
      if ((anObject->inherits("QDialog") || anObject->inherits("QDockWidget")) && qApp->activeWindow()) {
        // Workaround for Qt bug where opened QDialogs do not re-activate previous window
        // when accepted or rejected. We cannot rely on the parent pointers so push the previous
        // active window onto a stack before the dialog is shown.
        // We have to use a stack in case a dialog opens another dialog.
        // NOTE: It's important to use QPointers so that any widgets deleted by Qt do not lead to
        // hanging pointers in the stack.
        m_activationstack.push(qApp->activeWindow());
      }
      break;
    }
    case QEvent::Hide: {
      if ((anObject->inherits("QDialog") || anObject->inherits("QDockWidget")) && !m_activationstack.isEmpty()) {
        QPointer<QWidget> widget = m_activationstack.pop();
        if (widget) {
          // Re-acivate widgets in the order as dialogs are closed. See Show case above.
          widget->activateWindow();
          widget->raise();
        }
      }
      break;
    }
    default:
      break;
    }

    return QObject::eventFilter(anObject, anEvent);
  }
};

void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
  switch (type) {
  case QtInfoMsg:
    LINFOF(context.file ? context.file : "QtFile",
           context.line,
           context.function ? context.function : "QtFunction") << msg;
    break;
  case QtWarningMsg:
    LWARNF(context.file ? context.file : "QtFile",
           context.line,
           context.function ? context.function : "QtFunction") << msg;
    break;
  case QtCriticalMsg:
    LERRORF(context.file ? context.file : "QtFile",
            context.line,
            context.function ? context.function : "QtFunction") << msg;
    break;
  case QtFatalMsg:
    LFATALF(context.file ? context.file : "QtFile",
            context.line,
            context.function ? context.function : "QtFunction") << msg;
    abort();
  default:
    break;
  }
}

// force NVidia Optimus to used dedicated graphics
#ifdef _WIN32
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
}
#endif

void removeOldLogs(const QDir& dir, int numberToKeep = 20)
{
  QDir ld(dir);
  ld.cdUp();
  QStringList filters;
  filters << "????????""-??????.???_LOG";
  QFileInfoList list = ld.entryInfoList(filters,
                                        QDir::Dirs | QDir::NoSymLinks,
                                        QDir::Name);
  for (int i=0; i < list.size() - numberToKeep; ++i) {
    QDir logDir(list.at(i).absoluteFilePath());
    logDir.removeRecursively();
  }
}

int main(int argc, char *argv[])
{
  QSurfaceFormat format;

#if defined(__APPLE__) && defined(_USE_CORE_PROFILE_)
  format.setVersion(3, 2);
  format.setProfile(QSurfaceFormat::CoreProfile);
#endif
  //format.setStereo(true);
  QSurfaceFormat::setDefaultFormat(format);

  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts, true);
  QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);
  QApplication app(argc, argv);
  app.setApplicationName("Atlas");
  app.setOrganizationDomain("atlas.com");
  app.setOrganizationName("Atlas");

  // init the logging mechanism
  QDir logDir = ZSystemInfoInstance.logDir();
  removeOldLogs(logDir);
  nim::initLogging(argv[0], logDir.filePath("atlas"));
  nim::addLogSink(nim::logModelSinkInstance());

  qInstallMessageHandler(myMessageOutput);

  LOG(INFO) << "--- App Log Started ---";
  ZSystemInfoInstance.logOSInfo();
  ZCpuInfoInstance.logCpuInfo();
  fftw_init_threads();
  fftw_plan_with_nthreads(ZCpuInfoInstance.nPhysicalCores);

#ifdef _USE_MKL_
  // todo: check this for amd cpu
  MKLVersion mklVer;
  MKL_Get_Version(&mklVer);
  LOG(INFO) << "MKL: " << mklVer.Platform << mklVer.Processor << " "
            << mklVer.MajorVersion << " "
            << mklVer.MinorVersion << " "
            << mklVer.UpdateVersion << " "
            << mklVer.Build;
  LOG(INFO) << "";
#endif

#ifdef _USE_IPP_
  // todo: check this for amd cpu
  IppStatus status = ippInit();
  if (status == ippStsNonIntelCpu || status == ippStsNotSupportedCpu) {
    Ipp64u featureMask = 0;
    IppStatus st = ippGetCpuFeatures(&featureMask, NULL);
    if (st != ippStsNoErr) {
      if (st == ippStsNotSupportedCpu) {
        LOG(WARNING) << "IPP error: not supported cpu.";
        // manual set mask
        if (ZCpuInfoInstance.bMMX)
          featureMask |= 1;
        if (ZCpuInfoInstance.bSSE)
          featureMask |= 2;
        if (ZCpuInfoInstance.bSSE2)
          featureMask |= 4;
        if (ZCpuInfoInstance.bSSE3)
          featureMask |= 8;
        if (ZCpuInfoInstance.bSSSE3)
          featureMask |= 16;
        if (ZCpuInfoInstance.bMOVBE)
          featureMask |= 32;
        if (ZCpuInfoInstance.bSSE41)
          featureMask |= 64;
        if (ZCpuInfoInstance.bSSE42)
          featureMask |= 128;
        if (ZCpuInfoInstance.bAVX)
          featureMask |= 256;
        LOG(INFO) << ippSetCpuFeatures(featureMask);
      }
    } else {
      LOG(INFO) << ippSetCpuFeatures(featureMask);
    }
  }

  // pointer to static data, no need to delete
  const IppLibraryVersion* ippVer = ippiGetLibVersion();
  LOG(INFO) << "IPP: " << ippVer->Name << " "
            << ippVer->Version << " "
            << ippVer->major << " "
            << ippVer->minor << " "
            << ippVer->majorBuild << " "
            << ippVer->build;
  LOG(INFO) << "";
#endif

  if (!ZCpuInfoInstance.bSSE3) {
    QMessageBox::critical(nullptr, "CPU not supported", "This program requires CPU with SSE3 support.");
    return app.exec();
  }

  //qApp->installEventFilter(new MacEventFilter(qApp));

  // Our MainWindow has Qt::WA_DeleteOnClose attribute, don't delete again.
  nim::ZMainWindow *mainWin = new nim::ZMainWindow();
  mainWin->show();
  mainWin->initOpenglContext();

  int result;
  try {
    result =  app.exec();
  }
  catch (const nim::ZException & e) {
    LOG(FATAL) << e.what();
  }

  fftw_cleanup_threads();
  nim::shutdownLogging();

  return result;
}
