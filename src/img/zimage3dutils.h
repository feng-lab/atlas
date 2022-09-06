#pragma once

#include "zcpuinfo.h"
#include "zlog.h"
#include "zimagesse3.h"
#include "zimageavx.h"
#include "zimageavx512.h"
#include "zimage2dutils.h"
#include <tbb/parallel_for.h>
#include <boost/align/aligned_allocator.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <utility>

namespace nim {

// everything is row major

template<typename SignedIntegerType, typename TPixel>
TPixel getImage3DPixelValue(const TPixel* img,
                            size_t width,
                            size_t height,
                            size_t depth,
                            SignedIntegerType x,
                            SignedIntegerType y,
                            SignedIntegerType z,
                            PadOption padOption = PadOption::Constant,
                            TPixel padValue = TPixel(0))
{
  if (x >= 0 && x < static_cast<SignedIntegerType>(width) && y >= 0 && y < static_cast<SignedIntegerType>(height) &&
      z >= 0 && z < static_cast<SignedIntegerType>(depth)) {
    return img[static_cast<size_t>(z) * width * height + y * width + x];
  }

  if (padOption == PadOption::Constant) {
    return padValue;
  }

  SignedIntegerType coord[3];
  coord[0] = x;
  coord[1] = y;
  coord[2] = z;
  size_t imgSize[3];
  imgSize[0] = width;
  imgSize[1] = height;
  imgSize[2] = depth;
  wrapCoordToImage(coord, imgSize, 3, padOption);
  return img[coord[2] * width * height + coord[1] * width + coord[0]];
}

template<typename TPixel>
TPixel getImage3DPixelValue(const TPixel* img,
                            size_t width,
                            size_t height,
                            size_t depth,
                            size_t x,
                            size_t y,
                            size_t z,
                            PadOption padOption = PadOption::Constant,
                            TPixel padValue = TPixel(0))
{
  if (x < width && y < height && z < depth) {
    return img[z * width * height + y * width + x];
  }

  if (padOption == PadOption::Constant) {
    return padValue;
  }

  size_t coord[3];
  coord[0] = x;
  coord[1] = y;
  coord[2] = z;
  size_t imgSize[3];
  imgSize[0] = width;
  imgSize[1] = height;
  imgSize[2] = depth;
  wrapCoordToImage(coord, imgSize, 3, padOption);
  return img[coord[2] * width * height + coord[1] * width + coord[0]];
}

// imgOut should be preallocated and not same as img
template<typename TPixel>
void image3DPad(const TPixel* img,
                size_t width,
                size_t height,
                size_t depth,
                size_t leftPad,
                size_t rightPad,
                size_t upPad,
                size_t downPad,
                size_t frontPad,
                size_t backPad,
                TPixel* imgOut,
                PadOption padOption = PadOption::Constant,
                TPixel padValue = TPixel(0))
{
  DCHECK_NE(img, imgOut);

  size_t plane = width * height;

  if (leftPad == 0 && rightPad == 0 && upPad == 0 && downPad == 0 && frontPad == 0 && backPad == 0) {
    std::memcpy(imgOut, img, sizeof(TPixel) * plane * depth);
  } else {
    size_t desWidth = leftPad + width + rightPad;
    size_t desHeight = upPad + height + downPad;
    size_t desDepth = depth + frontPad + backPad;
    size_t desPlane = desWidth * desHeight;

    // copy image
    for (size_t j = 0; j < depth; ++j) {
      if (leftPad == 0 && rightPad == 0) {
        TPixel* desStart = imgOut + upPad * width + (j + frontPad) * desPlane;
        std::memcpy(desStart, img + j * plane, sizeof(TPixel) * plane);
      } else {
        TPixel* desStart = imgOut + upPad * desWidth + leftPad + (j + frontPad) * desPlane;
        for (size_t i = 0; i < height; ++i) {
          std::memcpy(desStart, img + i * width + j * plane, sizeof(TPixel) * width);
          desStart += desWidth;
        }
      }
    }

    // boundary
    for (size_t k = 0; k < frontPad; ++k) {
      for (size_t j = 0; j < desHeight; ++j) {
        for (size_t i = 0; i < desWidth; ++i) {
          imgOut[k * desHeight * desWidth + j * desWidth + i] =
            getImage3DPixelValue(img, width, height, depth, i - leftPad, j - upPad, k - frontPad, padOption, padValue);
        }
      }
    }
    for (size_t k = frontPad + depth; k < desDepth; ++k) {
      for (size_t j = 0; j < desHeight; ++j) {
        for (size_t i = 0; i < desWidth; ++i) {
          imgOut[k * desHeight * desWidth + j * desWidth + i] =
            getImage3DPixelValue(img, width, height, depth, i - leftPad, j - upPad, k - frontPad, padOption, padValue);
        }
      }
    }
    for (size_t k = frontPad; k < frontPad + depth; ++k) {
      for (size_t j = 0; j < upPad; ++j) {
        for (size_t i = 0; i < desWidth; ++i) {
          imgOut[k * desHeight * desWidth + j * desWidth + i] =
            getImage3DPixelValue(img, width, height, depth, i - leftPad, j - upPad, k - frontPad, padOption, padValue);
        }
      }
      for (size_t j = upPad + height; j < desHeight; ++j) {
        for (size_t i = 0; i < desWidth; ++i) {
          imgOut[k * desHeight * desWidth + j * desWidth + i] =
            getImage3DPixelValue(img, width, height, depth, i - leftPad, j - upPad, k - frontPad, padOption, padValue);
        }
      }
      for (size_t j = upPad; j < upPad + height; ++j) {
        for (size_t i = 0; i < leftPad; ++i) {
          imgOut[k * desHeight * desWidth + j * desWidth + i] =
            getImage3DPixelValue(img, width, height, depth, i - leftPad, j - upPad, k - frontPad, padOption, padValue);
        }
        for (size_t i = leftPad + width; i < desWidth; ++i) {
          imgOut[k * desHeight * desWidth + j * desWidth + i] =
            getImage3DPixelValue(img, width, height, depth, i - leftPad, j - upPad, k - frontPad, padOption, padValue);
        }
      }
    }
  }
}

template<typename TPixel>
void image3DFlip(TPixel* img, size_t width, size_t height, size_t depth, Dimension dim)
{
  if (dim == Dimension::X) {
    if (width <= 1) {
      return;
    }
    for (size_t d = 0; d < depth; ++d) {
      for (size_t i = 0; i < height; ++i) {
        TPixel* start = img + i * width + d * width * height;
        std::reverse(start, start + width);
      }
    }
  } else if (dim == Dimension::Y) {
    if (height <= 1) {
      return;
    }
    std::vector<TPixel> buffer(width);
    for (size_t d = 0; d < depth; ++d) {
      size_t j = 0;
      size_t k = height - 1;
      size_t size = sizeof(TPixel) * width;
      while (j < k) {
        std::memcpy(buffer.data(), img + j * width + d * width * height, size);
        std::memcpy(img + j * width + d * width * height, img + k * width + d * width * height, size);
        std::memcpy(img + k * width + d * width * height, buffer.data(), size);
        ++j;
        --k;
      }
    }
  } else if (dim == Dimension::Z) {
    if (depth <= 1) {
      return;
    }
    std::vector<TPixel> buffer(width * height);
    size_t j = 0;
    size_t k = depth - 1;
    size_t size = sizeof(TPixel) * width * height;
    while (j < k) {
      std::memcpy(buffer.data(), img + j * width * height, size);
      std::memcpy(img + j * width * height, img + k * width * height, size);
      std::memcpy(img + k * width * height, buffer.data(), size);
      ++j;
      --k;
    }
  }
}

// same as flip all dimensions
template<typename TPixel>
void image3DReflect(TPixel* img, size_t width, size_t height, size_t depth)
{
  std::reverse(img, img + width * height * depth);
}

template<typename TPixel, typename TPixelOut = TPixel>
struct Image3DFilterForOneBlock
{
  Image3DFilterForOneBlock(const TPixel* padImg,
                           size_t padImgWidth,
                           size_t padImgHeight,
                           const double* kernel,
                           size_t kernelWidth,
                           size_t kernelHeight,
                           size_t kernelDepth,
                           TPixelOut* imgOut,
                           size_t imgOutWidth,
                           size_t imgOutHeight)
    : m_padImg(padImg)
    , m_padImgWidth(padImgWidth)
    , m_padImgHeight(padImgHeight)
    , m_kernel(kernel)
    , m_kernelWidth(kernelWidth)
    , m_kernelHeight(kernelHeight)
    , m_kernelDepth(kernelDepth)
    , m_imgOut(imgOut)
    , m_imgOutWidth(imgOutWidth)
    , m_imgOutHeight(imgOutHeight)
  {}

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    for (size_t k = range.begin(); k != range.end(); ++k) {
      for (size_t j = 0; j < m_imgOutHeight; ++j) {
        for (size_t i = 0; i < m_imgOutWidth; ++i) {
          double sum = 0.0;
          for (size_t s = 0; s < m_kernelDepth; ++s) { // plane by plane
            for (size_t r = 0; r < m_kernelHeight; ++r) { // row by row
              const TPixel* imgStart =
                m_padImg + (j + r) * m_padImgWidth + i + (s + k) * m_padImgWidth * m_padImgHeight;
              sum = std::inner_product(imgStart,
                                       imgStart + m_kernelWidth,
                                       m_kernel + r * m_kernelWidth + s * m_kernelWidth * m_kernelHeight,
                                       sum);
            }
          }
          m_imgOut[j * m_imgOutWidth + i + k * m_imgOutWidth * m_imgOutHeight] = saturate_cast<TPixelOut>(sum);
        }
      }
    }
  }

