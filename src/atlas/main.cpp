#include "zapplication.h"

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
#include <folly/ScopeGuard.h>

#ifdef ATLAS_USE_MKL

#include "mkl_service.h"

#endif

#ifdef ATLAS_USE_IPP

#include "ippcore.h"
#include "ippi.h"

#endif

#include <QSurfaceFormat>

#include <QStack>
#include <QPointer>

using namespace nim;

// thanks to Daniel Price for this workaround
struct MacEventFilter : public QObject
{
  QStack<QPointer<QWidget>> m_activationstack; // stack of widgets to re-active on dialog close.

  explicit MacEventFilter(QObject* parent = nullptr)
    : QObject(parent)
  {}

  virtual bool eventFilter(QObject* anObject, QEvent* anEvent)
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

void myMessageOutput(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
  switch (type) {
    case QtDebugMsg:
      LWARNF(context.file ? context.file : "QtFile", context.line) << msg;
      break;
    case QtInfoMsg:
      LINFOF(context.file ? context.file : "QtFile", context.line) << msg;
      break;
    case QtWarningMsg:
      LWARNF(context.file ? context.file : "QtFile", context.line) << msg;
      break;
    case QtCriticalMsg:
      LERRORF(context.file ? context.file : "QtFile", context.line) << msg;
      break;
    case QtFatalMsg:
      LFATALF(context.file ? context.file : "QtFile", context.line) << msg;
      break;
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
  for (int i = 0; i < list.size() - numberToKeep; ++i) {
    QDir logDir(list.at(i).absoluteFilePath());
    logDir.removeRecursively();
  }
}

int main(int argc, char* argv[])
{
  QSurfaceFormat format;
#if defined(__APPLE__) && defined(ATLAS_USE_CORE_PROFILE)
  format.setVersion(3, 2);
  format.setProfile(QSurfaceFormat::CoreProfile);
#endif
  //format.setStereo(true);
  QSurfaceFormat::setDefaultFormat(format);

  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts, true);
  QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);
  nim::ZApplication app(argc, argv);
  app.setApplicationName("Atlas");
  app.setOrganizationDomain("atlas.com");
  app.setOrganizationName("Atlas");

  // init the logging mechanism
  QDir logDir = nim::ZSystemInfo::instance().logDir();
  removeOldLogs(logDir);

  nim::initLogging(argv[0], logDir.filePath("atlas"));
  folly::ScopeGuard guardlogging = folly::makeGuard([]() {
    LOG(INFO) << "--- App Log End ---";
    nim::shutdownLogging();
  });
  Q_UNUSED(guardlogging)

  nim::addLogSink(&nim::ZLogModelSink::instance());
  qInstallMessageHandler(myMessageOutput);

