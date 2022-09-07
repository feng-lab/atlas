#include "zfft.h"

#ifdef ZIMG_USE_MKL
#include <mkl.h>
#include <mkl_dfti.h>
#endif
#ifdef ZIMG_USE_FFTW
#include <fftw3.h>
#endif
#include <pocketfft_hdronly.h>
#include <folly/ScopeGuard.h>
// #include <tbb/global_control.h>
#include <thread>

DEFINE_uint32(zimg_global_fft_number_of_threads,
              0,
              "Number of threads fft will use, default is 0 which is hardware concurrency.");

DEFINE_bool(zimg_use_mkl_for_fft_if_available,
            true,
            "Whether to use mkl for fft computation if available, default is true");

DEFINE_bool(zimg_use_fftw_for_fft_if_available,
            true,
            "Whether to use fftw for fft computation if available, default is true");

namespace {

#ifdef ZIMG_USE_MKL

inline void MKL_DFTI_CHECK(MKL_LONG status)
{
  if (status && !DftiErrorClass(status, DFTI_NO_ERROR)) {
    throw nim::ZException(fmt::format("MKL FFT error: {}", DftiErrorMessage(status)));
  }
}

#endif

} // namespace

namespace nim {

ZComplexImg fft(const ZImg& img, size_t outWidth, size_t outHeight, size_t outDepth)
{
  if (img.isEmpty() || img.numChannels() != 1 || img.numTimes() != 1) {
    throw ZImgException(QString("fft: input img dimension is not supported: <%1>").arg(img.info().toQString()));
  }

  ZComplexImg res;
  if (img.isEmpty()) {
    return res;
  }

  size_t width = outWidth / 2 + 1;
  ZComplexImg tmp(width, outHeight, outDepth);
  res.swap(tmp);

  auto nthreads = FLAGS_zimg_global_fft_number_of_threads;
  nthreads = nthreads ? nthreads : std::thread::hardware_concurrency();

#ifdef ZIMG_USE_MKL
  if (FLAGS_zimg_use_mkl_for_fft_if_available) {
    ZImg wrapImg;
    // reinterpret_cast allowed (section "Complex numbers")
    wrapImg.wrapData(reinterpret_cast<double*>(res.rawData()), width * 2, outHeight, outDepth);
    wrapImg.pasteImg(img);
    wrapImg.clear(); // copy data finished

    // mkl_domain_set_num_threads(nthreads, MKL_DOMAIN_FFT);
    // tbb::global_control gc(tbb::global_control::max_allowed_parallelism, nthreads);
    MKL_LONG dims[] = {static_cast<MKL_LONG>(outDepth),
                       static_cast<MKL_LONG>(outHeight),
                       static_cast<MKL_LONG>(outWidth)};
    DFTI_DESCRIPTOR_HANDLE descHandle;
    MKL_DFTI_CHECK(DftiCreateDescriptor(&descHandle, DFTI_DOUBLE, DFTI_REAL, 3, dims));
    auto descHandleGuard = folly::makeGuard([&descHandle]() {
      DftiFreeDescriptor(&descHandle);
    });
    MKL_DFTI_CHECK(DftiSetValue(descHandle, DFTI_THREAD_LIMIT, FLAGS_zimg_global_fft_number_of_threads));
    MKL_DFTI_CHECK(DftiSetValue(descHandle, DFTI_CONJUGATE_EVEN_STORAGE, DFTI_COMPLEX_COMPLEX));
    MKL_LONG rstrides[] = {0, 2 * dims[1] * (dims[2] / 2 + 1), 2 * (dims[2] / 2 + 1), 1};
    MKL_DFTI_CHECK(DftiSetValue(descHandle, DFTI_INPUT_STRIDES, rstrides));
    MKL_LONG cstrides[] = {0, dims[1] * (dims[2] / 2 + 1), dims[2] / 2 + 1, 1};
    MKL_DFTI_CHECK(DftiSetValue(descHandle, DFTI_OUTPUT_STRIDES, cstrides));
    MKL_DFTI_CHECK(DftiCommitDescriptor(descHandle));
    MKL_DFTI_CHECK(DftiComputeForward(descHandle, res.rawData()));

    return res;
  }
#endif

#ifdef ZIMG_USE_FFTW
  if (FLAGS_zimg_use_fftw_for_fft_if_available) {
    // from fftw benchmark, 3d inplace is faster than outofplace, so we
    // first copy real data to complex img
    ZImg wrapImg;
    // reinterpret_cast allowed (section "Complex numbers")
    wrapImg.wrapData(reinterpret_cast<double*>(res.rawData()), width * 2, outHeight, outDepth);
    wrapImg.pasteImg(img);
    wrapImg.clear(); // copy data finished

    // do fft
    fftw_plan_with_nthreads(nthreads);
    fftw_plan p = fftw_plan_dft_r2c_3d(outDepth,
                                       outHeight,
                                       outWidth,
                                       reinterpret_cast<double*>(res.rawData()),
                                       reinterpret_cast<fftw_complex*>(res.rawData()),
                                       FFTW_ESTIMATE);
    fftw_execute(p);
    fftw_destroy_plan(p);

    return res;
  }
#endif

  ZImgInfo expandedInfo(outWidth, outHeight, outDepth, 1, 1, 8, VoxelFormat::Float);
  ZImg expandedImg(expandedInfo);
  expandedImg.pasteImg(img);
  pocketfft::r2c({expandedInfo.depth, expandedInfo.height, expandedInfo.width},
                 {static_cast<ptrdiff_t>(expandedInfo.planeByteNumber()),
                  static_cast<ptrdiff_t>(expandedInfo.rowByteNumber()),
                  static_cast<ptrdiff_t>(expandedInfo.voxelByteNumber())},
                 {static_cast<ptrdiff_t>(res.planeByteNumber()),
                  static_cast<ptrdiff_t>(res.rowByteNumber()),
                  static_cast<ptrdiff_t>(res.voxelByteNumber())},
                 {0, 1, 2},
                 true,
                 expandedImg.channelData<double>(0),
                 res.rawData(),
                 1.,
                 nthreads);
  return res;
}

ZImg ifft(ZComplexImg& cimg, size_t width, size_t outWidth, size_t outHeight, size_t outDepth)
{
  ZImg res;
  if (cimg.isEmpty()) {
    return res;
  }

  auto nthreads = FLAGS_zimg_global_fft_number_of_threads;
  nthreads = nthreads ? nthreads : std::thread::hardware_concurrency();

#ifdef ZIMG_USE_MKL
  if (FLAGS_zimg_use_mkl_for_fft_if_available) {
    // mkl_domain_set_num_threads(nthreads, MKL_DOMAIN_FFT);
    // tbb::global_control gc(tbb::global_control::max_allowed_parallelism, nthreads);
    MKL_LONG dims[] = {static_cast<MKL_LONG>(cimg.depth()),
                       static_cast<MKL_LONG>(cimg.height()),
                       static_cast<MKL_LONG>(width)};
    DFTI_DESCRIPTOR_HANDLE descHandle;
    MKL_DFTI_CHECK(DftiCreateDescriptor(&descHandle, DFTI_DOUBLE, DFTI_REAL, 3, dims));
    auto descHandleGuard = folly::makeGuard([&descHandle]() {
      DftiFreeDescriptor(&descHandle);
    });
    MKL_DFTI_CHECK(DftiSetValue(descHandle, DFTI_BACKWARD_SCALE, 1. / (width * cimg.height() * cimg.depth())));
    MKL_DFTI_CHECK(DftiSetValue(descHandle, DFTI_THREAD_LIMIT, FLAGS_zimg_global_fft_number_of_threads));
    MKL_DFTI_CHECK(DftiSetValue(descHandle, DFTI_CONJUGATE_EVEN_STORAGE, DFTI_COMPLEX_COMPLEX));
    MKL_LONG rstrides[] = {0, 2 * dims[1] * (dims[2] / 2 + 1), 2 * (dims[2] / 2 + 1), 1};
    MKL_LONG cstrides[] = {0, dims[1] * (dims[2] / 2 + 1), dims[2] / 2 + 1, 1};
    MKL_DFTI_CHECK(DftiSetValue(descHandle, DFTI_INPUT_STRIDES, cstrides));
    MKL_DFTI_CHECK(DftiSetValue(descHandle, DFTI_OUTPUT_STRIDES, rstrides));
    MKL_DFTI_CHECK(DftiCommitDescriptor(descHandle));
    MKL_DFTI_CHECK(DftiComputeBackward(descHandle, cimg.rawData()));

    ZImg wrapImg;
    wrapImg.wrapData(reinterpret_cast<double*>(cimg.rawData()), cimg.width() * 2, cimg.height(), cimg.depth());

    ZImgRegion region(0, outWidth, 0, outHeight, 0, outDepth);
    res = wrapImg.crop(region);
    cimg.clear();

    return res;
  }
#endif

#ifdef ZIMG_USE_FFTW
  if (FLAGS_zimg_use_fftw_for_fft_if_available) {
    fftw_plan_with_nthreads(nthreads);
    fftw_plan p = fftw_plan_dft_c2r_3d(cimg.depth(),
                                       cimg.height(),
                                       width,
                                       reinterpret_cast<fftw_complex*>(cimg.rawData()),
                                       reinterpret_cast<double*>(cimg.rawData()),
                                       FFTW_ESTIMATE);
    fftw_execute(p);
    fftw_destroy_plan(p);

    ZImg wrapImg;
    wrapImg.wrapData(reinterpret_cast<double*>(cimg.rawData()), cimg.width() * 2, cimg.height(), cimg.depth());

    ZImgRegion region(0, outWidth, 0, outHeight, 0, outDepth);
    res = wrapImg.crop(region);
    res *= 1. / (width * cimg.height() * cimg.depth());
    cimg.clear();

    return res;
  }
#endif

  ZImgInfo outImgInfo(width, cimg.height(), cimg.depth(), 1, 1, 8, VoxelFormat::Float);
  ZImg outImg(outImgInfo);
  pocketfft::c2r({outImgInfo.depth, outImgInfo.height, outImgInfo.width},
                 {static_cast<ptrdiff_t>(cimg.planeByteNumber()),
                  static_cast<ptrdiff_t>(cimg.rowByteNumber()),
                  static_cast<ptrdiff_t>(cimg.voxelByteNumber())},
                 {static_cast<ptrdiff_t>(outImgInfo.planeByteNumber()),
                  static_cast<ptrdiff_t>(outImgInfo.rowByteNumber()),
                  static_cast<ptrdiff_t>(outImgInfo.voxelByteNumber())},
                 {0, 1, 2},
                 false,
                 cimg.rawData(),
                 outImg.channelData<double>(0),
                 1. / outImgInfo.voxelNumber(),
                 nthreads);
  ZImgRegion region(0, outWidth, 0, outHeight, 0, outDepth);
  res = outImg.crop(region);
  return res;
}

} // namespace nim
