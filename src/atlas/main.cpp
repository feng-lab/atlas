#include <QApplication>

#include "zmainwindow.h"

#include "zlog.h"
#include "zlogmodelsink.h"
#include <QDir>
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
  case QtDebugMsg:
    LDEBUGF(context.file, context.line, context.function) << qPrintable(msg);
    break;
  case QtInfoMsg:
    LINFOF(context.file, context.line, context.function) << qPrintable(msg);
    break;
  case QtWarningMsg:
    LWARNF(context.file, context.line, context.function) << qPrintable(msg);
    break;
  case QtCriticalMsg:
    LERRORF(context.file, context.line, context.function) << qPrintable(msg);
    break;
  case QtFatalMsg:
    LFATALF(context.file, context.line, context.function) << qPrintable(msg);
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
  nim::initLogging(ZSystemInfoInstance.logDir().filePath("atlas_log.txt"));
  nim::addLogSink(nim::logModelSinkInstance());

  qInstallMessageHandler(myMessageOutput);

  LINFO() << "--- App Log Started ---";
  ZSystemInfoInstance.logOSInfo();
  ZCpuInfoInstance.logCpuInfo();
  fftw_init_threads();
  fftw_plan_with_nthreads(ZCpuInfoInstance.nPhysicalCores);

#ifdef _USE_MKL_
  // todo: check this for amd cpu
  MKLVersion mklVer;
  MKL_Get_Version(&mklVer);
  LINFO() << "MKL:" << mklVer.Platform << mklVer.Processor << mklVer.MajorVersion << mklVer.MinorVersion << mklVer.UpdateVersion << mklVer.Build;
  LINFO() << "";
#endif

#ifdef _USE_IPP_
  // todo: check this for amd cpu
  IppStatus status = ippInit();
  if (status == ippStsNonIntelCpu || status == ippStsNotSupportedCpu) {
    Ipp64u featureMask = 0;
    IppStatus st = ippGetCpuFeatures(&featureMask, NULL);
    if (st != ippStsNoErr) {
      if (st == ippStsNotSupportedCpu) {
        LWARN() << "IPP error: not supported cpu.";
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
        LINFO() << ippSetCpuFeatures(featureMask);
      }
    } else {
      LINFO() << ippSetCpuFeatures(featureMask);
    }
  }

  // pointer to static data, no need to delete
  const IppLibraryVersion* ippVer = ippiGetLibVersion();
  LINFO() << "IPP:" << ippVer->Name << ippVer->Version << ippVer->major << ippVer->minor << ippVer->majorBuild << ippVer->build;
  LINFO() << "";
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
    LFATAL() << e.what();
  }

  fftw_cleanup_threads();

  return result;
}
