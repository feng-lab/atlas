#include "zlog.h"
#include "zcpuinfo.h"
#include "zimginterface.h"
#include "zlogcache.h"
#ifdef ZIMG_USE_MKL
#include <mkl_service.h>
#endif
#ifdef ZIMG_USE_FFTW
#include <fftw3.h>
#endif
#include <itkMultiThreaderBase.h>
#include <QDir>

#ifdef ZIMG_USE_IPP
#include <ippcore.h>
#include <ippi.h>
#endif

namespace nim {

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

void initImgLib(const char* argv0,
                const QString& resourcesDIR,
                const QString& jdkDIR,
                const QString& jarsDIR,
                const QString& logFilename,
                bool isApp)
{
  initLogging(argv0, logFilename);

  if (isApp) {
    addLogSink(&ZLogCache::instance());
    qInstallMessageHandler(myMessageOutput);
    LOG(INFO) << "--- App Log Start ---";
  }

  ZCpuInfo::instance().logCpuInfo();

  // if jarsDIR exist and is valid, try jdkDIR first, then try JAVA_HOME
  ZImgGlobal::instance().jarsDIR = "";
  ZImgGlobal::instance().jdkDIR = "";
  if (jarsDIR.isEmpty()) {
    LOG(INFO) << "no java support";
  } else {
    QDir jarsD(jarsDIR);
    if (!jarsD.exists() || !jarsD.exists("bioformats_package.jar") || !jarsD.exists("scifio-itk-bridge.jar")) {
      throw ZIOException(QString("invalid jarsDIR: %1").arg(jarsDIR));
    }
    QDir jdkD;
    QString javahome = jdkDIR;
    if (javahome.isEmpty()) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
      javahome = qEnvironmentVariable("JAVA_HOME");
#else
      javahome = qgetenv("JAVA_HOME");
#endif
      if (javahome.isEmpty()) {
        ZImgGlobal::instance().jarsDIR = jarsD.absolutePath();
        LOG(INFO) << "jarsDIR: " << ZImgGlobal::instance().jarsDIR;
        LOG(INFO) << "no jdkDIR and enviroment variable JAVA_HOME, will try to use java in system path";
      } else {
        LOG(INFO) << "try java from JAVA_HOME: " << javahome << ", note: might crash if the java version is too low";
        jdkD = QDir(javahome);
      }
    } else {
      jdkD = QDir(javahome);
    }

    if (!javahome.isEmpty()) {
      if (!jdkD.exists() || !jdkD.exists("release") || !jdkD.exists("bin")) {
        throw ZIOException(QString("invalid jdkDIR: %1").arg(jdkD.absolutePath()));
      }
#ifdef _WIN32
      if (!jdkD.exists("bin/javaw.exe"))
#else
      if (!jdkD.exists("bin/java"))
#endif
      {
        throw ZIOException(QString("no java in jdkDIR: %1").arg(jdkD.absolutePath()));
      }

      ZImgGlobal::instance().jarsDIR = jarsD.absolutePath();
      LOG(INFO) << "jarsDIR: " << ZImgGlobal::instance().jarsDIR;
      ZImgGlobal::instance().jdkDIR = jdkD.absolutePath();
      LOG(INFO) << "jdkDIR: " << ZImgGlobal::instance().jdkDIR;
    }
  }
  ZImgGlobal::instance().resourcesDIR = resourcesDIR;
  LOG(INFO) << "resourcesDIR: " << ZImgGlobal::instance().resourcesDIR;

#ifdef ZIMG_USE_MKL
  // todo: check this for amd cpu
  MKLVersion mklVer;
  MKL_Get_Version(&mklVer);
  LOG(INFO) << "MKL: " << mklVer.Platform << mklVer.Processor << " " << mklVer.MajorVersion << "."
            << mklVer.MinorVersion << "." << mklVer.UpdateVersion << ".b" << mklVer.Build;
#endif

#ifdef ZIMG_USE_FFTW
  fftw_init_threads();
  fftw_plan_with_nthreads(ZCpuInfo::instance().nPhysicalCores);
#endif

#ifdef ZIMG_USE_IPP
  // todo: check this for amd cpu
  IppStatus status = ippInit();
  if (status == ippStsNonIntelCpu || status == ippStsNotSupportedCpu) {
    Ipp64u featureMask = 0;
    IppStatus st = ippGetCpuFeatures(&featureMask, nullptr);
    if (st != ippStsNoErr) {
      if (st == ippStsNotSupportedCpu) {
        LOG(WARNING) << "IPP error: not supported cpu.";
        // manual set mask
        if (nim::ZCpuInfo::instance().bMMX) {
          featureMask |= ippCPUID_MMX;
        }
        if (nim::ZCpuInfo::instance().bSSE) {
          featureMask |= ippCPUID_SSE;
        }
        if (nim::ZCpuInfo::instance().bSSE2) {
          featureMask |= ippCPUID_SSE2;
        }
        if (nim::ZCpuInfo::instance().bSSE3) {
          featureMask |= ippCPUID_SSE3;
        }
        if (nim::ZCpuInfo::instance().bSSSE3) {
          featureMask |= ippCPUID_SSSE3;
        }
        if (nim::ZCpuInfo::instance().bMOVBE) {
          featureMask |= ippCPUID_MOVBE;
        }
        if (nim::ZCpuInfo::instance().bSSE41) {
          featureMask |= ippCPUID_SSE41;
        }
        if (nim::ZCpuInfo::instance().bSSE42) {
          featureMask |= ippCPUID_SSE42;
        }
        if (nim::ZCpuInfo::instance().bAVX) {
          featureMask |= ippCPUID_AVX;
          featureMask |= ippAVX_ENABLEDBYOS;
        }
        if (nim::ZCpuInfo::instance().bAESNI) {
          featureMask |= ippCPUID_AES;
        }
        if (nim::ZCpuInfo::instance().bPCLMULQDQ) {
          featureMask |= ippCPUID_CLMUL;
        }
        if (nim::ZCpuInfo::instance().bRDRAND) {
          featureMask |= ippCPUID_RDRAND;
        }
        if (nim::ZCpuInfo::instance().bF16C) {
          featureMask |= ippCPUID_F16C;
        }
        if (nim::ZCpuInfo::instance().bAVX2) {
          featureMask |= ippCPUID_AVX2;
        }
        if (nim::ZCpuInfo::instance().bADX) {
          featureMask |= ippCPUID_ADCOX;
        }
        if (nim::ZCpuInfo::instance().bRDSEED) {
          featureMask |= ippCPUID_RDSEED;
        }
        if (nim::ZCpuInfo::instance().bPREFTEHCHW) {
          featureMask |= ippCPUID_PREFETCHW;
        }
        if (nim::ZCpuInfo::instance().bSHA) {
          featureMask |= ippCPUID_SHA;
        }
        if (nim::ZCpuInfo::instance().bAVX512F) {
          featureMask |= ippCPUID_AVX512F;
        }
        if (nim::ZCpuInfo::instance().bAVX512CD) {
          featureMask |= ippCPUID_AVX512CD;
        }
        if (nim::ZCpuInfo::instance().bAVX512ER) {
          featureMask |= ippCPUID_AVX512ER;
        }
        if (nim::ZCpuInfo::instance().bAVX512PF) {
          featureMask |= ippCPUID_AVX512PF;
        }
        if (nim::ZCpuInfo::instance().bAVX512BW) {
          featureMask |= ippCPUID_AVX512BW;
        }
        if (nim::ZCpuInfo::instance().bAVX512DQ) {
          featureMask |= ippCPUID_AVX512DQ;
        }
        if (nim::ZCpuInfo::instance().bAVX512VL) {
          featureMask |= ippCPUID_AVX512VL;
        }
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

  itk::MultiThreaderBase::Pointer mt = itk::MultiThreaderBase::New();
  mt.Print(std::cout);

  if (!ZCpuInfo::instance().bAVX) {
    LOG(FATAL) << "CPU not supported. This program requires CPU with AVX support.";
  }
}

void shutdownImgLib(bool isApp)
{
  if (isApp) {
    LOG(INFO) << "--- App Log End ---";
  }

#ifdef ZIMG_USE_FFTW
  fftw_cleanup_threads();
#endif

  shutdownLogging();
}

} // namespace nim
