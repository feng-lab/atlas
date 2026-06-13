#include "zimagehwy.h"

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "zimagehwy.cpp"
#include "hwy/foreach_target.h"
#include "hwy/highway.h"

HWY_BEFORE_NAMESPACE();

namespace nim {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

namespace {

template<typename D>
HWY_INLINE double reduceKernelRowLanes(D d,
                                       const double* HWY_RESTRICT imgStart,
                                       const double* HWY_RESTRICT kernel,
                                       size_t kernelWidth,
                                       size_t* index)
{
  const size_t lanes = hn::Lanes(d);
  auto vsum = hn::Zero(d);

  size_t k = *index;
  for (; k + lanes <= kernelWidth; k += lanes) {
    const auto va = hn::LoadU(d, imgStart + k);
    const auto vb = hn::LoadU(d, kernel + k);
    vsum = hn::MulAdd(va, vb, vsum);
  }

  *index = k;
  return hn::ReduceSum(d, vsum);
}

HWY_INLINE double
reduceKernelRow(const double* HWY_RESTRICT imgStart, const double* HWY_RESTRICT kernel, size_t kernelWidth)
{
  size_t k = 0;
  double sum = reduceKernelRowLanes(hn::ScalableTag<double>(), imgStart, kernel, kernelWidth, &k);
  sum += reduceKernelRowLanes(hn::CappedTag<double, 4>(), imgStart, kernel, kernelWidth, &k);
  sum += reduceKernelRowLanes(hn::CappedTag<double, 2>(), imgStart, kernel, kernelWidth, &k);

  for (; k < kernelWidth; ++k) {
    sum += imgStart[k] * kernel[k];
  }
  return sum;
}

} // namespace

void image2DFilterForOneBlock(const double* HWY_RESTRICT padImg,
                              size_t padImgWidth,
                              const double* HWY_RESTRICT kernel,
                              size_t kernelWidth,
                              size_t kernelHeight,
                              double* HWY_RESTRICT imgOut,
                              size_t imgOutWidth,
                              size_t rangeStart,
                              size_t rangeEnd)
{
  for (size_t j = rangeStart; j < rangeEnd; ++j) {
    for (size_t i = 0; i < imgOutWidth; ++i) {
      double sum = 0.0;
      for (size_t r = 0; r < kernelHeight; ++r) {
        const double* imgStart = padImg + (j + r) * padImgWidth + i;
        sum += reduceKernelRow(imgStart, kernel + r * kernelWidth, kernelWidth);
      }
      imgOut[j * imgOutWidth + i] = sum;
    }
  }
}

void image2DRowFilterForOneBlock(const double* HWY_RESTRICT padImg,
                                 size_t padImgWidth,
                                 const double* HWY_RESTRICT kernel,
                                 size_t kernelWidth,
                                 double* HWY_RESTRICT imgOut,
                                 size_t imgOutWidth,
                                 size_t rangeStart,
                                 size_t rangeEnd)
{
  for (size_t j = rangeStart; j < rangeEnd; ++j) {
    for (size_t i = 0; i < imgOutWidth; ++i) {
      const double* imgStart = padImg + j * padImgWidth + i;
      imgOut[j * imgOutWidth + i] = reduceKernelRow(imgStart, kernel, kernelWidth);
    }
  }
}

void image3DFilterForOneBlock(const double* HWY_RESTRICT padImg,
                              size_t padImgWidth,
                              size_t padImgHeight,
                              const double* HWY_RESTRICT kernel,
                              size_t kernelWidth,
                              size_t kernelHeight,
                              size_t kernelDepth,
                              double* HWY_RESTRICT imgOut,
                              size_t imgOutWidth,
                              size_t imgOutHeight,
                              size_t rangeStart,
                              size_t rangeEnd)
{
  for (size_t k = rangeStart; k < rangeEnd; ++k) {
    for (size_t j = 0; j < imgOutHeight; ++j) {
      for (size_t i = 0; i < imgOutWidth; ++i) {
        double sum = 0.0;
        for (size_t s = 0; s < kernelDepth; ++s) {
          for (size_t r = 0; r < kernelHeight; ++r) {
            const double* imgStart = padImg + (j + r) * padImgWidth + i + (s + k) * padImgWidth * padImgHeight;
            const double* kernelStart = kernel + r * kernelWidth + s * kernelWidth * kernelHeight;
            sum += reduceKernelRow(imgStart, kernelStart, kernelWidth);
          }
        }
        imgOut[j * imgOutWidth + i + k * imgOutWidth * imgOutHeight] = sum;
      }
    }
  }
}

void image3DRowFilterForOneBlock(const double* HWY_RESTRICT padImg,
                                 size_t padImgWidth,
                                 size_t padImgHeight,
                                 const double* HWY_RESTRICT kernel,
                                 size_t kernelWidth,
                                 double* HWY_RESTRICT imgOut,
                                 size_t imgOutWidth,
                                 size_t imgOutHeight,
                                 size_t rangeStart,
                                 size_t rangeEnd)
{
  for (size_t k = rangeStart; k < rangeEnd; ++k) {
    for (size_t j = 0; j < imgOutHeight; ++j) {
      for (size_t i = 0; i < imgOutWidth; ++i) {
        const double* imgStart = padImg + j * padImgWidth + i + k * padImgWidth * padImgHeight;
        imgOut[j * imgOutWidth + i + k * imgOutWidth * imgOutHeight] = reduceKernelRow(imgStart, kernel, kernelWidth);
      }
    }
  }
}

} // namespace HWY_NAMESPACE
} // namespace nim