  const TPixel* m_padImg;
  size_t m_padImgWidth;
  size_t m_padImgHeight;
  const double* m_kernel;
  size_t m_kernelWidth;
  size_t m_kernelHeight;
  size_t m_kernelDepth;
  TPixelOut* m_imgOut;
  size_t m_imgOutWidth;
  size_t m_imgOutHeight;
};

template<>
struct Image3DFilterForOneBlock<double, double>
{
  Image3DFilterForOneBlock(const double* padImg,
                           size_t padImgWidth,
                           size_t padImgHeight,
                           const double* kernel,
                           size_t kernelWidth,
                           size_t kernelHeight,
                           size_t kernelDepth,
                           double* imgOut,
                           size_t imgOutWidth,
                           size_t imgOutHeight)
    : m_padImg(padImg)
    , m_padImgWidth(padImgWidth)
    , m_padImgHeight(padImgHeight)
    , m_kernel(kernel)
    , m_kernelWidth(kernelWidth)
    , m_kernelHeight(kernelHeight)
    , m_kernelDepth(kernelDepth)
    , m_imgOut(imgOut)
    , m_imgOutWidth(imgOutWidth)
    , m_imgOutHeight(imgOutHeight)
  {}

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    if (ZCpuInfo::instance().bAVX512F && m_kernelWidth >= 8) {
      Image3DFilterForOneBlock_AVX512(m_padImg,
                                      m_padImgWidth,
                                      m_padImgHeight,
                                      m_kernel,
                                      m_kernelWidth,
                                      m_kernelHeight,
                                      m_kernelDepth,
                                      m_imgOut,
                                      m_imgOutWidth,
                                      m_imgOutHeight,
                                      range.begin(),
                                      range.end());
    } else if (ZCpuInfo::instance().bAVX && m_kernelWidth >= 4) {
      Image3DFilterForOneBlock_AVX(m_padImg,
                                   m_padImgWidth,
                                   m_padImgHeight,
                                   m_kernel,
                                   m_kernelWidth,
                                   m_kernelHeight,
                                   m_kernelDepth,
                                   m_imgOut,
                                   m_imgOutWidth,
                                   m_imgOutHeight,
                                   range.begin(),
                                   range.end());
    } else if (m_kernelWidth >= 2) {
      Image3DFilterForOneBlock_SSE3(m_padImg,
                                    m_padImgWidth,
                                    m_padImgHeight,
                                    m_kernel,
                                    m_kernelWidth,
                                    m_kernelHeight,
                                    m_kernelDepth,
                                    m_imgOut,
                                    m_imgOutWidth,
                                    m_imgOutHeight,
                                    range.begin(),
                                    range.end());
    } else {
      for (size_t k = range.begin(); k != range.end(); ++k) {
        for (size_t j = 0; j < m_imgOutHeight; ++j) {
          for (size_t i = 0; i < m_imgOutWidth; ++i) {
            double sum = 0.0;
            for (size_t s = 0; s < m_kernelDepth; ++s) { // plane by plane
              for (size_t r = 0; r < m_kernelHeight; ++r) { // row by row
                const double* imgStart =
                  m_padImg + (j + r) * m_padImgWidth + i + (s + k) * m_padImgWidth * m_padImgHeight;
                sum = std::inner_product(imgStart,
                                         imgStart + m_kernelWidth,
                                         m_kernel + r * m_kernelWidth + s * m_kernelWidth * m_kernelHeight,
                                         sum);
              }
            }
            m_imgOut[j * m_imgOutWidth + i + k * m_imgOutWidth * m_imgOutHeight] = sum;
          }
        }
      }
    }
  }

  const double* m_padImg;
  size_t m_padImgWidth;
  size_t m_padImgHeight;
  const double* m_kernel;
  size_t m_kernelWidth;
  size_t m_kernelHeight;
  size_t m_kernelDepth;
  double* m_imgOut;
  size_t m_imgOutWidth;
  size_t m_imgOutHeight;
};

