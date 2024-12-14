#pragma once

#include "zcpuinfo.h"
#include "zlog.h"
#include "zimagesse3.h"
#include "zimageavx.h"
#include "zimageavx512.h"
#include "zbenchtimer.h"
#include "zimginterface.h"
#include "zsaturateoperation.h"
#include "zimagefilterkernel.h"
#include <tbb/parallel_for.h>
#include <boost/align/aligned_allocator.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace nim {

// everything is row major

bool seperate2DKernel(const double* kernel, size_t width, size_t height, double* rowKernel, double* colKernel);

// wrap a pixel coordinate to a valid pixel coordinate inside image of sizes using padOption, image can be any dimension
// if padOption is constant, do nothing
template<typename SignedIntegerType>
void wrapCoordToImage(SignedIntegerType* coord, const size_t* imgSize, size_t numDimensions, PadOption padOption)
{
  if (padOption == PadOption::Symmetric) {
    for (size_t i = 0; i < numDimensions; ++i) {
      if (coord[i] < 0) {
        coord[i] = -coord[i] - 1;
      }
      if ((coord[i] / imgSize[i]) % 2 == 0) {
        coord[i] = coord[i] % imgSize[i];
      } else {
        coord[i] = imgSize[i] - 1 - coord[i] % imgSize[i];
      }
    }
  } else if (padOption == PadOption::Replicate) {
    for (size_t i = 0; i < numDimensions; ++i) {
      if (coord[i] < 0) {
        coord[i] = 0;
      }
      if (coord[i] > static_cast<SignedIntegerType>(imgSize[i]) - 1) {
        coord[i] = imgSize[i] - 1;
      }
    }
  } else if (padOption == PadOption::Circular) {
    for (size_t i = 0; i < numDimensions; ++i) {
      if (coord[i] >= 0) {
        coord[i] = coord[i] % imgSize[i];
      } else {
        coord[i] += ((-coord[i] - 1) / imgSize[i] + 1) * imgSize[i];
      }
    }
  }
}

template<>
void wrapCoordToImage<size_t>(size_t* coord, const size_t* imgSize, size_t numDimensions, PadOption padOption);

template<typename SignedIntegerType, typename TPixel>
TPixel getImage2DPixelValue(const TPixel* img,
                            size_t width,
                            size_t height,
                            SignedIntegerType x,
                            SignedIntegerType y,
                            PadOption padOption = PadOption::Constant,
                            TPixel padValue = TPixel(0))
{
  if (x >= 0 && x < static_cast<SignedIntegerType>(width) && y >= 0 && y < static_cast<SignedIntegerType>(height)) {
    return img[y * width + x];
  }

  if (padOption == PadOption::Constant) {
    return padValue;
  }

  SignedIntegerType coord[2];
  coord[0] = x;
  coord[1] = y;
  size_t imgSize[2];
  imgSize[0] = width;
  imgSize[1] = height;
  wrapCoordToImage(coord, imgSize, 2, padOption);
  return img[coord[1] * width + coord[0]];
}