HWY_AFTER_NAMESPACE();

#if HWY_ONCE

namespace nim {

HWY_EXPORT(image2DFilterForOneBlock);
HWY_EXPORT(image2DRowFilterForOneBlock);
HWY_EXPORT(image3DFilterForOneBlock);
HWY_EXPORT(image3DRowFilterForOneBlock);

void Image2DFilterForOneBlock_Hwy(const double* padImg,
                                  size_t padImgWidth,
                                  const double* kernel,
                                  size_t kernelWidth,
                                  size_t kernelHeight,
                                  double* imgOut,
                                  size_t imgOutWidth,
                                  size_t rangeStart,
                                  size_t rangeEnd)
{
  HWY_DYNAMIC_DISPATCH(image2DFilterForOneBlock)
  (padImg, padImgWidth, kernel, kernelWidth, kernelHeight, imgOut, imgOutWidth, rangeStart, rangeEnd);
}

void Image2DRowFilterForOneBlock_Hwy(const double* padImg,
                                     size_t padImgWidth,
                                     const double* kernel,
                                     size_t kernelWidth,
                                     double* imgOut,
                                     size_t imgOutWidth,
                                     size_t rangeStart,
                                     size_t rangeEnd)
{
  HWY_DYNAMIC_DISPATCH(image2DRowFilterForOneBlock)
  (padImg, padImgWidth, kernel, kernelWidth, imgOut, imgOutWidth, rangeStart, rangeEnd);
}

void Image3DFilterForOneBlock_Hwy(const double* padImg,
                                  size_t padImgWidth,
                                  size_t padImgHeight,
                                  const double* kernel,
                                  size_t kernelWidth,
                                  size_t kernelHeight,
                                  size_t kernelDepth,
                                  double* imgOut,
                                  size_t imgOutWidth,
                                  size_t imgOutHeight,
                                  size_t rangeStart,
                                  size_t rangeEnd)
{
  HWY_DYNAMIC_DISPATCH(image3DFilterForOneBlock)
  (padImg,
   padImgWidth,
   padImgHeight,
   kernel,
   kernelWidth,
   kernelHeight,
   kernelDepth,
   imgOut,
   imgOutWidth,
   imgOutHeight,
   rangeStart,
   rangeEnd);
}

void Image3DRowFilterForOneBlock_Hwy(const double* padImg,
                                     size_t padImgWidth,
                                     size_t padImgHeight,
                                     const double* kernel,
                                     size_t kernelWidth,
                                     double* imgOut,
                                     size_t imgOutWidth,
                                     size_t imgOutHeight,
                                     size_t rangeStart,
                                     size_t rangeEnd)
{
  HWY_DYNAMIC_DISPATCH(image3DRowFilterForOneBlock)
  (padImg, padImgWidth, padImgHeight, kernel, kernelWidth, imgOut, imgOutWidth, imgOutHeight, rangeStart, rangeEnd);
}

} // namespace nim

#endif // HWY_ONCE
