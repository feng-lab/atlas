#include "zlog.h"
#include "zcpuinfo.h"
#include "zimginterface.h"
#include "zlogcache.h"
#include "zh5zjpegxr.h"
#include "zmkl.h"
#ifdef ZIMG_USE_FFTW
#include <fftw3.h>
#endif
#include <itkMultiThreaderBase.h>
#include <QDir>
#include <cpuinfo.h>

#ifdef ZIMG_USE_IPP
#include <ippcore.h>
#include <ippi.h>
#endif

#include <folly/Singleton.h>
#include <folly/init/Phase.h>
#include <folly/synchronization/HazptrThreadPoolExecutor.h>

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
                const QString& jreDIR,
                const QString& jarsDIR,
                const QString& logFilename,
                bool isApp,
                bool isGUIMode)
{
  initLogging(argv0, logFilename);

  ZImgGlobal::instance().isApp = isApp;

  if (isApp) {
    if (isGUIMode) {
      addLogSink(&ZLogCache::instance());
    }
    qInstallMessageHandler(myMessageOutput);
    LOG(INFO) << "--- App Log Start ---";
  }

  // if jarsDIR exist and is valid, try jreDIR first, then try JAVA_HOME
  ZImgGlobal::instance().jarsDIR = "";
  ZImgGlobal::instance().jreDIR = "";
  if (jarsDIR.isEmpty()) {
    LOG(INFO) << "no java support";
  } else {
    QDir jarsD(jarsDIR);
    if (!jarsD.exists() || !jarsD.exists("bioformats_package.jar") || !jarsD.exists("scifio-itk-bridge.jar")) {
      throw ZIOException(QString("invalid jarsDIR: %1").arg(jarsDIR));
    }
    QDir jreD;
    QString javahome = jreDIR;
    if (javahome.isEmpty()) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
      javahome = qEnvironmentVariable("JAVA_HOME");
#else
      javahome = qgetenv("JAVA_HOME");
#endif
      if (javahome.isEmpty()) {
        ZImgGlobal::instance().jarsDIR = jarsD.absolutePath();
        VLOG(1) << "jarsDIR: " << ZImgGlobal::instance().jarsDIR;
        VLOG(1) << "no jreDIR and environment variable JAVA_HOME, will try to use java in system path";
      } else {
        VLOG(1) << "try java from JAVA_HOME: " << javahome
                << ", note: might crash if the java version is not compatible";
        jreD = QDir(javahome);
      }
    } else {
      jreD = QDir(javahome);
    }

    if (!javahome.isEmpty()) {
      if (!jreD.exists() || !jreD.exists("release") || !jreD.exists("bin")) {
        throw ZIOException(QString("invalid jreDIR: %1").arg(jreD.absolutePath()));
      }
#ifdef _WIN32
      if (!jreD.exists("bin/javaw.exe"))
#else
      if (!jreD.exists("bin/java"))
#endif
      {
        throw ZIOException(QString("no java in jreDIR: %1").arg(jreD.absolutePath()));
      }

      ZImgGlobal::instance().jarsDIR = jarsD.absolutePath();
      if (isApp) {
        LOG(INFO) << "jarsDIR: " << ZImgGlobal::instance().jarsDIR;
      }
      ZImgGlobal::instance().jreDIR = jreD.absolutePath();
      if (isApp) {
        LOG(INFO) << "jreDIR: " << ZImgGlobal::instance().jreDIR;
      }
    }
  }
  ZImgGlobal::instance().resourcesDIR = resourcesDIR;
  if (isApp) {
    LOG(INFO) << "resourcesDIR: " << ZImgGlobal::instance().resourcesDIR;
  }

  if (isApp) {
    ZCpuInfo::instance().logCpuInfo();
  }

