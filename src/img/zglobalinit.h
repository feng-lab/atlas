#pragma once

#include "zlog.h"
#include "zcpuinfo.h"
#include <fftw3.h>
#include <mkl_service.h>
#include <itkMultiThreaderBase.h>

namespace nim {

void initImgLib()
{
  initLogging("zimg");

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