template<typename TPixel, typename TPixelOut = TPixel>
struct Image3DRowFilterForOneBlock
{
  Image3DRowFilterForOneBlock(const TPixel* padImg,
                              size_t padImgWidth,
                              size_t padImgHeight,
                              const double* kernel,
                              size_t kernelWidth,
                              TPixelOut* imgOut,
                              size_t imgOutWidth,
                              size_t imgOutHeight)
    : m_padImg(padImg)
    , m_padImgWidth(padImgWidth)
    , m_padImgHeight(padImgHeight)
    , m_kernel(kernel)
    , m_kernelWidth(kernelWidth)
    , m_imgOut(imgOut)
    , m_imgOutWidth(imgOutWidth)
    , m_imgOutHeight(imgOutHeight)
  {}

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    for (size_t k = range.begin(); k != range.end(); ++k) {
      for (size_t j = 0; j < m_imgOutHeight; ++j) {
        for (size_t i = 0; i < m_imgOutWidth; ++i) {
          double sum = 0.0;
          const TPixel* imgStart = m_padImg + j * m_padImgWidth + i + k * m_padImgWidth * m_padImgHeight;
          sum = std::inner_product(imgStart, imgStart + m_kernelWidth, m_kernel, sum);
          m_imgOut[j * m_imgOutWidth + i + k * m_imgOutWidth * m_imgOutHeight] = saturate_cast<TPixelOut>(sum);
        }
      }
    }
  }

  const TPixel* m_padImg;
  size_t m_padImgWidth;
  size_t m_padImgHeight;
  const double* m_kernel;
  size_t m_kernelWidth;
  TPixelOut* m_imgOut;
  size_t m_imgOutWidth;
  size_t m_imgOutHeight;
};

template<>
struct Image3DRowFilterForOneBlock<double, double>
{
  Image3DRowFilterForOneBlock(const double* padImg,
                              size_t padImgWidth,
                              size_t padImgHeight,
                              const double* kernel,
                              size_t kernelWidth,
                              double* imgOut,
                              size_t imgOutWidth,
                              size_t imgOutHeight)
    : m_padImg(padImg)
    , m_padImgWidth(padImgWidth)
    , m_padImgHeight(padImgHeight)
    , m_kernel(kernel)
    , m_kernelWidth(kernelWidth)
    , m_imgOut(imgOut)
    , m_imgOutWidth(imgOutWidth)
    , m_imgOutHeight(imgOutHeight)
  {}

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    if (ZCpuInfo::instance().bAVX512F && m_kernelWidth >= 8) {
      Image3DRowFilterForOneBlock_AVX512(m_padImg,
                                         m_padImgWidth,
                                         m_padImgHeight,
                                         m_kernel,
                                         m_kernelWidth,
                                         m_imgOut,
                                         m_imgOutWidth,
                                         m_imgOutHeight,
                                         range.begin(),
                                         range.end());
    } else if (ZCpuInfo::instance().bAVX && m_kernelWidth >= 4) {
      Image3DRowFilterForOneBlock_AVX(m_padImg,
                                      m_padImgWidth,
                                      m_padImgHeight,
                                      m_kernel,
                                      m_kernelWidth,
                                      m_imgOut,
                                      m_imgOutWidth,
                                      m_imgOutHeight,
                                      range.begin(),
                                      range.end());
    } else if (m_kernelWidth >= 2) {
      Image3DRowFilterForOneBlock_SSE3(m_padImg,
                                       m_padImgWidth,
                                       m_padImgHeight,
                                       m_kernel,
                                       m_kernelWidth,
                                       m_imgOut,
                                       m_imgOutWidth,
                                       m_imgOutHeight,
                                       range.begin(),
                                       range.end());
    } else {
      for (size_t k = range.begin(); k != range.end(); ++k) {
        for (size_t j = 0; j < m_imgOutHeight; ++j) {
          for (size_t i = 0; i < m_imgOutWidth; ++i) {
            double sum = 0.0;
            const double* imgStart = m_padImg + j * m_padImgWidth + i + k * m_padImgWidth * m_padImgHeight;
            sum = std::inner_product(imgStart, imgStart + m_kernelWidth, m_kernel, sum);
            m_imgOut[j * m_imgOutWidth + i + k * m_imgOutWidth * m_imgOutHeight] = sum;
          }
        }
      }
    }
  }

  const double* m_padImg;
  size_t m_padImgWidth;
  size_t m_padImgHeight;
  const double* m_kernel;
  size_t m_kernelWidth;
  double* m_imgOut;
  size_t m_imgOutWidth;
  size_t m_imgOutHeight;
};