// imgOut should be preallocated and not same as img
template<typename TPixel>
void image2DPad(const TPixel* img,
                size_t width,
                size_t height,
                size_t leftPad,
                size_t rightPad,
                size_t upPad,
                size_t downPad,
                TPixel* imgOut,
                PadOption padOption = PadOption::Constant,
                TPixel padValue = TPixel(0))
{
  DCHECK_NE(img, imgOut);
  if (leftPad == 0 && rightPad == 0 && upPad == 0 && downPad == 0) {
    std::copy_n(img, width * height, imgOut);
  } else {
    size_t desWidth = leftPad + width + rightPad;
    size_t desHeight = upPad + height + downPad;

    // boundary
    if (padOption == PadOption::Constant) {
      std::fill(imgOut, imgOut + desWidth * desHeight, padValue);
    } else if (padOption == PadOption::Symmetric) {
      // corner
      for (size_t j = 0; j < upPad; ++j) {
        size_t refY = upPad - j - 1;
        if ((refY / height) % 2 == 1) {
          refY = height - 1 - refY % height;
        } else {
          refY = refY % height;
        }
        for (size_t i = 0; i < leftPad; ++i) {
          size_t refX = leftPad - i - 1;
          if ((refX / width) % 2 == 1) {
            refX = width - 1 - refX % width;
          } else {
            refX = refX % width;
          }
          imgOut[j * desWidth + i] = img[refY * width + refX];
        }
      }
      for (size_t j = 0; j < upPad; ++j) {
        size_t refY = upPad - j - 1;
        if ((refY / height) % 2 == 1) {
          refY = height - 1 - refY % height;
        } else {
          refY = refY % height;
        }
        for (size_t i = desWidth - rightPad; i < desWidth; ++i) {
          size_t refX = width - (i - desWidth + rightPad) - 1;
          if ((refX / width) % 2 == 1) {
            refX = width - 1 - refX % width;
          } else {
            refX = refX % width;
          }
          imgOut[j * desWidth + i] = img[refY * width + refX];
        }
      }
      for (size_t j = desHeight - downPad; j < desHeight; ++j) {
        size_t refY = height - (j - desHeight + downPad) - 1;
        if ((refY / height) % 2 == 1) {
          refY = height - 1 - refY % height;
        } else {
          refY = refY % height;
        }
        for (size_t i = 0; i < leftPad; ++i) {
          size_t refX = leftPad - i - 1;
          if ((refX / width) % 2 == 1) {
            refX = width - 1 - refX % width;
          } else {
            refX = refX % width;
          }
          imgOut[j * desWidth + i] = img[refY * width + refX];
        }
      }
      for (size_t j = desHeight - downPad; j < desHeight; ++j) {
        size_t refY = height - (j - desHeight + downPad) - 1;
        if ((refY / height) % 2 == 1) {
          refY = height - 1 - refY % height;
        } else {
          refY = refY % height;
        }
        for (size_t i = desWidth - rightPad; i < desWidth; ++i) {
          size_t refX = width - (i - desWidth + rightPad) - 1;
          if ((refX / width) % 2 == 1) {
            refX = width - 1 - refX % width;
          } else {
            refX = refX % width;
          }
          imgOut[j * desWidth + i] = img[refY * width + refX];
        }
      }
      // left
      for (size_t i = 0; i < leftPad; ++i) {
        size_t refX = leftPad - i - 1;
        if ((refX / width) % 2 == 1) {
          refX = width - 1 - refX % width;
        } else {
          refX = refX % width;
        }
        for (size_t j = 0; j < height; ++j) {
          imgOut[(j + upPad) * desWidth + i] = img[j * width + refX];
        }
      }
      // right
      for (size_t i = desWidth - rightPad; i < desWidth; ++i) {
        size_t refX = width - (i - desWidth + rightPad) - 1;
        if ((refX / width) % 2 == 1) {
          refX = width - 1 - refX % width;
        } else {
          refX = refX % width;
        }
        for (size_t j = 0; j < height; ++j) {
          imgOut[(j + upPad) * desWidth + i] = img[j * width + refX];
        }
      }
      // up
      for (size_t j = 0; j < upPad; ++j) {
        size_t refY = upPad - j - 1;
        if ((refY / height) % 2 == 1) {
          refY = height - 1 - refY % height;
        } else {
          refY = refY % height;
        }
        std::copy_n(img + refY * width, width, imgOut + j * desWidth + leftPad);
      }
      // down
      for (size_t j = desHeight - downPad; j < desHeight; ++j) {
        size_t refY = height - (j - desHeight + downPad) - 1;
        if ((refY / height) % 2 == 1) {
          refY = height - 1 - refY % height;
        } else {
          refY = refY % height;
        }
        std::copy_n(img + refY * width, width, imgOut + j * desWidth + leftPad);
      }
    }

    // copy image
    if (leftPad == 0 && rightPad == 0) {
      TPixel* desStart = imgOut + upPad * width;
      std::copy_n(img, width * height, desStart);
    } else {
      TPixel* desStart = imgOut + upPad * desWidth + leftPad;
      for (size_t i = 0; i < height; ++i) {
        std::copy_n(img + i * width, width, desStart);
        desStart += desWidth;
      }
    }

    if (padOption == PadOption::Replicate) { // replicate
      // left
      for (size_t i = 0; i < leftPad; ++i) {
        for (size_t j = 0; j < height; ++j) {
          imgOut[(j + upPad) * desWidth + i] = img[j * width + 0];
        }
      }
      // right
      for (size_t i = desWidth - rightPad; i < desWidth; ++i) {
        for (size_t j = 0; j < height; ++j) {
          imgOut[(j + upPad) * desWidth + i] = img[j * width + width - 1];
        }
      }
      // up
      for (size_t j = 0; j < upPad; ++j) {
        std::copy_n(imgOut + upPad * desWidth, desWidth, imgOut + j * desWidth);
      }
      // down
      for (size_t j = desHeight - downPad; j < desHeight; ++j) {
        std::copy_n(imgOut + (desHeight - 1 - downPad) * desWidth, desWidth, imgOut + j * desWidth);
      }
    } else if (padOption == PadOption::Circular) { // circular
      // left
      index_t refX = width;
      for (size_t i = leftPad; i-- > 0;) {
        --refX;
        refX = refX < 0 ? static_cast<index_t>(width) - 1 : refX;
        for (size_t j = 0; j < height; ++j) {
          imgOut[(j + upPad) * desWidth + i] = img[j * width + refX];
        }
      }
      // right
      refX = -1;
      for (size_t i = desWidth - rightPad; i < desWidth; ++i) {
        ++refX;
        refX = refX >= static_cast<index_t>(width) ? 0 : refX;
        for (size_t j = 0; j < height; ++j) {
          imgOut[(j + upPad) * desWidth + i] = img[j * width + refX];
        }
      }
      // up
      index_t refY = height;
      for (size_t j = upPad; j-- > 0;) {
        --refY;
        refY = refY < 0 ? static_cast<index_t>(height) - 1 : refY;
        std::copy_n(imgOut + (refY + upPad) * desWidth, desWidth, imgOut + j * desWidth);
      }
      // down
      refY = -1;
      for (size_t j = desHeight - downPad; j < desHeight; ++j) {
        ++refY;
        refY = refY >= static_cast<index_t>(height) ? 0 : refY;
        std::copy_n(imgOut + (refY + upPad) * desWidth, desWidth, imgOut + j * desWidth);
      }
    }
  }
}

