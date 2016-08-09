#pragma once

#include <cstring>
#include <cmath>
#include <algorithm>
#include <numeric>
#include "zlog.h"
//#include "zbenchtimer.h"

#ifndef _USE_QTCONCURRENT_
#include <tbb/parallel_for.h>
#else
#include <QtConcurrent/QtConcurrentMap>
#endif

#include <QList>
#include <utility>
#include <boost/align/aligned_allocator.hpp>
#include "zcpuinfo.h"
#include "zimagesse3.h"
#include "zimageavx.h"
#include "zimage2dutils.h"

namespace nim {

// everything is row major

template<typename SignedIntegerType, typename TPixel>
TPixel getImage3DPixelValue(const TPixel* img, size_t width, size_t height, size_t depth,
                            SignedIntegerType x, SignedIntegerType y, SignedIntegerType z,
                            PadOption padOption = PadOption::Constant, TPixel padValue = TPixel(0))
{
  if (x >= 0 && x < static_cast<SignedIntegerType>(width) &&
      y >= 0 && y < static_cast<SignedIntegerType>(height) &&
      z >= 0 && z < static_cast<SignedIntegerType>(depth))
    return img[static_cast<size_t>(z) * width * height + y * width + x];

  if (padOption == PadOption::Constant) {
    return padValue;
  } else {
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
}

template<typename TPixel>
TPixel getImage3DPixelValue(const TPixel* img, size_t width, size_t height, size_t depth,
                            size_t x, size_t y, size_t z,
                            PadOption padOption = PadOption::Constant, TPixel padValue = TPixel(0))
{
  if (x < width && y < height && z < depth)
    return img[z * width * height + y * width + x];

  if (padOption == PadOption::Constant) {
    return padValue;
  } else {
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
}

// imgOut should be preallocated and not same as img
template<typename TPixel>
void image3DPad(const TPixel* img, size_t width, size_t height, size_t depth,
                size_t leftPad, size_t rightPad, size_t upPad, size_t downPad, size_t frontPad, size_t backPad,
                TPixel* imgOut, PadOption padOption = PadOption::Constant, TPixel padValue = TPixel(0))
{
  DCHECK_NE(img, imgOut);

  size_t plane = width * height;

  if (leftPad == 0 && rightPad == 0 && upPad == 0 && downPad == 0 && frontPad == 0 && backPad == 0) {
    memcpy(imgOut, img, sizeof(TPixel) * plane * depth);
  } else {
    size_t desWidth = leftPad + width + rightPad;
    size_t desHeight = upPad + height + downPad;
    size_t desDepth = depth + frontPad + backPad;
    size_t desPlane = desWidth * desHeight;

    // copy image
    for (size_t j = 0; j < depth; ++j) {
      if (leftPad == 0 && rightPad == 0) {
        TPixel* desStart = imgOut + upPad * width + (j + frontPad) * desPlane;
        memcpy(desStart, img + j * plane, sizeof(TPixel) * plane);
      } else {
        TPixel* desStart = imgOut + upPad * desWidth + leftPad + (j + frontPad) * desPlane;
        for (size_t i = 0; i < height; ++i) {
          memcpy(desStart, img + i * width + j * plane, sizeof(TPixel) * width);
          desStart += desWidth;
        }
      }
    }

    // boundary
    for (size_t k = 0; k < frontPad; ++k) {
      for (size_t j = 0; j < desHeight; ++j) {
        for (size_t i = 0; i < desWidth; ++i) {
          imgOut[k * desHeight * desWidth + j * desWidth + i] = getImage3DPixelValue(img, width, height, depth,
                                                                                     i - leftPad, j - upPad,
                                                                                     k - frontPad,
                                                                                     padOption, padValue);
        }
      }
    }
    for (size_t k = frontPad + depth; k < desDepth; ++k) {
      for (size_t j = 0; j < desHeight; ++j) {
        for (size_t i = 0; i < desWidth; ++i) {
          imgOut[k * desHeight * desWidth + j * desWidth + i] = getImage3DPixelValue(img, width, height, depth,
                                                                                     i - leftPad, j - upPad,
                                                                                     k - frontPad,
                                                                                     padOption, padValue);
        }
      }
    }
    for (size_t k = frontPad; k < frontPad + depth; ++k) {
      for (size_t j = 0; j < upPad; ++j) {
        for (size_t i = 0; i < desWidth; ++i) {
          imgOut[k * desHeight * desWidth + j * desWidth + i] = getImage3DPixelValue(img, width, height, depth,
                                                                                     i - leftPad, j - upPad,
                                                                                     k - frontPad,
                                                                                     padOption, padValue);
        }
      }
      for (size_t j = upPad + height; j < desHeight; ++j) {
        for (size_t i = 0; i < desWidth; ++i) {
          imgOut[k * desHeight * desWidth + j * desWidth + i] = getImage3DPixelValue(img, width, height, depth,
                                                                                     i - leftPad, j - upPad,
                                                                                     k - frontPad,
                                                                                     padOption, padValue);
        }
      }
      for (size_t j = upPad; j < upPad + height; ++j) {
        for (size_t i = 0; i < leftPad; ++i) {
          imgOut[k * desHeight * desWidth + j * desWidth + i] = getImage3DPixelValue(img, width, height, depth,
                                                                                     i - leftPad, j - upPad,
                                                                                     k - frontPad,
                                                                                     padOption, padValue);
        }
        for (size_t i = leftPad + width; i < desWidth; ++i) {
          imgOut[k * desHeight * desWidth + j * desWidth + i] = getImage3DPixelValue(img, width, height, depth,
                                                                                     i - leftPad, j - upPad,
                                                                                     k - frontPad,
                                                                                     padOption, padValue);
        }
      }
    }
  }
}

template<typename TPixel>
void image3DFlip(TPixel* img, size_t width, size_t height, size_t depth, Dimension dim)
{
  if (dim == Dimension::X) {
    if (width <= 1)
      return;
    for (size_t d = 0; d < depth; ++d) {
      for (size_t i = 0; i < height; ++i) {
        TPixel* start = img + i * width + d * width * height;
        std::reverse(start, start + width);
      }
    }
  } else if (dim == Dimension::Y) {
    if (height <= 1)
      return;
    std::vector<TPixel> buffer(width);
    for (size_t d = 0; d < depth; ++d) {
      size_t j = 0;
      size_t k = height - 1;
      size_t size = sizeof(TPixel) * width;
      while (j < k) {
        memcpy(buffer.data(), img + j * width + d * width * height, size);
        memcpy(img + j * width + d * width * height, img + k * width + d * width * height, size);
        memcpy(img + k * width + d * width * height, buffer.data(), size);
        ++j;
        --k;
      }
    }
  } else if (dim == Dimension::Z) {
    if (depth <= 1)
      return;
    std::vector<TPixel> buffer(width * height);
    size_t j = 0;
    size_t k = depth - 1;
    size_t size = sizeof(TPixel) * width * height;
    while (j < k) {
      memcpy(buffer.data(), img + j * width * height, size);
      memcpy(img + j * width * height, img + k * width * height, size);
      memcpy(img + k * width * height, buffer.data(), size);
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
    : padImg(padImg)
    , padImgWidth(padImgWidth)
    , padImgHeight(padImgHeight)
    , kernel(kernel)
    , kernelWidth(kernelWidth)
    , kernelHeight(kernelHeight)
    , kernelDepth(kernelDepth)
    , imgOut(imgOut)
    , imgOutWidth(imgOutWidth)
    , imgOutHeight(imgOutHeight)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    for (size_t k = range.begin(); k != range.end(); ++k) {
#else
      void operator()(const std::pair<size_t,size_t> &range) const
      {
        for (size_t k=range.first; k<range.second; ++k) {
#endif
      for (size_t j = 0; j < imgOutHeight; ++j) {
        for (size_t i = 0; i < imgOutWidth; ++i) {
          double sum = 0.0;
          for (size_t s = 0; s < kernelDepth; ++s) { // plane by plane
            for (size_t r = 0; r < kernelHeight; ++r) {  // row by row
              const TPixel* imgStart = padImg + (j + r) * padImgWidth + i + (s + k) * padImgWidth * padImgHeight;
              sum = std::inner_product(imgStart, imgStart + kernelWidth,
                                       kernel + r * kernelWidth + s * kernelWidth * kernelHeight, sum);
            }
          }
          imgOut[j * imgOutWidth + i + k * imgOutWidth * imgOutHeight] = saturate_cast<TPixelOut>(sum);
        }
      }
    }
  }

  const TPixel* padImg;
  size_t padImgWidth;
  size_t padImgHeight;
  const double* kernel;
  size_t kernelWidth;
  size_t kernelHeight;
  size_t kernelDepth;
  TPixelOut* imgOut;
  size_t imgOutWidth;
  size_t imgOutHeight;
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
    : padImg(padImg)
    , padImgWidth(padImgWidth)
    , padImgHeight(padImgHeight)
    , kernel(kernel)
    , kernelWidth(kernelWidth)
    , kernelHeight(kernelHeight)
    , kernelDepth(kernelDepth)
    , imgOut(imgOut)
    , imgOutWidth(imgOutWidth)
    , imgOutHeight(imgOutHeight)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    if (kernelWidth < 8 || !(ZCpuInfo::instance().bAVX || ZCpuInfo::instance().bSSE3)) {
      for (size_t k = range.begin(); k != range.end(); ++k) {
#else
        void operator()(const std::pair<size_t,size_t> &range) const {
          if (kernelWidth < 8 || !(ZCpuInfoInstance.bAVX || ZCpuInfoInstance.bSSE3)) {
            for (size_t k=range.first; k<range.second; ++k) {
#endif
        for (size_t j = 0; j < imgOutHeight; ++j) {
          for (size_t i = 0; i < imgOutWidth; ++i) {
            double sum = 0.0;
            for (size_t s = 0; s < kernelDepth; ++s) { // plane by plane
              for (size_t r = 0; r < kernelHeight; ++r) {  // row by row
                const double* imgStart = padImg + (j + r) * padImgWidth + i + (s + k) * padImgWidth * padImgHeight;
                sum = std::inner_product(imgStart, imgStart + kernelWidth,
                                         kernel + r * kernelWidth + s * kernelWidth * kernelHeight, sum);
              }
            }
            imgOut[j * imgOutWidth + i + k * imgOutWidth * imgOutHeight] = sum;
          }
        }
      }
    } else if (ZCpuInfo::instance().bAVX) {
#ifndef _USE_QTCONCURRENT_
      Image3DFilterForOneBlock_AVX(padImg, padImgWidth, padImgHeight, kernel, kernelWidth, kernelHeight, kernelDepth,
                                   imgOut,
                                   imgOutWidth, imgOutHeight, range.begin(), range.end());
#else
      Image3DFilterForOneBlock_AVX(padImg, padImgWidth, padImgHeight, kernel, kernelWidth, kernelHeight, kernelDepth, imgOut,
                                   imgOutWidth, imgOutHeight, range.first, range.second);
#endif
    } else if (ZCpuInfo::instance().bSSE3) {
#ifndef _USE_QTCONCURRENT_
      Image3DFilterForOneBlock_SSE3(padImg, padImgWidth, padImgHeight, kernel, kernelWidth, kernelHeight, kernelDepth,
                                    imgOut,
                                    imgOutWidth, imgOutHeight, range.begin(), range.end());
#else
      Image3DFilterForOneBlock_SSE3(padImg, padImgWidth, padImgHeight, kernel, kernelWidth, kernelHeight, kernelDepth, imgOut,
                                    imgOutWidth, imgOutHeight, range.first, range.second);
#endif
    }
  }

  const double* padImg;
  size_t padImgWidth;
  size_t padImgHeight;
  const double* kernel;
  size_t kernelWidth;
  size_t kernelHeight;
  size_t kernelDepth;
  double* imgOut;
  size_t imgOutWidth;
  size_t imgOutHeight;
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
    : padImg(padImg)
    , padImgWidth(padImgWidth)
    , padImgHeight(padImgHeight)
    , kernel(kernel)
    , kernelWidth(kernelWidth)
    , imgOut(imgOut)
    , imgOutWidth(imgOutWidth)
    , imgOutHeight(imgOutHeight)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    for (size_t k = range.begin(); k != range.end(); ++k) {
#else
      void operator()(const std::pair<size_t,size_t> &range) const
      {
        for (size_t k=range.first; k<range.second; ++k) {
#endif
      for (size_t j = 0; j < imgOutHeight; ++j) {
        for (size_t i = 0; i < imgOutWidth; ++i) {
          double sum = 0.0;
          const TPixel* imgStart = padImg + j * padImgWidth + i + k * padImgWidth * padImgHeight;
          sum = std::inner_product(imgStart, imgStart + kernelWidth,
                                   kernel, sum);
          imgOut[j * imgOutWidth + i + k * imgOutWidth * imgOutHeight] = saturate_cast<TPixelOut>(sum);
        }
      }
    }
  }

  const TPixel* padImg;
  size_t padImgWidth;
  size_t padImgHeight;
  const double* kernel;
  size_t kernelWidth;
  TPixelOut* imgOut;
  size_t imgOutWidth;
  size_t imgOutHeight;
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
    : padImg(padImg)
    , padImgWidth(padImgWidth)
    , padImgHeight(padImgHeight)
    , kernel(kernel)
    , kernelWidth(kernelWidth)
    , imgOut(imgOut)
    , imgOutWidth(imgOutWidth)
    , imgOutHeight(imgOutHeight)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    if (kernelWidth < 8 || !(ZCpuInfo::instance().bAVX || ZCpuInfo::instance().bSSE3)) {
      for (size_t k = range.begin(); k != range.end(); ++k) {
#else
        void operator()(const std::pair<size_t,size_t> &range) const {
          if (kernelWidth < 8 || !(ZCpuInfoInstance.bAVX || ZCpuInfoInstance.bSSE3)) {
            for (size_t k=range.first; k<range.second; ++k) {
#endif
        for (size_t j = 0; j < imgOutHeight; ++j) {
          for (size_t i = 0; i < imgOutWidth; ++i) {
            double sum = 0.0;
            const double* imgStart = padImg + j * padImgWidth + i + k * padImgWidth * padImgHeight;
            sum = std::inner_product(imgStart, imgStart + kernelWidth,
                                     kernel, sum);
            imgOut[j * imgOutWidth + i + k * imgOutWidth * imgOutHeight] = sum;
          }
        }
      }
    } else if (ZCpuInfo::instance().bAVX) {
#ifndef _USE_QTCONCURRENT_
      Image3DRowFilterForOneBlock_AVX(padImg, padImgWidth, padImgHeight, kernel, kernelWidth,
                                      imgOut, imgOutWidth, imgOutHeight, range.begin(), range.end());
#else
      Image3DRowFilterForOneBlock_AVX(padImg, padImgWidth, padImgHeight, kernel, kernelWidth,
                                      imgOut, imgOutWidth, imgOutHeight, range.first, range.second);
#endif
    } else if (ZCpuInfo::instance().bSSE3) {
#ifndef _USE_QTCONCURRENT_
      Image3DRowFilterForOneBlock_SSE3(padImg, padImgWidth, padImgHeight, kernel, kernelWidth,
                                       imgOut, imgOutWidth, imgOutHeight, range.begin(), range.end());
#else
      Image3DRowFilterForOneBlock_SSE3(padImg, padImgWidth, padImgHeight, kernel, kernelWidth,
                                       imgOut, imgOutWidth, imgOutHeight, range.first, range.second);
#endif
    }
  }

  const double* padImg;
  size_t padImgWidth;
  size_t padImgHeight;
  const double* kernel;
  size_t kernelWidth;
  double* imgOut;
  size_t imgOutWidth;
  size_t imgOutHeight;
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
    : padImg(padImg)
    , padImgWidth(padImgWidth)
    , padImgHeight(padImgHeight)
    , kernel(kernel)
    , kernelHeight(kernelHeight)
    , imgOut(imgOut)
    , imgOutWidth(imgOutWidth)
    , imgOutHeight(imgOutHeight)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    for (size_t k = range.begin(); k != range.end(); ++k) {
#else
      void operator()(const std::pair<size_t,size_t> &range) const
      {
        for (size_t k=range.first; k<range.second; ++k) {
#endif
      for (size_t j = 0; j < imgOutHeight; ++j) {
        for (size_t i = 0; i < imgOutWidth; ++i) {
          double sum = 0.0;
          for (size_t r = 0; r < kernelHeight; ++r) {  // row by row
            sum += kernel[r] * (*(padImg + (j + r) * padImgWidth + i + k * padImgWidth * padImgHeight));
          }
          imgOut[j * imgOutWidth + i + k * imgOutWidth * imgOutHeight] = saturate_cast<TPixelOut>(sum);
        }
      }
    }
  }

  const TPixel* padImg;
  size_t padImgWidth;
  size_t padImgHeight;
  const double* kernel;
  size_t kernelHeight;
  TPixelOut* imgOut;
  size_t imgOutWidth;
  size_t imgOutHeight;
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
    : padImg(padImg)
    , padImgWidth(padImgWidth)
    , padImgHeight(padImgHeight)
    , kernel(kernel)
    , kernelDepth(kernelDepth)
    , imgOut(imgOut)
    , imgOutWidth(imgOutWidth)
    , imgOutHeight(imgOutHeight)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    for (size_t k = range.begin(); k != range.end(); ++k) {
#else
      void operator()(const std::pair<size_t,size_t> &range) const
      {
        for (size_t k=range.first; k<range.second; ++k) {
#endif
      for (size_t j = 0; j < imgOutHeight; ++j) {
        for (size_t i = 0; i < imgOutWidth; ++i) {
          double sum = 0.0;
          for (size_t p = 0; p < kernelDepth; ++p) {  // plane by plane
            sum += kernel[p] * (*(padImg + j * padImgWidth + i + (k + p) * padImgWidth * padImgHeight));
          }
          imgOut[j * imgOutWidth + i + k * imgOutWidth * imgOutHeight] = saturate_cast<TPixelOut>(sum);
        }
      }
    }
  }

  const TPixel* padImg;
  size_t padImgWidth;
  size_t padImgHeight;
  const double* kernel;
  size_t kernelDepth;
  TPixelOut* imgOut;
  size_t imgOutWidth;
  size_t imgOutHeight;
};

template<typename TPixel, typename TPixelOut>
void image3DFilter(const TPixel* img, size_t width, size_t height, size_t depth,
                   const double* kernel, size_t kernelWidth, size_t kernelHeight, size_t kernelDepth,
                   TPixelOut* imgOut,
                   PadOption boundaryOption = PadOption::Constant, TPixel boundaryValue = TPixel(0),
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
  std::vector<TPixel, boost::alignment::aligned_allocator<TPixel, 32>> padImg(desWidth * desHeight * desDepth);
  //ZBenchTimer bt;
  //bt.start();
  image3DPad(img, width, height, depth, leftPad, rightPad, upPad, downPad, frontPad, backPad, padImg.data(),
             boundaryOption, boundaryValue);
  //bt.stopAndPrint();

  //image3DWrite(padImg.data(), desWidth, desHeight, "/Users/feng/Downloads/padImg.tif");

  std::vector<double, boost::alignment::aligned_allocator<double, 32>> alignedKernel;
  alignedKernel.insert(alignedKernel.end(), kernel, kernel + kernelWidth * kernelHeight * kernelDepth);
  const double* adjKernel = alignedKernel.data();
  if (!corr) {
    image3DReflect(alignedKernel.data(), kernelWidth, kernelHeight, kernelDepth);
  }

  // get correlation of padImg and adjKernel
  Image3DFilterForOneBlock<TPixel, TPixelOut> functor(padImg.data(), desWidth, desHeight,
                                                      adjKernel, kernelWidth, kernelHeight, kernelDepth, imgOut, width,
                                                      height);
  if (!useMultithreading) {
#ifndef _USE_QTCONCURRENT_
    functor(tbb::blocked_range<size_t>(0, depth));
#else
    functor(std::make_pair(0, depth));
#endif
  } else {
#ifndef _USE_QTCONCURRENT_
    tbb::parallel_for(tbb::blocked_range<size_t>(0, depth), functor);
#else
    size_t numThreads = QThread::idealThreadCount();
    size_t numBlock = std::min(depth, numThreads * 2);
    size_t zPerBlock = depth / numBlock;
    QList<std::pair<size_t,size_t>> allRange;
    for (size_t i=0; i<numBlock; ++i) {
      allRange.push_back(std::make_pair(i*zPerBlock,
                                        (i==numBlock-1) ? depth : (i+1)*zPerBlock));
    }

    QtConcurrent::blockingMap(allRange, functor);
#endif
  }
}

template<typename TPixel, typename TPixelOut>
void image3DFilter(const TPixel* img, size_t width, size_t height, size_t depth,
                   const double* rowkernel, size_t kernelWidth,
                   const double* colkernel, size_t kernelHeight,
                   const double* zkernel, size_t kernelDepth,
                   TPixelOut* imgOut,
                   PadOption boundaryOption = PadOption::Constant, TPixel boundaryValue = TPixel(0),
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
  std::vector<TPixel, boost::alignment::aligned_allocator<TPixel, 32>> padImg(desWidth * desHeight * desDepth);
  //ZBenchTimer bt;
  //bt.start();
  image3DPad(img, width, height, depth, leftPad, rightPad, upPad, downPad, frontPad, backPad, padImg.data(),
             boundaryOption, boundaryValue);
  //bt.stopAndPrint();

  //image3DWrite(padImg.data(), desWidth, desHeight, "/Users/feng/Downloads/padImg.tif");

  std::vector<double, boost::alignment::aligned_allocator<double, 32>> alignedRowKernel;
  std::vector<double, boost::alignment::aligned_allocator<double, 32>> alignedColKernel;
  std::vector<double, boost::alignment::aligned_allocator<double, 32>> alignedZKernel;
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
    std::vector<double, boost::alignment::aligned_allocator<double, 32>> bufImg1(desWidth * height * desDepth);
    Image3DColFilterForOneBlock<TPixel, double> colfunctor(padImg.data(), desWidth, desHeight,
                                                           adjcolkernel, kernelHeight, bufImg1.data(), desWidth,
                                                           height);
#ifndef _USE_QTCONCURRENT_
    colfunctor(tbb::blocked_range<size_t>(0, desDepth));
#else
    colfunctor(std::make_pair(0, desDepth));
#endif
    padImg.clear();
    padImg.shrink_to_fit();

    std::vector<double, boost::alignment::aligned_allocator<double, 32>> bufImg2(width * height * desDepth);
    Image3DRowFilterForOneBlock<double, double> rowfunctor(bufImg1.data(), desWidth, height,
                                                           adjrowkernel, kernelWidth, bufImg2.data(), width, height);
#ifndef _USE_QTCONCURRENT_
    rowfunctor(tbb::blocked_range<size_t>(0, desDepth));
#else
    rowfunctor(std::make_pair(0, desDepth));
#endif
    bufImg1.clear();
    bufImg1.shrink_to_fit();

    Image3DZFilterForOneBlock<double, TPixelOut> zfunctor(bufImg2.data(), width, height,
                                                          adjzkernel, kernelDepth, imgOut, width, height);
#ifndef _USE_QTCONCURRENT_
    zfunctor(tbb::blocked_range<size_t>(0, depth));
#else
    zfunctor(std::make_pair(0, depth));
#endif
  } else {
#ifndef _USE_QTCONCURRENT_
    std::vector<double, boost::alignment::aligned_allocator<double, 32>> bufImg1(desWidth * height * desDepth);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, desDepth),
                      Image3DColFilterForOneBlock<TPixel, double>(padImg.data(), desWidth, desHeight,
                                                                  adjcolkernel, kernelHeight, bufImg1.data(), desWidth,
                                                                  height));
    padImg.clear();
    padImg.shrink_to_fit();

    std::vector<double, boost::alignment::aligned_allocator<double, 32>> bufImg2(width * height * desDepth);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, desDepth),
                      Image3DRowFilterForOneBlock<double, double>(bufImg1.data(), desWidth, height,
                                                                  adjrowkernel, kernelWidth, bufImg2.data(), width,
                                                                  height));
    bufImg1.clear();
    bufImg1.shrink_to_fit();

    tbb::parallel_for(tbb::blocked_range<size_t>(0, depth),
                      Image3DZFilterForOneBlock<double, TPixelOut>(bufImg2.data(), width, height,
                                                                   adjzkernel, kernelDepth, imgOut, width, height));
#else
    size_t numThreads = QThread::idealThreadCount();
    size_t numBlock = std::min(depth, numThreads * 2);
    size_t zPerBlock = depth / numBlock;
    QList<std::pair<size_t,size_t>> allRange;
    for (size_t i=0; i<numBlock; ++i) {
      allRange.push_back(std::make_pair(i*zPerBlock,
                                        (i==numBlock-1) ? depth : (i+1)*zPerBlock));
    }

    allRange[allRange.size()-1].second = desDepth;
    std::vector<double, boost::alignment::aligned_allocator<double, 32>> bufImg1(desWidth*height*desDepth);
    QtConcurrent::blockingMap(allRange,
                              Image3DColFilterForOneBlock<TPixel,double>(padImg.data(), desWidth, desHeight,
                              adjcolkernel, kernelHeight, bufImg1.data(), desWidth, height));
    padImg.clear();
    padImg.shrink_to_fit();

    std::vector<double, boost::alignment::aligned_allocator<double, 32>> bufImg2(width*height*desDepth);
    QtConcurrent::blockingMap(allRange,
                              Image3DRowFilterForOneBlock<double,double>(bufImg1.data(), desWidth, height,
                              adjrowkernel, kernelWidth, bufImg2.data(), width, height));
    bufImg1.clear();
    bufImg1.shrink_to_fit();

    allRange[allRange.size()-1].second = depth;
    QtConcurrent::blockingMap(allRange,
                              Image3DZFilterForOneBlock<double,TPixelOut>(bufImg2.data(), width, height,
                              adjzkernel, kernelDepth, imgOut, width, height));
#endif
  }
}

// boundaryOption see image2DFilter
template<typename TPixel, typename TPixelOut>
void image3DGaussianFilter(const TPixel* img, size_t width, size_t height, size_t depth,
                           double kernelSigmaX, double kernelSigmaY, double kernelSigmaZ,
                           TPixelOut* imgOut,
                           int kernelWidth = -1, int kernelHeight = -1, int kernelDepth = -1,
                           PadOption boundaryOption = PadOption::Constant, TPixel boundaryValue = TPixel(0),
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

  //ZBenchTimer bt;
  //bt.start();
  image3DFilter(img, width, height, depth, rowkernel.data(), kWidth,
                colkernel.data(), kHeight, zkernel.data(), kDepth,
                imgOut, boundaryOption, boundaryValue, true, useMultithreading);
  //bt.stopAndPrint();
#endif
}

// note: LoG filter will produce negative value, better use a out type that can represent negative pixel
// such as double
// boundaryOption see image2DFilter
template<typename TPixel, typename TPixelOut>
void image3DLoGFilter(const TPixel* img, size_t width, size_t height, size_t depth,
                      double kernelSigmaX, double kernelSigmaY, double kernelSigmaZ,
                      TPixelOut* imgOut,
                      int kernelWidth = -1, int kernelHeight = -1, int kernelDepth = -1,
                      PadOption boundaryOption = PadOption::Constant, TPixel boundaryValue = TPixel(0),
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

  //ZBenchTimer bt;
  //bt.start();
  image3DFilter(img, width, height, depth, rowLoGkernel.data(), kWidth,
                colGkernel.data(), kHeight, zGkernel.data(), kDepth,
                imgOut, boundaryOption, boundaryValue, true, useMultithreading);

  std::vector<TPixelOut> bufImg(width * height * depth);

  image3DFilter(img, width, height, depth, rowGkernel.data(), kWidth,
                colLoGkernel.data(), kHeight, zGkernel.data(), kDepth,
                bufImg.data(), boundaryOption, boundaryValue, true, useMultithreading);

  for (size_t i = 0; i < bufImg.size(); ++i)
    imgOut[i] += bufImg[i];

  image3DFilter(img, width, height, depth, rowGkernel.data(), kWidth,
                colGkernel.data(), kHeight, zLoGkernel.data(), kDepth,
                bufImg.data(), boundaryOption, boundaryValue, true, useMultithreading);

  for (size_t i = 0; i < bufImg.size(); ++i)
    imgOut[i] += bufImg[i];

  //bt.stopAndPrint();
}

template<typename TPixel, typename TPixelOut>
struct Resize3DForOneBlock
{
  Resize3DForOneBlock(const TPixel* img, size_t width, size_t height, size_t depth,
                      TPixelOut* imgOut, size_t outWidth, size_t outHeight, size_t outDepth,
                      const std::vector<double>& xWeights, const std::vector<size_t>& xIndices, size_t xKernelWidth,
                      const std::vector<double>& yWeights, const std::vector<size_t>& yIndices, size_t yKernelWidth,
                      const std::vector<double>& zWeights, const std::vector<size_t>& zIndices, size_t zKernelWidth)
    : m_img(img), m_width(width), m_height(height), m_depth(depth)
    , m_imgOut(imgOut), m_outWidth(outWidth), m_outHeight(outHeight), m_outDepth(outDepth)
    , m_xWeights(xWeights), m_xIndices(xIndices), m_xKernelWidth(xKernelWidth)
    , m_yWeights(yWeights), m_yIndices(yIndices), m_yKernelWidth(yKernelWidth)
    , m_zWeights(zWeights), m_zIndices(zIndices), m_zKernelWidth(zKernelWidth)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_

  void operator()(const tbb::blocked_range<size_t>& range) const
#else
  void operator()(const std::pair<size_t,size_t> &range) const
#endif
  {
    if (m_xKernelWidth == 1 && m_yKernelWidth == 1 && m_zKernelWidth == 1) {
#ifndef _USE_QTCONCURRENT_
      for (size_t z = range.begin(); z != range.end(); ++z) {
#else
        for (size_t z=range.first; z<range.second; ++z) {
#endif
        for (size_t y = 0; y < m_outHeight; ++y) {
          for (size_t x = 0; x < m_outWidth; ++x) {
            m_imgOut[z * m_outWidth * m_outHeight + y * m_outWidth + x] = saturate_cast<TPixelOut>(
              m_img[m_zIndices[z] * m_width * m_height + m_yIndices[y] * m_width + m_xIndices[x]]);
          }
        }
      }
    } else {
#ifndef _USE_QTCONCURRENT_
      for (size_t z = range.begin(); z != range.end(); ++z) {
#else
        for (size_t z=range.first; z<range.second; ++z) {
#endif
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
  const std::vector<double>& m_yWeights;
  const std::vector<size_t>& m_yIndices;
  size_t m_yKernelWidth;
  const std::vector<double>& m_zWeights;
  const std::vector<size_t>& m_zIndices;
  size_t m_zKernelWidth;
};

//((scale-1)/2) in output image maps to 0 in input image, and ((3*scale-1)/2) in output
//image maps to 1 in input image.
// 'antialiasing' specifies whether to perform antialiasing when shrinking an image. For the 'nearest' method,
// the parameter 'antialiasingForNearest' is used (default false); for all other methods, the default is true.
template<typename TPixel, typename TPixelOut>
void image3DResize(const TPixel* img, size_t width, size_t height, size_t depth,
                   TPixelOut* imgOut, size_t outWidth, size_t outHeight, size_t outDepth,
                   Interpolant interpolant = Interpolant::Cubic, bool antialiasing = true,
                   bool antialiasingForNearest = false,
                   bool useMultithreading = true)
{
  std::vector<double> xWeights;
  std::vector<size_t> xIndices;
  size_t xKernelWidth;
  std::vector<double> yWeights;
  std::vector<size_t> yIndices;
  size_t yKernelWidth;
  std::vector<double> zWeights;
  std::vector<size_t> zIndices;
  size_t zKernelWidth;
  _resizeContributions(width, outWidth, interpolant,
                       interpolant == Interpolant::Nearest ? antialiasingForNearest : antialiasing,
                       xWeights, xIndices, xKernelWidth);
  _resizeContributions(height, outHeight, interpolant,
                       interpolant == Interpolant::Nearest ? antialiasingForNearest : antialiasing,
                       yWeights, yIndices, yKernelWidth);
  _resizeContributions(depth, outDepth, interpolant,
                       interpolant == Interpolant::Nearest ? antialiasingForNearest : antialiasing,
                       zWeights, zIndices, zKernelWidth);

  Resize3DForOneBlock<TPixel, TPixelOut> func(img, width, height, depth, imgOut, outWidth, outHeight, outDepth,
                                              xWeights, xIndices, xKernelWidth,
                                              yWeights, yIndices, yKernelWidth,
                                              zWeights, zIndices, zKernelWidth);
  if (!useMultithreading) {
#ifndef _USE_QTCONCURRENT_
    func(tbb::blocked_range<size_t>(0, outDepth));
#else
    func(std::pair<size_t,size_t>(0, outDepth));
#endif
  } else {
#ifndef _USE_QTCONCURRENT_
    tbb::parallel_for(tbb::blocked_range<size_t>(0, outDepth), func);
#else
    size_t numThreads = QThread::idealThreadCount();
    size_t numBlock = std::min(outDepth, numThreads * 2);
    size_t pagesPerBlock = outDepth / numBlock;
    QList<std::pair<size_t,size_t>> allRange;
    for (size_t i=0; i<numBlock; ++i) {
      allRange.push_back(std::make_pair(i*pagesPerBlock,
                                        (i==numBlock-1) ? outDepth : ((i+1)*pagesPerBlock)));
    }

    QtConcurrent::blockingMap(allRange, func);
#endif
  }
}

} // namespace nim
