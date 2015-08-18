#include "zfft.h"
#include <fftw3.h>

namespace nim {

ZComplexImg fft(const ZImg &img, size_t outWidth, size_t outHeight, size_t outDepth)
{
  if (img.isEmpty() || img.numChannels() != 1 || img.numTimes() != 1) {
    throw ZImgException(QString("fft: input img dimension is not supported: <%1>")
                        .arg(img.info().toQString()));
  }

  ZComplexImg res;
  if (img.isEmpty())
    return res;

  size_t width = outWidth / 2 + 1;
  ZComplexImg tmp(width, outHeight, outDepth);
  res.swap(tmp);

  // from fftw benchmark, 3d inplace is faster than outofplace, so we
  // first copy real data to complex img
  ZImg wrapImg;
  wrapImg.wrapData(reinterpret_cast<double*>(res.rawData()), width*2, outHeight, outDepth);
  wrapImg.pasteImg(img);
  wrapImg.clear();   // copy data finished

  // do fft
  fftw_plan p = fftw_plan_dft_r2c_3d(outDepth, outHeight, outWidth,
                                     reinterpret_cast<double*>(res.rawData()),
                                     reinterpret_cast<fftw_complex*>(res.rawData()),
                                     FFTW_ESTIMATE);
  fftw_execute(p);
  fftw_destroy_plan(p);

  return res;
}

ZImg ifft(ZComplexImg &cimg, size_t width, size_t outWidth, size_t outHeight, size_t outDepth)
{
  ZImg res;
  if (cimg.isEmpty())
    return res;

  fftw_plan p = fftw_plan_dft_c2r_3d(cimg.depth(), cimg.height(), width,
                                     reinterpret_cast<fftw_complex*>(cimg.rawData()),
                                     reinterpret_cast<double*>(cimg.rawData()),
                                     FFTW_ESTIMATE);
  fftw_execute(p);
  fftw_destroy_plan(p);

  ZImg wrapImg;
  wrapImg.wrapData(reinterpret_cast<double*>(cimg.rawData()), cimg.width()*2, cimg.height(), cimg.depth());

  ZImgRegion region(0, outWidth, 0, outHeight, 0, outDepth);
  res = wrapImg.crop(region);
  res /= static_cast<uint64_t>(width * cimg.height() * cimg.depth());
  cimg.clear();

  return res;
}

} // namespace nim