template<typename TPixel>
void image2DFlip(TPixel* img, size_t width, size_t height, Dimension dim)
{
  if (dim == Dimension::X) {
    if (width <= 1) {
      return;
    }
    for (size_t i = 0; i < height; ++i) {
      TPixel* start = img + i * width;
      std::reverse(start, start + width);
    }
  } else if (dim == Dimension::Y) {
    if (height <= 1) {
      return;
    }
    std::vector<TPixel> buffer(width);
    size_t j = 0;
    size_t k = height - 1;
    while (j < k) {
      std::copy_n(img + j * width, width, buffer.data());
      std::copy_n(img + k * width, width, img + j * width);
      std::copy_n(buffer.data(), width, img + k * width);
      ++j;
      --k;
    }
  }
}

// same as flip all dimensions
template<typename TPixel>
void image2DReflect(TPixel* img, size_t width, size_t height)
{
  std::reverse(img, img + width * height);
}

template<typename TPixel>
void image2DTranspose(TPixel* img, size_t width, size_t height)
{
  if (width == height) {
    for (size_t i = 0; i < height; ++i) {
      for (size_t j = i + 1; j < width; ++j) {
        std::swap(img[i + j * height], img[j + i * width]);
      }
    }
  } else {
    constexpr size_t blockSize = 32;
    std::vector<TPixel> buf(width * height);
    for (size_t i = 0; i < height; i += blockSize) {
      for (size_t j = 0; j < width; j += blockSize) {
        // transpose the block beginning at [i,j]
        size_t maxH = std::min(i + blockSize, height);
        size_t maxW = std::min(j + blockSize, width);
        for (size_t k = i; k < maxH; ++k) {
          for (size_t l = j; l < maxW; ++l) {
            buf[k + l * height] = img[l + k * width];
          }
        }
      }
    }
    std::copy_n(buf.data(), width * height, img);
  }
}

template<typename TPixel, typename TPixelOut = TPixel>
struct Image2DFilterForOneBlock
{
  Image2DFilterForOneBlock(const TPixel* padImg,
                           size_t padImgWidth,
                           const double* kernel,
                           size_t kernelWidth,
                           size_t kernelHeight,
                           TPixelOut* imgOut,
                           size_t imgOutWidth)
    : m_padImg(padImg)
    , m_padImgWidth(padImgWidth)
    , m_kernel(kernel)
    , m_kernelWidth(kernelWidth)
    , m_kernelHeight(kernelHeight)
    , m_imgOut(imgOut)
    , m_imgOutWidth(imgOutWidth)
  {}

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    for (size_t j = range.begin(); j != range.end(); ++j) {
      for (size_t i = 0; i < m_imgOutWidth; ++i) {
        double sum = 0.0;
        for (size_t r = 0; r < m_kernelHeight; ++r) { // row by row
          const TPixel* imgStart = m_padImg + (j + r) * m_padImgWidth + i;
          sum = std::inner_product(imgStart, imgStart + m_kernelWidth, m_kernel + r * m_kernelWidth, sum);
        }
        m_imgOut[j * m_imgOutWidth + i] = saturate_cast<TPixelOut>(sum);
      }
    }
  }

  const TPixel* m_padImg;
  size_t m_padImgWidth;
  const double* m_kernel;
  size_t m_kernelWidth;
  size_t m_kernelHeight;
  TPixelOut* m_imgOut;
  size_t m_imgOutWidth;
};