template<typename TPixel, typename TPixelOut = TPixel>
struct Image3DColFilterForOneBlock
{
  Image3DColFilterForOneBlock(const TPixel* padImg,
                              size_t padImgWidth,
                              size_t padImgHeight,
                              const double* kernel,
                              size_t kernelHeight,
                              TPixelOut* imgOut,
                              size_t imgOutWidth,
                              size_t imgOutHeight)
    : m_padImg(padImg)
    , m_padImgWidth(padImgWidth)
    , m_padImgHeight(padImgHeight)
    , m_kernel(kernel)
    , m_kernelHeight(kernelHeight)
    , m_imgOut(imgOut)
    , m_imgOutWidth(imgOutWidth)
    , m_imgOutHeight(imgOutHeight)
  {}

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    for (size_t k = range.begin(); k != range.end(); ++k) {
      for (size_t j = 0; j < m_imgOutHeight; ++j) {
        for (size_t i = 0; i < m_imgOutWidth; ++i) {
          double sum = 0.0;
          for (size_t r = 0; r < m_kernelHeight; ++r) { // row by row
            sum += m_kernel[r] * (*(m_padImg + (j + r) * m_padImgWidth + i + k * m_padImgWidth * m_padImgHeight));
          }
          m_imgOut[j * m_imgOutWidth + i + k * m_imgOutWidth * m_imgOutHeight] = saturate_cast<TPixelOut>(sum);
        }
      }
    }
  }

  const TPixel* m_padImg;
  size_t m_padImgWidth;
  size_t m_padImgHeight;
  const double* m_kernel;
  size_t m_kernelHeight;
  TPixelOut* m_imgOut;
  size_t m_imgOutWidth;
  size_t m_imgOutHeight;
};

template<typename TPixel, typename TPixelOut = TPixel>
struct Image3DZFilterForOneBlock
{
  Image3DZFilterForOneBlock(const TPixel* padImg,
                            size_t padImgWidth,
                            size_t padImgHeight,
                            const double* kernel,
                            size_t kernelDepth,
                            TPixelOut* imgOut,
                            size_t imgOutWidth,
                            size_t imgOutHeight)
    : m_padImg(padImg)
    , m_padImgWidth(padImgWidth)
    , m_padImgHeight(padImgHeight)
    , m_kernel(kernel)
    , m_kernelDepth(kernelDepth)
    , m_imgOut(imgOut)
    , m_imgOutWidth(imgOutWidth)
    , m_imgOutHeight(imgOutHeight)
  {}

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    for (size_t k = range.begin(); k != range.end(); ++k) {
      for (size_t j = 0; j < m_imgOutHeight; ++j) {
        for (size_t i = 0; i < m_imgOutWidth; ++i) {
          double sum = 0.0;
          for (size_t p = 0; p < m_kernelDepth; ++p) { // plane by plane
            sum += m_kernel[p] * (*(m_padImg + j * m_padImgWidth + i + (k + p) * m_padImgWidth * m_padImgHeight));
          }
          m_imgOut[j * m_imgOutWidth + i + k * m_imgOutWidth * m_imgOutHeight] = saturate_cast<TPixelOut>(sum);
        }
      }
    }
  }

  const TPixel* m_padImg;
  size_t m_padImgWidth;
  size_t m_padImgHeight;
  const double* m_kernel;
  size_t m_kernelDepth;
  TPixelOut* m_imgOut;
  size_t m_imgOutWidth;
  size_t m_imgOutHeight;
};

template<typename TPixel, typename TPixelOut>
void image3DFilter(const TPixel* img,
                   size_t width,
                   size_t height,
                   size_t depth,
                   const double* kernel,
                   size_t kernelWidth,
                   size_t kernelHeight,
                   size_t kernelDepth,
                   TPixelOut* imgOut,
                   PadOption boundaryOption = PadOption::Constant,
                   TPixel boundaryValue = TPixel(0),
                   bool corr = true,
                   bool useMultithreading = true)
{
  size_t leftPad = kernelWidth / 2;
  size_t rightPad = (kernelWidth - 1) / 2;
  size_t upPad = kernelHeight / 2;
  size_t downPad = (kernelHeight - 1) / 2;
  size_t frontPad = kernelDepth / 2;
  size_t backPad = (kernelDepth - 1) / 2;
  size_t desWidth = leftPad + width + rightPad;
  size_t desHeight = upPad + height + downPad;
  size_t desDepth = frontPad + depth + backPad;
  std::vector<TPixel, boost::alignment::aligned_allocator<TPixel, 64>> padImg(desWidth * desHeight * desDepth);
  // ZBenchTimer bt;
  // bt.start();
  image3DPad(img,
             width,
             height,
             depth,
             leftPad,
             rightPad,
             upPad,
             downPad,
             frontPad,
             backPad,
             padImg.data(),
             boundaryOption,
             boundaryValue);
  // bt.stopAndPrint();

  // image3DWrite(padImg.data(), desWidth, desHeight, "/Users/feng/Downloads/padImg.tif");

  std::vector<double, boost::alignment::aligned_allocator<double, 64>> alignedKernel;
  alignedKernel.insert(alignedKernel.end(), kernel, kernel + kernelWidth * kernelHeight * kernelDepth);
  const double* adjKernel = alignedKernel.data();
  if (!corr) {
    image3DReflect(alignedKernel.data(), kernelWidth, kernelHeight, kernelDepth);
  }

  // get correlation of padImg and adjKernel
  Image3DFilterForOneBlock<TPixel, TPixelOut> functor(padImg.data(),
                                                      desWidth,
                                                      desHeight,
                                                      adjKernel,
                                                      kernelWidth,
                                                      kernelHeight,
                                                      kernelDepth,
                                                      imgOut,
                                                      width,
                                                      height);
  if (!useMultithreading) {
    functor(tbb::blocked_range<size_t>(0, depth));
  } else {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, depth), functor);
  }
}