  try {
    LOG(INFO) << "--- App Log Start ---";
    nim::ZSystemInfo::instance().logOSInfo();
    nim::ZCpuInfo::instance().logCpuInfo();

    fftw_init_threads();
    folly::ScopeGuard guardfftw = folly::makeGuard([]() {
      fftw_cleanup_threads();
    });
    Q_UNUSED(guardfftw)

    fftw_plan_with_nthreads(nim::ZCpuInfo::instance().nPhysicalCores);

#ifdef ATLAS_USE_MKL
    // todo: check this for amd cpu
    MKLVersion mklVer;
    MKL_Get_Version(&mklVer);
    LOG(INFO) << "MKL: " << mklVer.Platform << mklVer.Processor << " "
              << mklVer.MajorVersion << "."
              << mklVer.MinorVersion << "."
              << mklVer.UpdateVersion << ".b"
              << mklVer.Build;
#endif

#ifdef ATLAS_USE_IPP
    // todo: check this for amd cpu
    IppStatus status = ippInit();
    if (status == ippStsNonIntelCpu || status == ippStsNotSupportedCpu) {
      Ipp64u featureMask = 0;
      IppStatus st = ippGetCpuFeatures(&featureMask, nullptr);
      if (st != ippStsNoErr) {
        if (st == ippStsNotSupportedCpu) {
          LOG(WARNING) << "IPP error: not supported cpu.";
          // manual set mask
          if (nim::ZCpuInfo::instance().bMMX)
            featureMask |= ippCPUID_MMX;
          if (nim::ZCpuInfo::instance().bSSE)
            featureMask |= ippCPUID_SSE;
          if (nim::ZCpuInfo::instance().bSSE2)
            featureMask |= ippCPUID_SSE2;
          if (nim::ZCpuInfo::instance().bSSE3)
            featureMask |= ippCPUID_SSE3;
          if (nim::ZCpuInfo::instance().bSSSE3)
            featureMask |= ippCPUID_SSSE3;
          if (nim::ZCpuInfo::instance().bMOVBE)
            featureMask |= ippCPUID_MOVBE;
          if (nim::ZCpuInfo::instance().bSSE41)
            featureMask |= ippCPUID_SSE41;
          if (nim::ZCpuInfo::instance().bSSE42)
            featureMask |= ippCPUID_SSE42;
          if (nim::ZCpuInfo::instance().bAVX) {
            featureMask |= ippCPUID_AVX;
            featureMask |= ippAVX_ENABLEDBYOS;
          }
          if (nim::ZCpuInfo::instance().bAESNI)
            featureMask |= ippCPUID_AES;
          if (nim::ZCpuInfo::instance().bPCLMULQDQ)
            featureMask |= ippCPUID_CLMUL;
          if (nim::ZCpuInfo::instance().bRDRAND)
            featureMask |= ippCPUID_RDRAND;
          if (nim::ZCpuInfo::instance().bF16C)
            featureMask |= ippCPUID_F16C;
          if (nim::ZCpuInfo::instance().bAVX2)
            featureMask |= ippCPUID_AVX2;
          if (nim::ZCpuInfo::instance().bADX)
            featureMask |= ippCPUID_ADCOX;
          if (nim::ZCpuInfo::instance().bRDSEED)
            featureMask |= ippCPUID_RDSEED;
          if (nim::ZCpuInfo::instance().bPREFTEHCHW)
            featureMask |= ippCPUID_PREFETCHW;
          if (nim::ZCpuInfo::instance().bSHA)
            featureMask |= ippCPUID_SHA;
          if (nim::ZCpuInfo::instance().bAVX512F)
            featureMask |= ippCPUID_AVX512F;
          if (nim::ZCpuInfo::instance().bAVX512CD)
            featureMask |= ippCPUID_AVX512CD;
          if (nim::ZCpuInfo::instance().bAVX512ER)
            featureMask |= ippCPUID_AVX512ER;
          if (nim::ZCpuInfo::instance().bAVX512PF)
            featureMask |= ippCPUID_AVX512PF;
          if (nim::ZCpuInfo::instance().bAVX512BW)
            featureMask |= ippCPUID_AVX512BW;
          if (nim::ZCpuInfo::instance().bAVX512DQ)
            featureMask |= ippCPUID_AVX512DQ;
          if (nim::ZCpuInfo::instance().bAVX512VL)
            featureMask |= ippCPUID_AVX512VL;
          LOG(INFO) << ippSetCpuFeatures(featureMask);
        }
      } else {
        LOG(INFO) << ippSetCpuFeatures(featureMask);
      }
    }

    // pointer to static data, no need to delete
    const IppLibraryVersion* ippVer = ippiGetLibVersion();
    LOG(INFO) << "IPP: " << ippVer->Name << " " << ippVer->Version;
#endif

    if (!nim::ZCpuInfo::instance().bSSE3) {
      QMessageBox::critical(nullptr, app.applicationName(),
                            "CPU not supported.\nThis program requires CPU with SSE3 support. Click OK to exit.");
      LOG(ERROR) << "CPU not supported";
      return 1;
    }

    //qApp->installEventFilter(new MacEventFilter(qApp));

    // Our MainWindow has Qt::WA_DeleteOnClose attribute, don't delete again.
    nim::ZMainWindow* mainWin = new nim::ZMainWindow();
    mainWin->show();
    mainWin->initOpenglContext();

    return app.exec();
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
