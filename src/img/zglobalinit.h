#pragma once

#include "zlog.h"
#include "zcpuinfo.h"
#include <fftw3.h>
#include <mkl_service.h>
#include <itkMultiThreaderBase.h>

#ifdef ZIMG_USE_IPP
#include <ippcore.h>
#include <ippi.h>
#endif

namespace nim {

void initImgLib(const char* argv0, const QString& logFilename = "")
{
  initLogging(argv0, logFilename);

  ZCpuInfo::instance().logCpuInfo();

  fftw_init_threads();
  fftw_plan_with_nthreads(ZCpuInfo::instance().nPhysicalCores);

  // todo: check this for amd cpu
  MKLVersion mklVer;
  MKL_Get_Version(&mklVer);
  LOG(INFO) << "MKL: " << mklVer.Platform << mklVer.Processor << " "
            << mklVer.MajorVersion << "."
            << mklVer.MinorVersion << "."
            << mklVer.UpdateVersion << ".b"
            << mklVer.Build;

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

  itk::MultiThreaderBase::Pointer mt = itk::MultiThreaderBase::New();
  mt.Print(std::cout);

  if (!ZCpuInfo::instance().bSSE3) {
    LOG(FATAL) << "CPU not supported. This program requires CPU with SSE3 support.";
  }
}

void shutdownImgLib()
{
  fftw_cleanup_threads();

  shutdownLogging();
}

} // namespace nim