template<typename TPixel, typename TPixelOut>
void image3DFilter(const TPixel* img,
                   size_t width,
                   size_t height,
                   size_t depth,
                   const double* rowkernel,
                   size_t kernelWidth,
                   const double* colkernel,
                   size_t kernelHeight,
                   const double* zkernel,
                   size_t kernelDepth,
                   TPixelOut* imgOut,
                   PadOption boundaryOption = PadOption::Constant,
                   TPixel boundaryValue = TPixel(0),
                   bool corr = true,
                   bool useMultithreading = true)
{
  size_t leftPad = kernelWidth / 2;
  size_t rightPad = (kernelWidth - 1) / 2;
  size_t upPad = kernelHeight / 2;
  size_t downPad = (kernelHeight - 1) / 2;
  size_t frontPad = kernelDepth / 2;
  size_t backPad = (kernelDepth - 1) / 2;
  size_t desWidth = leftPad + width + rightPad;
  size_t desHeight = upPad + height + downPad;
  size_t desDepth = frontPad + depth + backPad;
  std::vector<TPixel, boost::alignment::aligned_allocator<TPixel, 64>> padImg(desWidth * desHeight * desDepth);
  // ZBenchTimer bt;
  // bt.start();
  image3DPad(img,
             width,
             height,
             depth,
             leftPad,
             rightPad,
             upPad,
             downPad,
             frontPad,
             backPad,
             padImg.data(),
             boundaryOption,
             boundaryValue);
  // bt.stopAndPrint();

  // image3DWrite(padImg.data(), desWidth, desHeight, "/Users/feng/Downloads/padImg.tif");

  std::vector<double, boost::alignment::aligned_allocator<double, 64>> alignedRowKernel;
  std::vector<double, boost::alignment::aligned_allocator<double, 64>> alignedColKernel;
  std::vector<double, boost::alignment::aligned_allocator<double, 64>> alignedZKernel;
  alignedRowKernel.insert(alignedRowKernel.end(), rowkernel, rowkernel + kernelWidth);
  alignedColKernel.insert(alignedColKernel.end(), colkernel, colkernel + kernelHeight);
  alignedZKernel.insert(alignedZKernel.end(), zkernel, zkernel + kernelDepth);
  const double* adjrowkernel = alignedRowKernel.data();
  const double* adjcolkernel = alignedColKernel.data();
  const double* adjzkernel = alignedZKernel.data();
  if (!corr) {
    image2DFlip(alignedRowKernel.data(), kernelWidth, 1, Dimension::X);
    image2DFlip(alignedColKernel.data(), kernelHeight, 1, Dimension::X);
    image2DFlip(alignedZKernel.data(), kernelDepth, 1, Dimension::X);
  }

  // get correlation of padImg and adjKernel
  if (!useMultithreading) {
    std::vector<double, boost::alignment::aligned_allocator<double, 64>> bufImg1(desWidth * height * desDepth);
    Image3DColFilterForOneBlock<TPixel, double>
      colfunctor(padImg.data(), desWidth, desHeight, adjcolkernel, kernelHeight, bufImg1.data(), desWidth, height);
    colfunctor(tbb::blocked_range<size_t>(0, desDepth));
    clearAndDeallocate(padImg);

    std::vector<double, boost::alignment::aligned_allocator<double, 64>> bufImg2(width * height * desDepth);
    Image3DRowFilterForOneBlock<double, double>
      rowfunctor(bufImg1.data(), desWidth, height, adjrowkernel, kernelWidth, bufImg2.data(), width, height);
    rowfunctor(tbb::blocked_range<size_t>(0, desDepth));
    clearAndDeallocate(bufImg1);

    Image3DZFilterForOneBlock<double, TPixelOut>
      zfunctor(bufImg2.data(), width, height, adjzkernel, kernelDepth, imgOut, width, height);
    zfunctor(tbb::blocked_range<size_t>(0, depth));
  } else {
    std::vector<double, boost::alignment::aligned_allocator<double, 64>> bufImg1(desWidth * height * desDepth);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, desDepth),
                      Image3DColFilterForOneBlock<TPixel, double>(padImg.data(),
                                                                  desWidth,
                                                                  desHeight,
                                                                  adjcolkernel,
                                                                  kernelHeight,
                                                                  bufImg1.data(),
                                                                  desWidth,
                                                                  height));
    clearAndDeallocate(padImg);

    std::vector<double, boost::alignment::aligned_allocator<double, 64>> bufImg2(width * height * desDepth);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, desDepth),
                      Image3DRowFilterForOneBlock<double, double>(bufImg1.data(),
                                                                  desWidth,
                                                                  height,
                                                                  adjrowkernel,
                                                                  kernelWidth,
                                                                  bufImg2.data(),
                                                                  width,
                                                                  height));
    clearAndDeallocate(bufImg1);

    tbb::parallel_for(tbb::blocked_range<size_t>(0, depth),
                      Image3DZFilterForOneBlock<double, TPixelOut>(bufImg2.data(),
                                                                   width,
                                                                   height,
                                                                   adjzkernel,
                                                                   kernelDepth,
                                                                   imgOut,
                                                                   width,
                                                                   height));
  }
}

