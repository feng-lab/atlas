#pragma once

#include <cstddef>

namespace nim {

void Image2DFilterForOneBlock_AVX(const double* padImg,
                                  size_t padImgWidth,
                                  const double* kernel,
                                  size_t kernelWidth,
                                  size_t kernelHeight,
                                  double* imgOut,
                                  size_t imgOutWidth,
                                  size_t rangeStart,
                                  size_t rangeEnd);

void Image2DRowFilterForOneBlock_AVX(const double* padImg,
                                     size_t padImgWidth,
                                     const double* kernel,
                                     size_t kernelWidth,
                                     double* imgOut,
                                     size_t imgOutWidth,
                                     size_t rangeStart,
                                     size_t rangeEnd);


void Image3DFilterForOneBlock_AVX(const double* padImg,
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
                                  size_t rangeEnd);

void Image3DRowFilterForOneBlock_AVX(const double* padImg,
                                     size_t padImgWidth,
                                     size_t padImgHeight,
                                     const double* kernel,
                                     size_t kernelWidth,
                                     double* imgOut,
                                     size_t imgOutWidth,
                                     size_t imgOutHeight,
                                     size_t rangeStart,
                                     size_t rangeEnd);

} // namespace nim