template<>
struct Image2DFilterForOneBlock<double, double>
{
  Image2DFilterForOneBlock(const double* padImg,
                           size_t padImgWidth,
                           const double* kernel,
                           size_t kernelWidth,
                           size_t kernelHeight,
                           double* imgOut,
                           size_t imgOutWidth)
    : m_padImg(padImg)
    , m_padImgWidth(padImgWidth)
    , m_kernel(kernel)
    , m_kernelWidth(kernelWidth)
    , m_kernelHeight(kernelHeight)
    , m_imgOut(imgOut)
    , m_imgOutWidth(imgOutWidth)
  {}

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    if (ZCpuInfo::instance().bAVX512F && m_kernelWidth >= 8) {
      Image2DFilterForOneBlock_AVX512(m_padImg,
                                      m_padImgWidth,
                                      m_kernel,
                                      m_kernelWidth,
                                      m_kernelHeight,
                                      m_imgOut,
                                      m_imgOutWidth,
                                      range.begin(),
                                      range.end());
    } else if (m_kernelWidth >= 4) {
      Image2DFilterForOneBlock_AVX(m_padImg,
                                   m_padImgWidth,
                                   m_kernel,
                                   m_kernelWidth,
                                   m_kernelHeight,
                                   m_imgOut,
                                   m_imgOutWidth,
                                   range.begin(),
                                   range.end());
    } else if (m_kernelWidth >= 2) {
      Image2DFilterForOneBlock_SSE3(m_padImg,
                                    m_padImgWidth,
                                    m_kernel,
                                    m_kernelWidth,
                                    m_kernelHeight,
                                    m_imgOut,
                                    m_imgOutWidth,
                                    range.begin(),
                                    range.end());
    } else {
      for (size_t j = range.begin(); j != range.end(); ++j) {
        for (size_t i = 0; i < m_imgOutWidth; ++i) {
          double sum = 0.0;
          for (size_t r = 0; r < m_kernelHeight; ++r) { // row by row
            const double* imgStart = m_padImg + (j + r) * m_padImgWidth + i;
            sum = std::inner_product(imgStart, imgStart + m_kernelWidth, m_kernel + r * m_kernelWidth, sum);
          }
          m_imgOut[j * m_imgOutWidth + i] = sum;
        }
      }
    }
  }

  const double* m_padImg;
  size_t m_padImgWidth;
  const double* m_kernel;
  size_t m_kernelWidth;
  size_t m_kernelHeight;
  double* m_imgOut;
  size_t m_imgOutWidth;
};

template<typename TPixel, typename TPixelOut = TPixel>
struct Image2DRowFilterForOneBlock
{
  Image2DRowFilterForOneBlock(const TPixel* padImg,
                              size_t padImgWidth,
                              const double* kernel,
                              size_t kernelWidth,
                              TPixelOut* imgOut,
                              size_t imgOutWidth)
    : m_padImg(padImg)
    , m_padImgWidth(padImgWidth)
    , m_kernel(kernel)
    , m_kernelWidth(kernelWidth)
    , m_imgOut(imgOut)
    , m_imgOutWidth(imgOutWidth)
  {}

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    for (size_t j = range.begin(); j != range.end(); ++j) {
      for (size_t i = 0; i < m_imgOutWidth; ++i) {
        double sum = 0.0;
        const TPixel* imgStart = m_padImg + j * m_padImgWidth + i;
        sum = std::inner_product(imgStart, imgStart + m_kernelWidth, m_kernel, sum);
        m_imgOut[j * m_imgOutWidth + i] = saturate_cast<TPixelOut>(sum);
      }
    }
  }

  const TPixel* m_padImg;
  size_t m_padImgWidth;
  const double* m_kernel;
  size_t m_kernelWidth;
  TPixelOut* m_imgOut;
  size_t m_imgOutWidth;
};