// boundaryOption see image2DFilter
template<typename TPixel, typename TPixelOut>
void image3DGaussianFilter(const TPixel* img,
                           size_t width,
                           size_t height,
                           size_t depth,
                           double kernelSigmaX,
                           double kernelSigmaY,
                           double kernelSigmaZ,
                           TPixelOut* imgOut,
                           index_t kernelWidth = -1,
                           index_t kernelHeight = -1,
                           index_t kernelDepth = -1,
                           PadOption boundaryOption = PadOption::Constant,
                           TPixel boundaryValue = TPixel(0),
                           bool useMultithreading = true)
{
  size_t kWidth;
  size_t kHeight;
  size_t kDepth;
#if 0
  std::vector<double> kernel = create3DGaussianKernel(kernelSigmaX, kernelSigmaY, kernelSigmaZ,
                                                      kernelWidth, kernelHeight, kernelDepth,
                                                      &kWidth, &kHeight, &kDepth);

  //ZBenchTimer bt;
  //bt.start();
  image3DFilter(img, width, height, depth, kernel.data(), kWidth, kHeight, kDepth,
      imgOut, boundaryOption, boundaryValue, true, useMultithreading);
  //bt.stopAndPrint();
#else
  std::vector<double> rowkernel = create1DGaussianKernel(kernelSigmaX, kernelWidth, &kWidth);
  std::vector<double> colkernel = create1DGaussianKernel(kernelSigmaY, kernelHeight, &kHeight);
  std::vector<double> zkernel = create1DGaussianKernel(kernelSigmaZ, kernelDepth, &kDepth);

  // ZBenchTimer bt;
  // bt.start();
  image3DFilter(img,
                width,
                height,
                depth,
                rowkernel.data(),
                kWidth,
                colkernel.data(),
                kHeight,
                zkernel.data(),
                kDepth,
                imgOut,
                boundaryOption,
                boundaryValue,
                true,
                useMultithreading);
  // bt.stopAndPrint();
#endif
}

// note: LoG filter will produce negative value, better use a out type that can represent negative pixel
// such as double
// boundaryOption see image2DFilter
template<typename TPixel, typename TPixelOut>
void image3DLoGFilter(const TPixel* img,
                      size_t width,
                      size_t height,
                      size_t depth,
                      double kernelSigmaX,
                      double kernelSigmaY,
                      double kernelSigmaZ,
                      TPixelOut* imgOut,
                      index_t kernelWidth = -1,
                      index_t kernelHeight = -1,
                      index_t kernelDepth = -1,
                      PadOption boundaryOption = PadOption::Constant,
                      TPixel boundaryValue = TPixel(0),
                      bool useMultithreading = true)
{
  size_t kWidth;
  size_t kHeight;
  size_t kDepth;

  std::vector<double> rowLoGkernel = create1DLoGKernel(kernelSigmaX, kernelWidth, &kWidth);
  std::vector<double> colLoGkernel = create1DLoGKernel(kernelSigmaY, kernelHeight, &kHeight);
  std::vector<double> zLoGkernel = create1DLoGKernel(kernelSigmaZ, kernelDepth, &kDepth);
  std::vector<double> rowGkernel = create1DGaussianKernel(kernelSigmaX, kernelWidth, &kWidth);
  std::vector<double> colGkernel = create1DGaussianKernel(kernelSigmaY, kernelHeight, &kHeight);
  std::vector<double> zGkernel = create1DGaussianKernel(kernelSigmaZ, kernelDepth, &kDepth);

  // ZBenchTimer bt;
  // bt.start();
  image3DFilter(img,
                width,
                height,
                depth,
                rowLoGkernel.data(),
                kWidth,
                colGkernel.data(),
                kHeight,
                zGkernel.data(),
                kDepth,
                imgOut,
                boundaryOption,
                boundaryValue,
                true,
                useMultithreading);

  std::vector<TPixelOut> bufImg(width * height * depth);

  image3DFilter(img,
                width,
                height,
                depth,
                rowGkernel.data(),
                kWidth,
                colLoGkernel.data(),
                kHeight,
                zGkernel.data(),
                kDepth,
                bufImg.data(),
                boundaryOption,
                boundaryValue,
                true,
                useMultithreading);

  for (size_t i = 0; i < bufImg.size(); ++i) {
    imgOut[i] += bufImg[i];
  }

  image3DFilter(img,
                width,
                height,
                depth,
                rowGkernel.data(),
                kWidth,
                colGkernel.data(),
                kHeight,
                zLoGkernel.data(),
                kDepth,
                bufImg.data(),
                boundaryOption,
                boundaryValue,
                true,
                useMultithreading);

  for (size_t i = 0; i < bufImg.size(); ++i) {
    imgOut[i] += bufImg[i];
  }

  // bt.stopAndPrint();
}

