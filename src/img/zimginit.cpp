#include "zimginit.h"
#include "zlog.h"
#include "zbioformatsbridgeclient.h"
#include "zcpuinfo.h"
#include "zimginterface.h"
#include "zh5zjpegxr.h"
#include "zmkl.h"
#ifdef ZIMG_USE_FFTW
#include <fftw3.h>
#endif
#include <itkMultiThreaderBase.h>
#include <QDir>
#include <QFileInfo>
#include <cpuinfo.h>

#include <folly/Singleton.h>
#include <folly/init/Phase.h>
#include <folly/synchronization/HazptrThreadPoolExecutor.h>

namespace nim {

namespace {

constexpr const char* kBioFormatsJar = "bioformats_package.jar";
constexpr const char* kAtlasBioFormatsBridgeJar = "atlas-bioformats-bridge.jar";
#ifdef _WIN32
constexpr const char* kJavaExecutable = "bin/java.exe";
#else
constexpr const char* kJavaExecutable = "bin/java";
#endif

} // namespace

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

  if (jarsDIR.isEmpty()) {
    LOG(INFO) << "no java support";
  } else {
    QDir jarsD(jarsDIR);
    if (!jarsD.exists() || !jarsD.exists(kAtlasBioFormatsBridgeJar)) {
      throw ZException(fmt::format("invalid jarsDIR: {}", jarsDIR));
    }
    const QString bridgeJarPath = jarsD.absoluteFilePath(kAtlasBioFormatsBridgeJar);
    const QString bioFormatsJarPath = jarsD.exists(kBioFormatsJar) ? jarsD.absoluteFilePath(kBioFormatsJar) : QString();
    if (verbose) {
      LOG(INFO) << "jarsDIR: " << jarsD.absolutePath();
      LOG(INFO) << "Bio-Formats bridge jar: " << bridgeJarPath;
      if (bioFormatsJarPath.isEmpty()) {
        LOG(INFO) << "bioformats jar: not configured";
      } else {
        LOG(INFO) << "bioformats jar: " << bioFormatsJarPath;
      }
    }

    ZBioFormatsBridgeClient::configureBridgeJarPath(bridgeJarPath);
    if (!bioFormatsJarPath.isEmpty()) {
      ZBioFormatsBridgeClient::configureBioFormatsJarPath(bioFormatsJarPath);
    }

    if (jreDIR.isEmpty()) {
      LOG(INFO) << "no bundled jreDIR; Bio-Formats Java executable is not configured";
    } else {
      QDir jreD(jreDIR);
      if (!jreD.exists() || !jreD.exists("bin")) {
        throw ZException(fmt::format("invalid jreDIR: {}", jreD.absolutePath()));
      }
      if (!jreD.exists(kJavaExecutable)) {
        throw ZException(fmt::format("no java in jreDIR: {}", jreD.absolutePath()));
      }

      ZBioFormatsBridgeClient::configureJavaExecutablePath(jreD.absoluteFilePath(kJavaExecutable));
      if (verbose) {
        LOG(INFO) << "jreDIR: " << jreD.absolutePath();
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

#if ZIMG_MKL_ENABLED
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

  // Do not manually tear down Folly singletons here.
  //
  // Folly already registers an atexit handler to destroy its singleton vault.
  // Calling destroyInstancesFinal() from this destructor can run before other
  // subsystems (e.g. the HTTP client/event-base thread) have fully shut down,
  // which can lead to use-after-destroy crashes in background threads.
}

} // namespace nim
