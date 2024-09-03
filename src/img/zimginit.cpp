#include "zimginit.h"
#include "zlog.h"
#include "zcpuinfo.h"
#include "zimginterface.h"
#include "zh5zjpegxr.h"
#include "zmkl.h"
#ifdef ZIMG_USE_FFTW
#include <fftw3.h>
#endif
#include <itkMultiThreaderBase.h>
#include <QDir>
#include <cpuinfo.h>

#include <folly/Singleton.h>
#include <folly/init/Phase.h>
#include <folly/synchronization/HazptrThreadPoolExecutor.h>

namespace nim {

const ZImgInit&
ZImgInit::instance(const QString& resourcesDIR, const QString& jreDIR, const QString& jarsDIR, bool verbose)
{
  static ZImgInit imgInit(resourcesDIR, jreDIR, jarsDIR, verbose);
  return imgInit;
}

ZImgInit::ZImgInit(const QString& resourcesDIR, const QString& jreDIR, const QString& jarsDIR, bool verbose)
{
  if (!google::IsGoogleLoggingInitialized()) {
    ZLogInit::instance("zimg"s);
    LOG(WARNING) << "glog is not initialized, initialize it now";
  }

  // if jarsDIR exist and is valid, try jreDIR first, then try JAVA_HOME
  ZImgGlobal::instance().jarsDIR = "";
  ZImgGlobal::instance().jreDIR = "";
  if (jarsDIR.isEmpty()) {
    LOG(INFO) << "no java support";
  } else {
    QDir jarsD(jarsDIR);
    if (!jarsD.exists() || !jarsD.exists("bioformats_package.jar") || !jarsD.exists("scifio-itk-bridge.jar")) {
      throw ZException(fmt::format("invalid jarsDIR: {}", jarsDIR));
    }
    QDir jreD;
    QString javahome = jreDIR;
    if (javahome.isEmpty()) {
      javahome = qEnvironmentVariable("JAVA_HOME");
      if (javahome.isEmpty()) {
        ZImgGlobal::instance().jarsDIR = jarsD.absolutePath();
        LOG(INFO) << "jarsDIR: " << ZImgGlobal::instance().jarsDIR;
        LOG(INFO) << "no jreDIR and environment variable JAVA_HOME, will try to use java in system path";
      } else {
        LOG(INFO) << "try java from JAVA_HOME: " << javahome
                  << ", note: might crash if the java version is not compatible";
        jreD = QDir(javahome);
      }
    } else {
      jreD = QDir(javahome);
    }

    if (!javahome.isEmpty()) {
      if (!jreD.exists() || !jreD.exists("release") || !jreD.exists("bin")) {
        throw ZException(fmt::format("invalid jreDIR: {}", jreD.absolutePath()));
      }
#ifdef _WIN32
      if (!jreD.exists("bin/javaw.exe"))
#else
      if (!jreD.exists("bin/java"))
#endif
      {
        throw ZException(fmt::format("no java in jreDIR: {}", jreD.absolutePath()));
      }

      ZImgGlobal::instance().jarsDIR = jarsD.absolutePath();
      if (verbose) {
        LOG(INFO) << "jarsDIR: " << ZImgGlobal::instance().jarsDIR;
      }
      ZImgGlobal::instance().jreDIR = jreD.absolutePath();
      if (verbose) {
        LOG(INFO) << "jreDIR: " << ZImgGlobal::instance().jreDIR;
      }
    }
  }
  ZImgGlobal::instance().resourcesDIR = resourcesDIR;
  if (verbose) {
    LOG(INFO) << "resourcesDIR: " << ZImgGlobal::instance().resourcesDIR;
  }

  if (verbose) {
    ZCpuInfo::instance().logCpuInfo();
  }

#ifdef ZIMG_USE_MKL
  if (verbose) {
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

  if (verbose) {
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

ZImgInit::~ZImgInit()
{
#ifdef ZIMG_USE_FFTW
  fftw_cleanup_threads();
#endif

  folly::SingletonVault::singleton()->destroyInstancesFinal();
}

} // namespace nim