#ifdef ZIMG_USE_MKL
  if (isApp) {
    // todo: check this for amd cpu
    MKLVersion mklVer;
    MKL_Get_Version(&mklVer);
    LOG(INFO) << "MKL: " << mklVer.Platform << mklVer.Processor << " " << mklVer.MajorVersion << "."
              << mklVer.MinorVersion << "." << mklVer.UpdateVersion << ".b" << mklVer.Build;
  }
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
        if (cpuinfo_has_x86_mmx()) {
          featureMask |= ippCPUID_MMX;
        }
        if (cpuinfo_has_x86_sse()) {
          featureMask |= ippCPUID_SSE;
        }
        if (cpuinfo_has_x86_sse2()) {
          featureMask |= ippCPUID_SSE2;
        }
        if (cpuinfo_has_x86_sse3()) {
          featureMask |= ippCPUID_SSE3;
        }
        if (cpuinfo_has_x86_ssse3()) {
          featureMask |= ippCPUID_SSSE3;
        }
        if (cpuinfo_has_x86_movbe()) {
          featureMask |= ippCPUID_MOVBE;
        }
        if (cpuinfo_has_x86_sse4_1()) {
          featureMask |= ippCPUID_SSE41;
        }
        if (cpuinfo_has_x86_sse4_2()) {
          featureMask |= ippCPUID_SSE42;
        }
        if (cpuinfo_has_x86_avx()) {
          featureMask |= ippCPUID_AVX;
          featureMask |= ippAVX_ENABLEDBYOS;
        }
        if (cpuinfo_has_x86_aes()) {
          featureMask |= ippCPUID_AES;
        }
        if (cpuinfo_has_x86_pclmulqdq()) {
          featureMask |= ippCPUID_CLMUL;
        }
        if (cpuinfo_has_x86_rdrand()) {
          featureMask |= ippCPUID_RDRAND;
        }
        if (cpuinfo_has_x86_f16c()) {
          featureMask |= ippCPUID_F16C;
        }
        if (cpuinfo_has_x86_avx2()) {
          featureMask |= ippCPUID_AVX2;
        }
        if (cpuinfo_has_x86_adx()) {
          featureMask |= ippCPUID_ADCOX;
        }
        if (cpuinfo_has_x86_rdseed()) {
          featureMask |= ippCPUID_RDSEED;
        }
        if (cpuinfo_has_x86_prefetchw()) {
          featureMask |= ippCPUID_PREFETCHW;
        }
        if (cpuinfo_has_x86_sha()) {
          featureMask |= ippCPUID_SHA;
        }
        if (cpuinfo_has_x86_avx512f()) {
          featureMask |= ippCPUID_AVX512F;
        }
        if (cpuinfo_has_x86_avx512cd()) {
          featureMask |= ippCPUID_AVX512CD;
        }
        if (cpuinfo_has_x86_avx512er()) {
          featureMask |= ippCPUID_AVX512ER;
        }
        if (cpuinfo_has_x86_avx512pf()) {
          featureMask |= ippCPUID_AVX512PF;
        }
        if (cpuinfo_has_x86_avx512bw()) {
          featureMask |= ippCPUID_AVX512BW;
        }
        if (cpuinfo_has_x86_avx512dq()) {
          featureMask |= ippCPUID_AVX512DQ;
        }
        if (cpuinfo_has_x86_avx512vl()) {
          featureMask |= ippCPUID_AVX512VL;
        }
        if (cpuinfo_has_x86_avx512vbmi()) {
          featureMask |= ippCPUID_AVX512VBMI;
        }
        if (cpuinfo_has_x86_mpx()) {
          featureMask |= ippCPUID_MPX;
        }
        if (cpuinfo_has_x86_avx512_4fmaps()) {
          featureMask |= ippCPUID_AVX512_4FMADDPS;
        }
        if (cpuinfo_has_x86_avx512_4vnniw()) {
          featureMask |= ippCPUID_AVX512_4VNNIW;
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

  if (isApp) {
    itk::MultiThreaderBase::Pointer mt = itk::MultiThreaderBase::New();
    mt.Print(std::cout);
  }

  if (ZCpuInfo::instance().isX86_64 && !ZCpuInfo::instance().bAVX) {
    throw ZException("CPU not supported. This program requires CPU with AVX support.");
  }

  jpegxr_register_h5filter();

  // folly
  // Indicate ProcessPhase::Regular and register handler to
  // indicate ProcessPhase::Exit.
  folly::set_process_phases();
  // Move from the registration phase to the "you can actually instantiate
  // things now" phase.
  folly::SingletonVault::singleton()->registrationComplete();
  // Set the default hazard pointer domain to use a thread pool executor
  // for asynchronous reclamation
  folly::enable_hazptr_thread_pool_executor();
}

void shutdownImgLib()
{
  if (ZImgGlobal::instance().isApp) {
    LOG(INFO) << "--- App Log End ---";
  }

#ifdef ZIMG_USE_FFTW
  fftw_cleanup_threads();
#endif

  folly::SingletonVault::singleton()->destroyInstancesFinal();

  shutdownLogging();
}

} // namespace nim