template<>
struct Image2DRowFilterForOneBlock<double, double>
{
  Image2DRowFilterForOneBlock(const double* padImg,
                              size_t padImgWidth,
                              const double* kernel,
                              size_t kernelWidth,
                              double* imgOut,
                              size_t imgOutWidth)
    : m_padImg(padImg)
    , m_padImgWidth(padImgWidth)
    , m_kernel(kernel)
    , m_kernelWidth(kernelWidth)
    , m_imgOut(imgOut)
    , m_imgOutWidth(imgOutWidth)
  {}

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    if (ZCpuInfo::instance().bAVX512F && m_kernelWidth >= 8) {
      Image2DRowFilterForOneBlock_AVX512(m_padImg,
                                         m_padImgWidth,
                                         m_kernel,
                                         m_kernelWidth,
                                         m_imgOut,
                                         m_imgOutWidth,
                                         range.begin(),
                                         range.end());
    } else if (m_kernelWidth >= 4) {
      Image2DRowFilterForOneBlock_AVX(m_padImg,
                                      m_padImgWidth,
                                      m_kernel,
                                      m_kernelWidth,
                                      m_imgOut,
                                      m_imgOutWidth,
                                      range.begin(),
                                      range.end());
    } else if (m_kernelWidth >= 2) {
      Image2DRowFilterForOneBlock_SSE3(m_padImg,
                                       m_padImgWidth,
                                       m_kernel,
                                       m_kernelWidth,
                                       m_imgOut,
                                       m_imgOutWidth,
                                       range.begin(),
                                       range.end());
    } else {
      for (size_t j = range.begin(); j != range.end(); ++j) {
        for (size_t i = 0; i < m_imgOutWidth; ++i) {
          double sum = 0.0;
          const double* imgStart = m_padImg + j * m_padImgWidth + i;
          sum = std::inner_product(imgStart, imgStart + m_kernelWidth, m_kernel, sum);
          m_imgOut[j * m_imgOutWidth + i] = sum;
        }
      }
    }
  }

  const double* m_padImg;
  size_t m_padImgWidth;
  const double* m_kernel;
  size_t m_kernelWidth;
  double* m_imgOut;
  size_t m_imgOutWidth;
};

template<typename TPixel, typename TPixelOut = TPixel>
struct Image2DColFilterForOneBlock
{
  Image2DColFilterForOneBlock(const TPixel* padImg,
                              size_t padImgWidth,
                              const double* kernel,
                              size_t kernelHeight,
                              TPixelOut* imgOut,
                              size_t imgOutWidth)
    : m_padImg(padImg)
    , m_padImgWidth(padImgWidth)
    , m_kernel(kernel)
    , m_kernelHeight(kernelHeight)
    , m_imgOut(imgOut)
    , m_imgOutWidth(imgOutWidth)
  {}

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    for (size_t j = range.begin(); j != range.end(); ++j) {
      for (size_t i = 0; i < m_imgOutWidth; ++i) {
        double sum = 0.0;
        for (size_t r = 0; r < m_kernelHeight; ++r) { // row by row
          sum += m_kernel[r] * (*(m_padImg + (j + r) * m_padImgWidth + i));
        }
        m_imgOut[j * m_imgOutWidth + i] = saturate_cast<TPixelOut>(sum);
      }
    }
  }

  const TPixel* m_padImg;
  size_t m_padImgWidth;
  const double* m_kernel;
  size_t m_kernelHeight;
  TPixelOut* m_imgOut;
  size_t m_imgOutWidth;
};

template<typename TPixel, typename TPixelOut>
void image2DFilter(const TPixel* img,
                   size_t width,
                   size_t height,
                   const double* kernel,
                   size_t kernelWidth,
                   size_t kernelHeight,
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
  size_t desWidth = leftPad + width + rightPad;
  size_t desHeight = upPad + height + downPad;
  std::vector<TPixel, boost::alignment::aligned_allocator<TPixel, 64>> padImg(desWidth * desHeight);
  // ZBenchTimer bt;
  // bt.start();
  image2DPad(img, width, height, leftPad, rightPad, upPad, downPad, padImg.data(), boundaryOption, boundaryValue);
  // bt.stopAndPrint();

  // image2DWrite(padImg.data(), desWidth, desHeight, "/Users/feng/Downloads/padImg.tif");

  std::vector<double, boost::alignment::aligned_allocator<double, 64>> alignedKernel;
  alignedKernel.insert(alignedKernel.end(), kernel, kernel + kernelWidth * kernelHeight);
  if (!corr) {
    image2DReflect(alignedKernel.data(), kernelWidth, kernelHeight);
  }

  std::vector<double, boost::alignment::aligned_allocator<double, 64>> rowKernel(kernelWidth);
  std::vector<double, boost::alignment::aligned_allocator<double, 64>> colKernel(kernelHeight);
  if (seperate2DKernel(alignedKernel.data(), kernelWidth, kernelHeight, rowKernel.data(), colKernel.data())) {
    std::vector<double, boost::alignment::aligned_allocator<double, 64>> bufImg(width * desHeight);

    Image2DRowFilterForOneBlock<TPixel, double> rowfunctor(padImg.data(),
                                                           desWidth,
                                                           rowKernel.data(),
                                                           kernelWidth,
                                                           bufImg.data(),
                                                           width);
    Image2DColFilterForOneBlock<double, TPixelOut> colfunctor(bufImg.data(),
                                                              width,
                                                              colKernel.data(),
                                                              kernelHeight,
                                                              imgOut,
                                                              width);

    // get correlation of padImg and adjKernel
    if (!useMultithreading) {
      rowfunctor(tbb::blocked_range<size_t>(0, desHeight));
      colfunctor(tbb::blocked_range<size_t>(0, height));
    } else {
      tbb::parallel_for(tbb::blocked_range<size_t>(0, desHeight), rowfunctor);
      tbb::parallel_for(tbb::blocked_range<size_t>(0, height), colfunctor);
    }
  } else {
    // get correlation of padImg and adjKernel
    Image2DFilterForOneBlock<TPixel, TPixelOut>
      functor(padImg.data(), desWidth, alignedKernel.data(), kernelWidth, kernelHeight, imgOut, width);
    if (!useMultithreading) {
      functor(tbb::blocked_range<size_t>(0, height));
    } else {
      tbb::parallel_for(tbb::blocked_range<size_t>(0, height), functor);
    }
  }
}