template<typename TPixel, typename TPixelOut>
struct Resize3DForOneBlock
{
  Resize3DForOneBlock(const TPixel* img,
                      size_t width,
                      size_t height,
                      size_t depth,
                      TPixelOut* imgOut,
                      size_t outWidth,
                      size_t outHeight,
                      size_t outDepth,
                      const std::vector<double>& xWeights,
                      const std::vector<size_t>& xIndices,
                      size_t xKernelWidth,
                      bool xKernelIsTrivial,
                      const std::vector<double>& yWeights,
                      const std::vector<size_t>& yIndices,
                      size_t yKernelWidth,
                      bool yKernelIsTrivial,
                      const std::vector<double>& zWeights,
                      const std::vector<size_t>& zIndices,
                      size_t zKernelWidth,
                      bool zKernelIsTrivial)
    : m_img(img)
    , m_width(width)
    , m_height(height)
    , m_depth(depth)
    , m_imgOut(imgOut)
    , m_outWidth(outWidth)
    , m_outHeight(outHeight)
    , m_outDepth(outDepth)
    , m_xWeights(xWeights)
    , m_xIndices(xIndices)
    , m_xKernelWidth(xKernelWidth)
    , m_xKernelIsTrivial(xKernelIsTrivial)
    , m_yWeights(yWeights)
    , m_yIndices(yIndices)
    , m_yKernelWidth(yKernelWidth)
    , m_yKernelIsTrivial(yKernelIsTrivial)
    , m_zWeights(zWeights)
    , m_zIndices(zIndices)
    , m_zKernelWidth(zKernelWidth)
    , m_zKernelIsTrivial(zKernelIsTrivial)
  {}

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    if (m_xKernelIsTrivial && m_yKernelIsTrivial && m_zKernelIsTrivial) {
      for (size_t z = range.begin(); z != range.end(); ++z) {
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] =
              saturate_cast<TPixelOut>(m_img[z * m_width * m_height + y * m_width + x]);
          }
        }
      }
    } else if (m_xKernelIsTrivial && m_yKernelIsTrivial) {
      for (size_t z = range.begin(); z != range.end(); ++z) {
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            double valxyz = 0;
            for (size_t kz = 0; kz < m_zKernelWidth; ++kz) {
              valxyz += m_zWeights[z * m_zKernelWidth + kz] *
                        m_img[m_zIndices[z * m_zKernelWidth + kz] * m_width * m_height + y * m_width + x];
            }
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(valxyz);
          }
        }
      }
    } else if (m_xKernelIsTrivial && m_zKernelIsTrivial) {
      {
        for (size_t z = range.begin(); z != range.end(); ++z) {
          for (size_t y = 0; y < m_outHeight; ++y) {
            for (size_t x = 0; x < m_outWidth; ++x) {
              double valxyz = 0;
              for (size_t ky = 0; ky < m_yKernelWidth; ++ky) {
                valxyz += m_yWeights[y * m_yKernelWidth + ky] *
                          m_img[z * m_width * m_height + m_yIndices[y * m_yKernelWidth + ky] * m_width + x];
              }
              m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(valxyz);
            }
          }
        }
      }
    } else if (m_yKernelIsTrivial && m_zKernelIsTrivial) {
      for (size_t z = range.begin(); z != range.end(); ++z) {
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            double valxyz = 0;
            for (size_t kx = 0; kx < m_xKernelWidth; ++kx) {
              valxyz += m_xWeights[x * m_xKernelWidth + kx] *
                        m_img[z * m_width * m_height + y * m_width + m_xIndices[x * m_xKernelWidth + kx]];
            }
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(valxyz);
          }
        }
      }
    } else if (m_xKernelIsTrivial) {
      for (size_t z = range.begin(); z != range.end(); ++z) {
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            double valxyz = 0;
            for (size_t kz = 0; kz < m_zKernelWidth; ++kz) {
              double valxy = 0;
              for (size_t ky = 0; ky < m_yKernelWidth; ++ky) {
                valxy +=
                  m_yWeights[y * m_yKernelWidth + ky] * m_img[m_zIndices[z * m_zKernelWidth + kz] * m_width * m_height +
                                                              m_yIndices[y * m_yKernelWidth + ky] * m_width + x];
              }
              valxyz += m_zWeights[z * m_zKernelWidth + kz] * valxy;
            }
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(valxyz);
          }
        }
      }
    } else if (m_yKernelIsTrivial) {
      for (size_t z = range.begin(); z != range.end(); ++z) {
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            double valxyz = 0;
            for (size_t kz = 0; kz < m_zKernelWidth; ++kz) {
              double valx = 0;
              for (size_t kx = 0; kx < m_xKernelWidth; ++kx) {
                valx +=
                  m_xWeights[x * m_xKernelWidth + kx] * m_img[m_zIndices[z * m_zKernelWidth + kz] * m_width * m_height +
                                                              y * m_width + m_xIndices[x * m_xKernelWidth + kx]];
              }
              valxyz += m_zWeights[z * m_zKernelWidth + kz] * valx;
            }
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(valxyz);
          }
        }
      }
    } else if (m_zKernelIsTrivial) {
      for (size_t z = range.begin(); z != range.end(); ++z) {
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            double valxy = 0;
            for (size_t ky = 0; ky < m_yKernelWidth; ++ky) {
              double valx = 0;
              for (size_t kx = 0; kx < m_xKernelWidth; ++kx) {
                valx += m_xWeights[x * m_xKernelWidth + kx] *
                        m_img[z * m_width * m_height + m_yIndices[y * m_yKernelWidth + ky] * m_width +
                              m_xIndices[x * m_xKernelWidth + kx]];
              }
              valxy += m_yWeights[y * m_yKernelWidth + ky] * valx;
            }
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(valxy);
          }
        }
      }
    } else if (m_xKernelWidth == 1 && m_yKernelWidth == 1 && m_zKernelWidth == 1) {
      for (size_t z = range.begin(); z != range.end(); ++z) {
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(
              m_img[m_zIndices[z] * m_width * m_height + m_yIndices[y] * m_width + m_xIndices[x]]);
          }
        }
      }
    } else if (m_xKernelWidth == 1 && m_yKernelWidth == 1) {
      for (size_t z = range.begin(); z != range.end(); ++z) {
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            double valxyz = 0;
            for (size_t kz = 0; kz < m_zKernelWidth; ++kz) {
              valxyz +=
                m_zWeights[z * m_zKernelWidth + kz] * m_img[m_zIndices[z * m_zKernelWidth + kz] * m_width * m_height +
                                                            m_yIndices[y] * m_width + m_xIndices[x]];
            }
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(valxyz);
          }
        }
      }
    } else if (m_xKernelWidth == 1 && m_zKernelWidth == 1) {
      for (size_t z = range.begin(); z != range.end(); ++z) {
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            double valxyz = 0;
            for (size_t ky = 0; ky < m_yKernelWidth; ++ky) {
              valxyz += m_yWeights[y * m_yKernelWidth + ky] *
                        m_img[m_zIndices[z] * m_width * m_height + m_yIndices[y * m_yKernelWidth + ky] * m_width +
                              m_xIndices[x]];
            }
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(valxyz);
          }
        }
      }
    } else if (m_yKernelWidth == 1 && m_zKernelWidth == 1) {
      for (size_t z = range.begin(); z != range.end(); ++z) {
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            double valxyz = 0;
            for (size_t kx = 0; kx < m_xKernelWidth; ++kx) {
              valxyz += m_xWeights[x * m_xKernelWidth + kx] *
                        m_img[m_zIndices[z] * m_width * m_height + m_yIndices[y] * m_width +
                              m_xIndices[x * m_xKernelWidth + kx]];
            }
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(valxyz);
          }
        }
      }
    } else if (m_xKernelWidth == 1) {
      for (size_t z = range.begin(); z != range.end(); ++z) {
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            double valxyz = 0;
            for (size_t kz = 0; kz < m_zKernelWidth; ++kz) {
              double valxy = 0;
              for (size_t ky = 0; ky < m_yKernelWidth; ++ky) {
                valxy += m_yWeights[y * m_yKernelWidth + ky] *
                         m_img[m_zIndices[z * m_zKernelWidth + kz] * m_width * m_height +
                               m_yIndices[y * m_yKernelWidth + ky] * m_width + m_xIndices[x]];
              }
              valxyz += m_zWeights[z * m_zKernelWidth + kz] * valxy;
            }
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(valxyz);
          }
        }
      }
    } else if (m_yKernelWidth == 1) {
      for (size_t z = range.begin(); z != range.end(); ++z) {
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            double valxyz = 0;
            for (size_t kz = 0; kz < m_zKernelWidth; ++kz) {
              double valx = 0;
              for (size_t kx = 0; kx < m_xKernelWidth; ++kx) {
                valx += m_xWeights[x * m_xKernelWidth + kx] *
                        m_img[m_zIndices[z * m_zKernelWidth + kz] * m_width * m_height + m_yIndices[y] * m_width +
                              m_xIndices[x * m_xKernelWidth + kx]];
              }
              valxyz += m_zWeights[z * m_zKernelWidth + kz] * valx;
            }
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(valxyz);
          }
        }
      }
    } else if (m_zKernelWidth == 1) {
      for (size_t z = range.begin(); z != range.end(); ++z) {
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            double valxy = 0;
            for (size_t ky = 0; ky < m_yKernelWidth; ++ky) {
              double valx = 0;
              for (size_t kx = 0; kx < m_xKernelWidth; ++kx) {
                valx += m_xWeights[x * m_xKernelWidth + kx] *
                        m_img[m_zIndices[z] * m_width * m_height + m_yIndices[y * m_yKernelWidth + ky] * m_width +
                              m_xIndices[x * m_xKernelWidth + kx]];
              }
              valxy += m_yWeights[y * m_yKernelWidth + ky] * valx;
            }
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(valxy);
          }
        }
      }
    } else {
      for (size_t z = range.begin(); z != range.end(); ++z) {
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            double valxyz = 0;
            for (size_t kz = 0; kz < m_zKernelWidth; ++kz) {
              double valxy = 0;
              for (size_t ky = 0; ky < m_yKernelWidth; ++ky) {
                double valx = 0;
                for (size_t kx = 0; kx < m_xKernelWidth; ++kx) {
                  valx += m_xWeights[x * m_xKernelWidth + kx] *
                          m_img[m_zIndices[z * m_zKernelWidth + kz] * m_width * m_height +
                                m_yIndices[y * m_yKernelWidth + ky] * m_width + m_xIndices[x * m_xKernelWidth + kx]];
                }
                valxy += m_yWeights[y * m_yKernelWidth + ky] * valx;
              }
              valxyz += m_zWeights[z * m_zKernelWidth + kz] * valxy;
            }
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(valxyz);
          }
        }
      }
    }
  }

  const TPixel* m_img;
  size_t m_width;
  size_t m_height;
  size_t m_depth;
  TPixelOut* m_imgOut;
  size_t m_outWidth;
  size_t m_outHeight;
  size_t m_outDepth;
  const std::vector<double>& m_xWeights;
  const std::vector<size_t>& m_xIndices;
  size_t m_xKernelWidth;
  bool m_xKernelIsTrivial;
  const std::vector<double>& m_yWeights;
  const std::vector<size_t>& m_yIndices;
  size_t m_yKernelWidth;
  bool m_yKernelIsTrivial;
  const std::vector<double>& m_zWeights;
  const std::vector<size_t>& m_zIndices;
  size_t m_zKernelWidth;
  bool m_zKernelIsTrivial;
};

//((scale-1)/2) in output image maps to 0 in input image, and ((3*scale-1)/2) in output
// image maps to 1 in input image.
// 'antialiasing' specifies whether to perform antialiasing when shrinking an image. For the 'nearest' method,
// the parameter 'antialiasingForNearest' is used (default false); for all other methods, the default is true.
template<typename TPixel, typename TPixelOut>
void image3DResize(const TPixel* img,
                   size_t width,
                   size_t height,
                   size_t depth,
                   TPixelOut* imgOut,
                   size_t outWidth,
                   size_t outHeight,
                   size_t outDepth,
                   Interpolant interpolant = Interpolant::Cubic,
                   bool antialiasing = true,
                   bool antialiasingForNearest = false,
                   bool useMultithreading = true)
{
  std::vector<double> xWeights;
  std::vector<size_t> xIndices;
  size_t xKernelWidth;
  bool xKernelIsTrivial;
  std::vector<double> yWeights;
  std::vector<size_t> yIndices;
  size_t yKernelWidth;
  bool yKernelIsTrivial;
  std::vector<double> zWeights;
  std::vector<size_t> zIndices;
  size_t zKernelWidth;
  bool zKernelIsTrivial;
  _resizeContributions(width,
                       outWidth,
                       interpolant,
                       interpolant == Interpolant::Nearest ? antialiasingForNearest : antialiasing,
                       xWeights,
                       xIndices,
                       xKernelWidth,
                       xKernelIsTrivial);
  _resizeContributions(height,
                       outHeight,
                       interpolant,
                       interpolant == Interpolant::Nearest ? antialiasingForNearest : antialiasing,
                       yWeights,
                       yIndices,
                       yKernelWidth,
                       yKernelIsTrivial);
  _resizeContributions(depth,
                       outDepth,
                       interpolant,
                       interpolant == Interpolant::Nearest ? antialiasingForNearest : antialiasing,
                       zWeights,
                       zIndices,
                       zKernelWidth,
                       zKernelIsTrivial);

  Resize3DForOneBlock<TPixel, TPixelOut> func(img,
                                              width,
                                              height,
                                              depth,
                                              imgOut,
                                              outWidth,
                                              outHeight,
                                              outDepth,
                                              xWeights,
                                              xIndices,
                                              xKernelWidth,
                                              xKernelIsTrivial,
                                              yWeights,
                                              yIndices,
                                              yKernelWidth,
                                              yKernelIsTrivial,
                                              zWeights,
                                              zIndices,
                                              zKernelWidth,
                                              zKernelIsTrivial);
  if (!useMultithreading) {
    func(tbb::blocked_range<size_t>(0, outDepth));
  } else {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, outDepth), func);
  }
}

} // namespace nim