template<typename TPixel, typename TPixelOut>
void image2DFilter(const TPixel* img,
                   size_t width,
                   size_t height,
                   const double* rowkernel,
                   size_t kernelWidth,
                   const double* colkernel,
                   size_t kernelHeight,
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
  size_t desWidth = leftPad + width + rightPad;
  size_t desHeight = upPad + height + downPad;
  std::vector<TPixel, boost::alignment::aligned_allocator<TPixel, 64>> padImg(desWidth * desHeight);
  // ZBenchTimer bt;
  // bt.start();
  image2DPad(img, width, height, leftPad, rightPad, upPad, downPad, padImg.data(), boundaryOption, boundaryValue);
  // bt.stopAndPrint();

  // image2DWrite(padImg.data(), desWidth, desHeight, "/Users/feng/Downloads/padImg.tif");

  std::vector<double, boost::alignment::aligned_allocator<double, 64>> alignedRowKernel;
  std::vector<double, boost::alignment::aligned_allocator<double, 64>> alignedColKernel;
  alignedRowKernel.insert(alignedRowKernel.end(), rowkernel, rowkernel + kernelWidth);
  alignedColKernel.insert(alignedColKernel.end(), colkernel, colkernel + kernelHeight);
  if (!corr) {
    image2DFlip(alignedRowKernel.data(), kernelWidth, 1, Dimension::X);
    image2DFlip(alignedColKernel.data(), kernelHeight, 1, Dimension::X);
  }

  std::vector<double, boost::alignment::aligned_allocator<double, 64>> bufImg(width * desHeight);

  Image2DRowFilterForOneBlock<TPixel, double> rowfunctor(padImg.data(),
                                                         desWidth,
                                                         alignedRowKernel.data(),
                                                         kernelWidth,
                                                         bufImg.data(),
                                                         width);
  Image2DColFilterForOneBlock<double, TPixelOut> colfunctor(bufImg.data(),
                                                            width,
                                                            alignedColKernel.data(),
                                                            kernelHeight,
                                                            imgOut,
                                                            width);

  // get correlation of padImg and adjKernel
  if (!useMultithreading) {
    rowfunctor(tbb::blocked_range<size_t>(0, desHeight));
    colfunctor(tbb::blocked_range<size_t>(0, height));
  } else {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, desHeight), rowfunctor);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, height), colfunctor);
  }
}

// boundaryOption see image2DFilter
template<typename TPixel, typename TPixelOut>
void image2DGaussianFilter(const TPixel* img,
                           size_t width,
                           size_t height,
                           double kernelSigmaX,
                           double kernelSigmaY,
                           TPixelOut* imgOut,
                           index_t kernelWidth = -1,
                           index_t kernelHeight = -1,
                           PadOption boundaryOption = PadOption::Constant,
                           TPixel boundaryValue = TPixel(0),
                           bool useMultithreading = true)
{
  size_t kWidth;
  size_t kHeight;
#if 0
  std::vector<double> kernel = create2DGaussianKernel(kernelSigmaX, kernelSigmaY,
                                                      kernelWidth, kernelHeight,
                                                      &kWidth, &kHeight);

  //ZBenchTimer bt;
  //bt.start();
  image2DFilter(img, width, height, kernel.data(), kWidth, kHeight,
      imgOut, boundaryOption, boundaryValue, true, useMultithreading);
  //bt.stopAndPrint();
#else
  std::vector<double> rowkernel = create1DGaussianKernel(kernelSigmaX, kernelWidth, &kWidth);
  std::vector<double> colkernel = create1DGaussianKernel(kernelSigmaY, kernelHeight, &kHeight);

  // ZBenchTimer bt;
  // bt.start();
  image2DFilter(img,
                width,
                height,
                rowkernel.data(),
                kWidth,
                colkernel.data(),
                kHeight,
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
void image2DLoGFilter(const TPixel* img,
                      size_t width,
                      size_t height,
                      double kernelSigmaX,
                      double kernelSigmaY,
                      TPixelOut* imgOut,
                      index_t kernelWidth = -1,
                      index_t kernelHeight = -1,
                      PadOption boundaryOption = PadOption::Constant,
                      TPixel boundaryValue = TPixel(0),
                      bool useMultithreading = true)
{
  size_t kWidth;
  size_t kHeight;

  std::vector<double> rowLoGkernel = create1DLoGKernel(kernelSigmaX, kernelWidth, &kWidth);
  std::vector<double> colLoGkernel = create1DLoGKernel(kernelSigmaY, kernelHeight, &kHeight);
  std::vector<double> rowGkernel = create1DGaussianKernel(kernelSigmaX, kernelWidth, &kWidth);
  std::vector<double> colGkernel = create1DGaussianKernel(kernelSigmaY, kernelHeight, &kHeight);

  // ZBenchTimer bt;
  // bt.start();
  image2DFilter(img,
                width,
                height,
                rowLoGkernel.data(),
                kWidth,
                colGkernel.data(),
                kHeight,
                imgOut,
                boundaryOption,
                boundaryValue,
                true,
                useMultithreading);

  std::vector<TPixelOut> bufImg(width * height);

  image2DFilter(img,
                width,
                height,
                rowGkernel.data(),
                kWidth,
                colLoGkernel.data(),
                kHeight,
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

void _resizeContributions(size_t inLength,
                          size_t outLength,
                          Interpolant interpolant,
                          bool antialiasing,
                          std::vector<double>& weights,
                          std::vector<size_t>& indices,
                          size_t& kernelWidth,
                          bool& kernelIsTrivial);

template<typename TPixel, typename TPixelOut>
struct Resize2DForOneBlock
{
  Resize2DForOneBlock(const TPixel* img,
                      size_t width,
                      size_t height,
                      TPixelOut* imgOut,
                      size_t outWidth,
                      size_t outHeight,
                      const std::vector<double>& xWeights,
                      const std::vector<size_t>& xIndices,
                      size_t xKernelWidth,
                      bool xKernelIsTrivial,
                      const std::vector<double>& yWeights,
                      const std::vector<size_t>& yIndices,
                      size_t yKernelWidth,
                      bool yKernelIsTrivial)
    : m_img(img)
    , m_width(width)
    , m_height(height)
    , m_imgOut(imgOut)
    , m_outWidth(outWidth)
    , m_outHeight(outHeight)
    , m_xWeights(xWeights)
    , m_xIndices(xIndices)
    , m_xKernelWidth(xKernelWidth)
    , m_xKernelIsTrivial(xKernelIsTrivial)
    , m_yWeights(yWeights)
    , m_yIndices(yIndices)
    , m_yKernelWidth(yKernelWidth)
    , m_yKernelIsTrivial(yKernelIsTrivial)
  {}

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    if (m_xKernelIsTrivial && m_yKernelIsTrivial) {
      for (size_t y = range.begin(); y != range.end(); ++y) {
        for (size_t x = 0; x < m_outWidth; ++x) {
          m_imgOut[y * m_outWidth + x] = saturate_cast<TPixelOut>(m_img[y * m_width + x]);
        }
      }
    } else if (m_xKernelIsTrivial) {
      for (size_t y = range.begin(); y != range.end(); ++y) {
        for (size_t x = 0; x < m_outWidth; ++x) {
          double valxy = 0;
          for (size_t ky = 0; ky < m_yKernelWidth; ++ky) {
            valxy += m_yWeights[y * m_yKernelWidth + ky] * m_img[m_yIndices[y * m_yKernelWidth + ky] * m_width + x];
          }
          m_imgOut[y * m_outWidth + x] = saturate_cast<TPixelOut>(valxy);
        }
      }
    } else if (m_yKernelIsTrivial) {
      for (size_t y = range.begin(); y != range.end(); ++y) {
        for (size_t x = 0; x < m_outWidth; ++x) {
          double valx = 0;
          for (size_t kx = 0; kx < m_xKernelWidth; ++kx) {
            valx += m_xWeights[x * m_xKernelWidth + kx] * m_img[y * m_width + m_xIndices[x * m_xKernelWidth + kx]];
          }
          m_imgOut[y * m_outWidth + x] = saturate_cast<TPixelOut>(valx);
        }
      }
    } else if (m_xKernelWidth == 1 && m_yKernelWidth == 1) {
      for (size_t y = range.begin(); y != range.end(); ++y) {
        for (size_t x = 0; x < m_outWidth; ++x) {
          m_imgOut[y * m_outWidth + x] = saturate_cast<TPixelOut>(m_img[m_yIndices[y] * m_width + m_xIndices[x]]);
        }
      }
    } else if (m_xKernelWidth == 1) {
      for (size_t y = range.begin(); y != range.end(); ++y) {
        for (size_t x = 0; x < m_outWidth; ++x) {
          double valxy = 0;
          for (size_t ky = 0; ky < m_yKernelWidth; ++ky) {
            valxy += m_yWeights[y * m_yKernelWidth + ky] *
                     m_img[m_yIndices[y * m_yKernelWidth + ky] * m_width + m_xIndices[x]];
          }
          m_imgOut[y * m_outWidth + x] = saturate_cast<TPixelOut>(valxy);
        }
      }
    } else if (m_yKernelWidth == 1) {
      for (size_t y = range.begin(); y != range.end(); ++y) {
        for (size_t x = 0; x < m_outWidth; ++x) {
          double valx = 0;
          for (size_t kx = 0; kx < m_xKernelWidth; ++kx) {
            valx += m_xWeights[x * m_xKernelWidth + kx] *
                    m_img[m_yIndices[y] * m_width + m_xIndices[x * m_xKernelWidth + kx]];
          }
          m_imgOut[y * m_outWidth + x] = saturate_cast<TPixelOut>(valx);
        }
      }
    } else {
      for (size_t y = range.begin(); y != range.end(); ++y) {
        for (size_t x = 0; x < m_outWidth; ++x) {
          double valxy = 0;
          for (size_t ky = 0; ky < m_yKernelWidth; ++ky) {
            double valx = 0;
            for (size_t kx = 0; kx < m_xKernelWidth; ++kx) {
              valx += m_xWeights[x * m_xKernelWidth + kx] *
                      m_img[m_yIndices[y * m_yKernelWidth + ky] * m_width + m_xIndices[x * m_xKernelWidth + kx]];
            }
            valxy += m_yWeights[y * m_yKernelWidth + ky] * valx;
          }
          m_imgOut[y * m_outWidth + x] = saturate_cast<TPixelOut>(valxy);
        }
      }
    }
  }

  const TPixel* m_img;
  size_t m_width;
  size_t m_height;
  TPixelOut* m_imgOut;
  size_t m_outWidth;
  size_t m_outHeight;
  const std::vector<double>& m_xWeights;
  const std::vector<size_t>& m_xIndices;
  size_t m_xKernelWidth;
  bool m_xKernelIsTrivial;
  const std::vector<double>& m_yWeights;
  const std::vector<size_t>& m_yIndices;
  size_t m_yKernelWidth;
  bool m_yKernelIsTrivial;
};

//((scale-1)/2) in output image maps to 0 in input image, and ((3*scale-1)/2) in output
// image maps to 1 in input image.
// 'antialiasing' specifies whether to perform antialiasing when shrinking an image. For the 'nearest' method,
// the parameter 'antialiasingForNearest' is used (default false); for all other methods, the default is true.
template<typename TPixel, typename TPixelOut>
void image2DResize(const TPixel* img,
                   size_t width,
                   size_t height,
                   TPixelOut* imgOut,
                   size_t outWidth,
                   size_t outHeight,
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

  Resize2DForOneBlock<TPixel, TPixelOut> func(img,
                                              width,
                                              height,
                                              imgOut,
                                              outWidth,
                                              outHeight,
                                              xWeights,
                                              xIndices,
                                              xKernelWidth,
                                              xKernelIsTrivial,
                                              yWeights,
                                              yIndices,
                                              yKernelWidth,
                                              yKernelIsTrivial);
  if (!useMultithreading) {
    func(tbb::blocked_range<size_t>(0, outHeight));
  } else {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, outHeight), func);
  }
}

} // namespace nim
