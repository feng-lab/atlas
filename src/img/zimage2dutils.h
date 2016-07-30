#ifndef ZIMAGE2DUTILS_H
#define ZIMAGE2DUTILS_H

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
#include <cassert>
#include <boost/align/aligned_allocator.hpp>
#include "zcpuinfo.h"
#include "zimagesse3.h"
#include "zimageavx.h"
#include "zimagefilterkernel.h"
#include "zimginterface.h"
#include "zsaturateoperation.h"

namespace nim {

// everything is row major

bool seperate2DKernel(const double* kernel, size_t width, size_t height,
                      double* rowKernel, double* colKernel);

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
      if ((coord[i] / imgSize[i]) % 2 == 0)
        coord[i] = coord[i] % imgSize[i];
      else
        coord[i] = imgSize[i] - 1 - coord[i] % imgSize[i];
    }
  } else if (padOption == PadOption::Replicate) {
    for (size_t i = 0; i < numDimensions; ++i) {
      if (coord[i] < 0) {
        coord[i] = 0;
      }
      if (coord[i] > static_cast<int>(imgSize[i]) - 1)
        coord[i] = imgSize[i] - 1;
    }
  } else if (padOption == PadOption::Circular) {
    for (size_t i = 0; i < numDimensions; ++i) {
      if (coord[i] >= 0)
        coord[i] = coord[i] % imgSize[i];
      else
        coord[i] += ((-coord[i] - 1) / imgSize[i] + 1) * imgSize[i];
    }
  }
}

template<>
void wrapCoordToImage<size_t>(size_t* coord, const size_t* imgSize, size_t numDimensions, PadOption padOption);

template<typename SignedIntegerType, typename TPixel>
TPixel getImage2DPixelValue(const TPixel* img, size_t width, size_t height,
                            SignedIntegerType x, SignedIntegerType y,
                            PadOption padOption = PadOption::Constant, TPixel padValue = TPixel(0))
{
  if (x >= 0 && x < static_cast<SignedIntegerType>(width) &&
      y >= 0 && y < static_cast<SignedIntegerType>(height))
    return img[y * width + x];

  if (padOption == PadOption::Constant) {
    return padValue;
  } else {
    SignedIntegerType coord[2];
    coord[0] = x;
    coord[1] = y;
    size_t imgSize[2];
    imgSize[0] = width;
    imgSize[1] = height;
    wrapCoordToImage(coord, imgSize, 2, padOption);
    return img[coord[1] * width + coord[0]];
  }
}

// imgOut should be preallocated and not same as img
template<typename TPixel>
void image2DPad(const TPixel* img, size_t width, size_t height,
                size_t leftPad, size_t rightPad, size_t upPad, size_t downPad,
                TPixel* imgOut, PadOption padOption = PadOption::Constant, TPixel padValue = TPixel(0))
{
  DCHECK_NE(img, imgOut);
  size_t size = sizeof(TPixel) * width * height;
  if (leftPad == 0 && rightPad == 0 && upPad == 0 && downPad == 0) {
    memcpy(imgOut, img, size);
  } else {
    size_t desWidth = leftPad + width + rightPad;
    size_t desHeight = upPad + height + downPad;

    // boundary
    if (padOption == PadOption::Constant) {
      std::fill(imgOut, imgOut + desWidth * desHeight, static_cast<TPixel>(padValue));
    } else if (padOption == PadOption::Symmetric) {
      // corner
      for (size_t j = 0; j < upPad; ++j) {
        size_t refY = upPad - j - 1;
        if ((refY / height) % 2 == 1)
          refY = height - 1 - refY % height;
        else
          refY = refY % height;
        for (size_t i = 0; i < leftPad; ++i) {
          size_t refX = leftPad - i - 1;
          if ((refX / width) % 2 == 1)
            refX = width - 1 - refX % width;
          else
            refX = refX % width;
          imgOut[j * desWidth + i] = img[refY * width + refX];
        }
      }
      for (size_t j = 0; j < upPad; ++j) {
        size_t refY = upPad - j - 1;
        if ((refY / height) % 2 == 1)
          refY = height - 1 - refY % height;
        else
          refY = refY % height;
        for (size_t i = desWidth - rightPad; i < desWidth; ++i) {
          size_t refX = width - (i - desWidth + rightPad) - 1;
          if ((refX / width) % 2 == 1)
            refX = width - 1 - refX % width;
          else
            refX = refX % width;
          imgOut[j * desWidth + i] = img[refY * width + refX];
        }
      }
      for (size_t j = desHeight - downPad; j < desHeight; ++j) {
        size_t refY = height - (j - desHeight + downPad) - 1;
        if ((refY / height) % 2 == 1)
          refY = height - 1 - refY % height;
        else
          refY = refY % height;
        for (size_t i = 0; i < leftPad; ++i) {
          size_t refX = leftPad - i - 1;
          if ((refX / width) % 2 == 1)
            refX = width - 1 - refX % width;
          else
            refX = refX % width;
          imgOut[j * desWidth + i] = img[refY * width + refX];
        }
      }
      for (size_t j = desHeight - downPad; j < desHeight; ++j) {
        size_t refY = height - (j - desHeight + downPad) - 1;
        if ((refY / height) % 2 == 1)
          refY = height - 1 - refY % height;
        else
          refY = refY % height;
        for (size_t i = desWidth - rightPad; i < desWidth; ++i) {
          size_t refX = width - (i - desWidth + rightPad) - 1;
          if ((refX / width) % 2 == 1)
            refX = width - 1 - refX % width;
          else
            refX = refX % width;
          imgOut[j * desWidth + i] = img[refY * width + refX];
        }
      }
      // left
      for (size_t i = 0; i < leftPad; ++i) {
        size_t refX = leftPad - i - 1;
        if ((refX / width) % 2 == 1)
          refX = width - 1 - refX % width;
        else
          refX = refX % width;
        for (size_t j = 0; j < height; ++j) {
          imgOut[(j + upPad) * desWidth + i] = img[j * width + refX];
        }
      }
      // right
      for (size_t i = desWidth - rightPad; i < desWidth; ++i) {
        size_t refX = width - (i - desWidth + rightPad) - 1;
        if ((refX / width) % 2 == 1)
          refX = width - 1 - refX % width;
        else
          refX = refX % width;
        for (size_t j = 0; j < height; ++j) {
          imgOut[(j + upPad) * desWidth + i] = img[j * width + refX];
        }
      }
      // up
      for (size_t j = 0; j < upPad; ++j) {
        size_t refY = upPad - j - 1;
        if ((refY / height) % 2 == 1)
          refY = height - 1 - refY % height;
        else
          refY = refY % height;
        memcpy(imgOut + j * desWidth + leftPad,
               img + refY * width,
               sizeof(TPixel) * width);
      }
      // down
      for (size_t j = desHeight - downPad; j < desHeight; ++j) {
        size_t refY = height - (j - desHeight + downPad) - 1;
        if ((refY / height) % 2 == 1)
          refY = height - 1 - refY % height;
        else
          refY = refY % height;
        memcpy(imgOut + j * desWidth + leftPad,
               img + refY * width,
               sizeof(TPixel) * width);
      }

    }

    // copy image
    if (leftPad == 0 && rightPad == 0) {
      TPixel* desStart = imgOut + upPad * width;
      memcpy(desStart, img, size);
    } else {
      TPixel* desStart = imgOut + upPad * desWidth + leftPad;
      for (size_t i = 0; i < height; ++i) {
        memcpy(desStart, img + i * width, sizeof(TPixel) * width);
        desStart += desWidth;
      }
    }

    if (padOption == PadOption::Replicate) { //replicate
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
        memcpy(imgOut + j * desWidth,
               imgOut + upPad * desWidth,
               sizeof(TPixel) * desWidth);
      }
      // down
      for (size_t j = desHeight - downPad; j < desHeight; ++j) {
        memcpy(imgOut + j * desWidth,
               imgOut + (desHeight - 1 - downPad) * desWidth,
               sizeof(TPixel) * desWidth);
      }
    } else if (padOption == PadOption::Circular) { //circular
      // left
      int refX = width;
      for (int i = leftPad - 1; i >= 0; --i) {
        --refX;
        refX = refX < 0 ? width - 1 : refX;
        for (size_t j = 0; j < height; ++j) {
          imgOut[(j + upPad) * desWidth + i] = img[j * width + refX];
        }
      }
      // right
      refX = -1;
      for (size_t i = desWidth - rightPad; i < desWidth; ++i) {
        ++refX;
        refX = refX >= static_cast<int>(width) ? 0 : refX;
        for (size_t j = 0; j < height; ++j) {
          imgOut[(j + upPad) * desWidth + i] = img[j * width + refX];
        }
      }
      // up
      int refY = height;
      for (int j = upPad - 1; j >= 0; --j) {
        --refY;
        refY = refY < 0 ? height - 1 : refY;
        memcpy(imgOut + j * desWidth,
               imgOut + (refY + upPad) * desWidth,
               sizeof(TPixel) * desWidth);
      }
      // down
      refY = -1;
      for (int j = desHeight - downPad; j < static_cast<int>(desHeight); ++j) {
        ++refY;
        refY = refY >= static_cast<int>(height) ? 0 : refY;
        memcpy(imgOut + j * desWidth,
               imgOut + (refY + upPad) * desWidth,
               sizeof(TPixel) * desWidth);
      }
    }
  }
}

template<typename TPixel>
void image2DFlip(TPixel* img, size_t width, size_t height, Dimension dim)
{
  if (dim == Dimension::X) {
    if (width <= 1)
      return;
    for (size_t i = 0; i < height; ++i) {
      size_t j = i * width;
      size_t k = j + width - 1;
      while (j < k) {
        std::swap(img[j], img[k]);
        ++j;
        --k;
      }
    }
  } else if (dim == Dimension::Y) {
    if (height <= 1)
      return;
    std::vector<TPixel> buffer(width);
    size_t j = 0;
    size_t k = height - 1;
    size_t size = sizeof(TPixel) * width;
    while (j < k) {
      memcpy(buffer.data(), img + j * width, size);
      memcpy(img + j * width, img + k * width, size);
      memcpy(img + k * width, buffer.data(), size);
      ++j;
      --k;
    }
  }
}

// same as flip all dimensions
template<typename TPixel>
void image2DReflect(TPixel* img, size_t width, size_t height)
{
  size_t length = width * height;
  size_t j = length - 1;
  for (size_t i = 0; i < length / 2; ++i, --j) {
    std::swap(img[i], img[j]);
  }
}

template<typename TPixel>
void image2DTranspose(TPixel* img, size_t width, size_t height)
{
  std::vector<TPixel> buf(width * height);
  for (size_t i = 0; i < height; i++)
    for (size_t j = 0; j < width; j++)
      buf[i + j * height] = img[j + i * width];
  memcpy(img, buf.data(), sizeof(TPixel) * width * height);
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
    : padImg(padImg)
    , padImgWidth(padImgWidth)
    , kernel(kernel)
    , kernelWidth(kernelWidth)
    , kernelHeight(kernelHeight)
    , imgOut(imgOut)
    , imgOutWidth(imgOutWidth)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    for (size_t j = range.begin(); j != range.end(); ++j) {
#else
      void operator()(const std::pair<size_t,size_t> &range) const {
        for (size_t j=range.first; j<range.second; ++j) {
#endif
      for (size_t i = 0; i < imgOutWidth; ++i) {
        double sum = 0.0;
        for (size_t r = 0; r < kernelHeight; ++r) {  // row by row
          const TPixel* imgStart = padImg + (j + r) * padImgWidth + i;
          sum = std::inner_product(imgStart, imgStart + kernelWidth,
                                   kernel + r * kernelWidth, sum);
        }
        imgOut[j * imgOutWidth + i] = saturate_cast<TPixelOut>(sum);
      }
    }
  }

  const TPixel* padImg;
  size_t padImgWidth;
  const double* kernel;
  size_t kernelWidth;
  size_t kernelHeight;
  TPixelOut* imgOut;
  size_t imgOutWidth;
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
    : padImg(padImg)
    , padImgWidth(padImgWidth)
    , kernel(kernel)
    , kernelWidth(kernelWidth)
    , kernelHeight(kernelHeight)
    , imgOut(imgOut)
    , imgOutWidth(imgOutWidth)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    if (kernelWidth < 8 || !(ZCpuInfoInstance.bAVX || ZCpuInfoInstance.bSSE3)) {
      for (size_t j = range.begin(); j != range.end(); ++j) {
#else
        void operator()(const std::pair<size_t,size_t> &range) const {
          if (kernelWidth < 8 || !(ZCpuInfoInstance.bAVX || ZCpuInfoInstance.bSSE3)) {
            for (size_t j=range.first; j<range.second; ++j) {
#endif
        for (size_t i = 0; i < imgOutWidth; ++i) {
          double sum = 0.0;
          for (size_t r = 0; r < kernelHeight; ++r) {  // row by row
            const double* imgStart = padImg + (j + r) * padImgWidth + i;
            sum = std::inner_product(imgStart, imgStart + kernelWidth,
                                     kernel + r * kernelWidth, sum);
          }
          imgOut[j * imgOutWidth + i] = sum;
        }
      }
    } else if (ZCpuInfoInstance.bAVX) {
#ifndef _USE_QTCONCURRENT_
      Image2DFilterForOneBlock_AVX(padImg, padImgWidth, kernel, kernelWidth, kernelHeight,
                                   imgOut, imgOutWidth, range.begin(), range.end());
#else
      Image2DFilterForOneBlock_AVX(padImg, padImgWidth, kernel, kernelWidth, kernelHeight,
                                   imgOut, imgOutWidth, range.first, range.second);
#endif
    } else if (ZCpuInfoInstance.bSSE3) {
#ifndef _USE_QTCONCURRENT_
      Image2DFilterForOneBlock_SSE3(padImg, padImgWidth, kernel, kernelWidth, kernelHeight,
                                    imgOut, imgOutWidth, range.begin(), range.end());
#else
      Image2DFilterForOneBlock_SSE3(padImg, padImgWidth, kernel, kernelWidth, kernelHeight,
                                    imgOut, imgOutWidth, range.first, range.second);
#endif
    }
  }

  const double* padImg;
  size_t padImgWidth;
  const double* kernel;
  size_t kernelWidth;
  size_t kernelHeight;
  double* imgOut;
  size_t imgOutWidth;
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
    : padImg(padImg)
    , padImgWidth(padImgWidth)
    , kernel(kernel)
    , kernelWidth(kernelWidth)
    , imgOut(imgOut)
    , imgOutWidth(imgOutWidth)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    for (size_t j = range.begin(); j != range.end(); ++j) {
#else
      void operator()(const std::pair<size_t,size_t> &range) const {
        for (size_t j=range.first; j<range.second; ++j) {
#endif
      for (size_t i = 0; i < imgOutWidth; ++i) {
        double sum = 0.0;
        const TPixel* imgStart = padImg + j * padImgWidth + i;
        sum = std::inner_product(imgStart, imgStart + kernelWidth,
                                 kernel, sum);
        imgOut[j * imgOutWidth + i] = saturate_cast<TPixelOut>(sum);
      }
    }
  }

  const TPixel* padImg;
  size_t padImgWidth;
  const double* kernel;
  size_t kernelWidth;
  TPixelOut* imgOut;
  size_t imgOutWidth;
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
    : padImg(padImg)
    , padImgWidth(padImgWidth)
    , kernel(kernel)
    , kernelWidth(kernelWidth)
    , imgOut(imgOut)
    , imgOutWidth(imgOutWidth)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    if (kernelWidth < 8 || !(ZCpuInfoInstance.bAVX || ZCpuInfoInstance.bSSE3)) {
      for (size_t j = range.begin(); j != range.end(); ++j) {
#else
        void operator()(const std::pair<size_t,size_t> &range) const {
          if (kernelWidth < 8 || !(ZCpuInfoInstance.bAVX || ZCpuInfoInstance.bSSE3)) {
            for (size_t j=range.first; j<range.second; ++j) {
#endif
        for (size_t i = 0; i < imgOutWidth; ++i) {
          double sum = 0.0;
          const double* imgStart = padImg + j * padImgWidth + i;
          sum = std::inner_product(imgStart, imgStart + kernelWidth,
                                   kernel, sum);
          imgOut[j * imgOutWidth + i] = sum;
        }
      }
    } else if (ZCpuInfoInstance.bAVX) {
#ifndef _USE_QTCONCURRENT_
      Image2DRowFilterForOneBlock_AVX(padImg, padImgWidth, kernel, kernelWidth, imgOut, imgOutWidth,
                                      range.begin(), range.end());
#else
      Image2DRowFilterForOneBlock_AVX(padImg, padImgWidth, kernel, kernelWidth, imgOut, imgOutWidth,
                                      range.first ,range.second);
#endif
    } else if (ZCpuInfoInstance.bSSE3) {
#ifndef _USE_QTCONCURRENT_
      Image2DRowFilterForOneBlock_SSE3(padImg, padImgWidth, kernel, kernelWidth, imgOut, imgOutWidth,
                                       range.begin(), range.end());
#else
      Image2DRowFilterForOneBlock_SSE3(padImg, padImgWidth, kernel, kernelWidth, imgOut, imgOutWidth,
                                       range.first ,range.second);
#endif
    }
  }

  const double* padImg;
  size_t padImgWidth;
  const double* kernel;
  size_t kernelWidth;
  double* imgOut;
  size_t imgOutWidth;
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
    : padImg(padImg)
    , padImgWidth(padImgWidth)
    , kernel(kernel)
    , kernelHeight(kernelHeight)
    , imgOut(imgOut)
    , imgOutWidth(imgOutWidth)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_

  void operator()(const tbb::blocked_range<size_t>& range) const
  {
    for (size_t j = range.begin(); j != range.end(); ++j) {
#else
      void operator()(const std::pair<size_t,size_t> &range) const {
        for (size_t j=range.first; j<range.second; ++j) {
#endif
      for (size_t i = 0; i < imgOutWidth; ++i) {
        double sum = 0.0;
        for (size_t r = 0; r < kernelHeight; ++r) {  // row by row
          sum += kernel[r] * (*(padImg + (j + r) * padImgWidth + i));
        }
        imgOut[j * imgOutWidth + i] = saturate_cast<TPixelOut>(sum);
      }
    }
  }

  const TPixel* padImg;
  size_t padImgWidth;
  const double* kernel;
  size_t kernelHeight;
  TPixelOut* imgOut;
  size_t imgOutWidth;
};

template<typename TPixel, typename TPixelOut>
void image2DFilter(const TPixel* img, size_t width, size_t height,
                   const double* kernel, size_t kernelWidth, size_t kernelHeight,
                   TPixelOut* imgOut,
                   PadOption boundaryOption = PadOption::Constant, TPixel boundaryValue = TPixel(0),
                   bool corr = true,
                   bool useMultithreading = true)
{
  size_t leftPad = kernelWidth / 2;
  size_t rightPad = (kernelWidth - 1) / 2;
  size_t upPad = kernelHeight / 2;
  size_t downPad = (kernelHeight - 1) / 2;
  size_t desWidth = leftPad + width + rightPad;
  size_t desHeight = upPad + height + downPad;
  std::vector<TPixel, boost::alignment::aligned_allocator<TPixel, 32>> padImg(desWidth * desHeight);
  //ZBenchTimer bt;
  //bt.start();
  image2DPad(img, width, height, leftPad, rightPad, upPad, downPad, padImg.data(),
             boundaryOption, boundaryValue);
  //bt.stopAndPrint();

  //image2DWrite(padImg.data(), desWidth, desHeight, "/Users/feng/Downloads/padImg.tif");

  std::vector<double, boost::alignment::aligned_allocator<double, 32>> alignedKernel;
  alignedKernel.insert(alignedKernel.end(), kernel, kernel + kernelWidth * kernelHeight);
  if (!corr) {
    image2DReflect(alignedKernel.data(), kernelWidth, kernelHeight);
  }

  std::vector<double, boost::alignment::aligned_allocator<double, 32>> rowKernel(kernelWidth);
  std::vector<double, boost::alignment::aligned_allocator<double, 32>> colKernel(kernelHeight);
  if (seperate2DKernel(alignedKernel.data(), kernelWidth, kernelHeight,
                       rowKernel.data(), colKernel.data())) {
    std::vector<double, boost::alignment::aligned_allocator<double, 32>> bufImg(width * desHeight);

    Image2DRowFilterForOneBlock<TPixel, double> rowfunctor(padImg.data(), desWidth,
                                                           rowKernel.data(), kernelWidth, bufImg.data(), width);
    Image2DColFilterForOneBlock<double, TPixelOut> colfunctor(bufImg.data(), width,
                                                              colKernel.data(), kernelHeight, imgOut, width);

    // get correlation of padImg and adjKernel
    if (!useMultithreading) {
#ifndef _USE_QTCONCURRENT_
      rowfunctor(tbb::blocked_range<size_t>(0, desHeight));
      colfunctor(tbb::blocked_range<size_t>(0, height));
#else
      rowfunctor(std::make_pair(0, desHeight));
      colfunctor(std::make_pair(0, height));
#endif
    } else {
#ifndef _USE_QTCONCURRENT_
      tbb::parallel_for(tbb::blocked_range<size_t>(0, desHeight), rowfunctor);
      tbb::parallel_for(tbb::blocked_range<size_t>(0, height), colfunctor);
#else
      size_t numThreads = QThread::idealThreadCount();
      size_t numBlock = std::min(height, numThreads * 2);
      size_t rowsPerBlock = height / numBlock;
      QList<std::pair<size_t,size_t>> allRange;
      for (size_t i=0; i<numBlock; ++i) {
        allRange.push_back(std::make_pair(i*rowsPerBlock,
                                          (i==numBlock-1) ? height : (i+1)*rowsPerBlock));
      }

      allRange[allRange.size()-1].second = desHeight;
      QtConcurrent::blockingMap(allRange, rowfunctor);

      allRange[allRange.size()-1].second = height;
      QtConcurrent::blockingMap(allRange, colfunctor);
#endif
    }
  } else {
    // get correlation of padImg and adjKernel
    Image2DFilterForOneBlock<TPixel, TPixelOut> functor(padImg.data(), desWidth,
                                                        alignedKernel.data(), kernelWidth, kernelHeight, imgOut, width);
    if (!useMultithreading) {
#ifndef _USE_QTCONCURRENT_
      functor(tbb::blocked_range<size_t>(0, height));
#else
      functor(std::make_pair(0, height));
#endif
    } else {
#ifndef _USE_QTCONCURRENT_
      tbb::parallel_for(tbb::blocked_range<size_t>(0, height), functor);
#else
      size_t numThreads = QThread::idealThreadCount();
      size_t numBlock = std::min(height, numThreads * 2);
      size_t rowsPerBlock = height / numBlock;
      QList<std::pair<size_t,size_t>> allRange;
      for (size_t i=0; i<numBlock; ++i) {
        allRange.push_back(std::make_pair(i*rowsPerBlock,
                                          (i==numBlock-1) ? height : (i+1)*rowsPerBlock));
      }

      QtConcurrent::blockingMap(allRange, functor);
#endif
    }
  }
}

template<typename TPixel, typename TPixelOut>
void image2DFilter(const TPixel* img, size_t width, size_t height,
                   const double* rowkernel, size_t kernelWidth,
                   const double* colkernel, size_t kernelHeight,
                   TPixelOut* imgOut,
                   PadOption boundaryOption = PadOption::Constant, TPixel boundaryValue = TPixel(0),
                   bool corr = true,
                   bool useMultithreading = true)
{
  size_t leftPad = kernelWidth / 2;
  size_t rightPad = (kernelWidth - 1) / 2;
  size_t upPad = kernelHeight / 2;
  size_t downPad = (kernelHeight - 1) / 2;
  size_t desWidth = leftPad + width + rightPad;
  size_t desHeight = upPad + height + downPad;
  std::vector<TPixel, boost::alignment::aligned_allocator<TPixel, 32>> padImg(desWidth * desHeight);
  //ZBenchTimer bt;
  //bt.start();
  image2DPad(img, width, height, leftPad, rightPad, upPad, downPad, padImg.data(),
             boundaryOption, boundaryValue);
  //bt.stopAndPrint();

  //image2DWrite(padImg.data(), desWidth, desHeight, "/Users/feng/Downloads/padImg.tif");

  std::vector<double, boost::alignment::aligned_allocator<double, 32>> alignedRowKernel;
  std::vector<double, boost::alignment::aligned_allocator<double, 32>> alignedColKernel;
  alignedRowKernel.insert(alignedRowKernel.end(), rowkernel, rowkernel + kernelWidth);
  alignedColKernel.insert(alignedColKernel.end(), colkernel, colkernel + kernelHeight);
  if (!corr) {
    image2DFlip(alignedRowKernel.data(), kernelWidth, 1, Dimension::X);
    image2DFlip(alignedColKernel.data(), kernelHeight, 1, Dimension::X);
  }

  std::vector<double, boost::alignment::aligned_allocator<double, 32>> bufImg(width * desHeight);

  Image2DRowFilterForOneBlock<TPixel, double> rowfunctor(padImg.data(), desWidth,
                                                         alignedRowKernel.data(), kernelWidth, bufImg.data(), width);
  Image2DColFilterForOneBlock<double, TPixelOut> colfunctor(bufImg.data(), width,
                                                            alignedColKernel.data(), kernelHeight, imgOut, width);

  // get correlation of padImg and adjKernel
  if (!useMultithreading) {
#ifndef _USE_QTCONCURRENT_
    rowfunctor(tbb::blocked_range<size_t>(0, desHeight));
    colfunctor(tbb::blocked_range<size_t>(0, height));
#else
    rowfunctor(std::make_pair(0, desHeight));
    colfunctor(std::make_pair(0, height));
#endif
  } else {
#ifndef _USE_QTCONCURRENT_
    tbb::parallel_for(tbb::blocked_range<size_t>(0, desHeight), rowfunctor);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, height), colfunctor);
#else
    size_t numThreads = QThread::idealThreadCount();
    size_t numBlock = std::min(height, numThreads * 2);
    size_t rowsPerBlock = height / numBlock;
    QList<std::pair<size_t,size_t>> allRange;
    for (size_t i=0; i<numBlock; ++i) {
      allRange.push_back(std::make_pair(i*rowsPerBlock,
                                        (i==numBlock-1) ? height : (i+1)*rowsPerBlock));
    }

    allRange[allRange.size()-1].second = desHeight;
    QtConcurrent::blockingMap(allRange, rowfunctor);

    allRange[allRange.size()-1].second = height;
    QtConcurrent::blockingMap(allRange, colfunctor);
#endif
  }
}

// boundaryOption see image2DFilter
template<typename TPixel, typename TPixelOut>
void image2DGaussianFilter(const TPixel* img, size_t width, size_t height,
                           double kernelSigmaX, double kernelSigmaY,
                           TPixelOut* imgOut,
                           int kernelWidth = -1, int kernelHeight = -1,
                           PadOption boundaryOption = PadOption::Constant, TPixel boundaryValue = TPixel(0),
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

  //ZBenchTimer bt;
  //bt.start();
  image2DFilter(img, width, height, rowkernel.data(), kWidth,
                colkernel.data(), kHeight,
                imgOut, boundaryOption, boundaryValue, true, useMultithreading);
  //bt.stopAndPrint();
#endif
}

// note: LoG filter will produce negative value, better use a out type that can represent negative pixel
// such as double
// boundaryOption see image2DFilter
template<typename TPixel, typename TPixelOut>
void image2DLoGFilter(const TPixel* img, size_t width, size_t height,
                      double kernelSigmaX, double kernelSigmaY,
                      TPixelOut* imgOut,
                      int kernelWidth = -1, int kernelHeight = -1,
                      PadOption boundaryOption = PadOption::Constant, TPixel boundaryValue = TPixel(0),
                      bool useMultithreading = true)
{
  size_t kWidth;
  size_t kHeight;

  std::vector<double> rowLoGkernel = create1DLoGKernel(kernelSigmaX, kernelWidth, &kWidth);
  std::vector<double> colLoGkernel = create1DLoGKernel(kernelSigmaY, kernelHeight, &kHeight);
  std::vector<double> rowGkernel = create1DGaussianKernel(kernelSigmaX, kernelWidth, &kWidth);
  std::vector<double> colGkernel = create1DGaussianKernel(kernelSigmaY, kernelHeight, &kHeight);

  //ZBenchTimer bt;
  //bt.start();
  image2DFilter(img, width, height, rowLoGkernel.data(), kWidth,
                colGkernel.data(), kHeight,
                imgOut, boundaryOption, boundaryValue, true, useMultithreading);

  std::vector<TPixelOut> bufImg(width * height);

  image2DFilter(img, width, height, rowGkernel.data(), kWidth,
                colLoGkernel.data(), kHeight,
                bufImg.data(), boundaryOption, boundaryValue, true, useMultithreading);

  for (size_t i = 0; i < bufImg.size(); ++i)
    imgOut[i] += bufImg[i];

  //bt.stopAndPrint();
}

void _resizeContributions(size_t inLength, size_t outLength, Interpolant interpolant, bool antialiasing,
                          std::vector<double>& weights, std::vector<size_t>& indices, size_t& kernelWidth);

template<typename TPixel, typename TPixelOut>
struct Resize2DForOneBlock
{
  Resize2DForOneBlock(const TPixel* img, size_t width, size_t height,
                      TPixelOut* imgOut, size_t outWidth, size_t outHeight,
                      const std::vector<double>& xWeights, const std::vector<size_t>& xIndices, size_t xKernelWidth,
                      const std::vector<double>& yWeights, const std::vector<size_t>& yIndices, size_t yKernelWidth)
    : m_img(img), m_width(width), m_height(height)
    , m_imgOut(imgOut), m_outWidth(outWidth), m_outHeight(outHeight)
    , m_xWeights(xWeights), m_xIndices(xIndices), m_xKernelWidth(xKernelWidth)
    , m_yWeights(yWeights), m_yIndices(yIndices), m_yKernelWidth(yKernelWidth)
  {
  }

  typedef void result_type;

#ifndef _USE_QTCONCURRENT_

  void operator()(const tbb::blocked_range<size_t>& range) const
#else
  void operator()(const std::pair<size_t,size_t> &range) const
#endif
  {
    if (m_xKernelWidth == 1 && m_yKernelWidth == 1) {
#ifndef _USE_QTCONCURRENT_
      for (size_t y = range.begin(); y != range.end(); ++y) {
#else
        for (size_t y=range.first; y<range.second; ++y) {
#endif
        for (size_t x = 0; x < m_outWidth; ++x) {
          m_imgOut[y * m_outWidth + x] = saturate_cast<TPixelOut>(m_img[m_yIndices[y] * m_width + m_xIndices[x]]);
        }
      }
    } else {
#ifndef _USE_QTCONCURRENT_
      for (size_t y = range.begin(); y != range.end(); ++y) {
#else
        for (size_t y=range.first; y<range.second; ++y) {
#endif
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
  const std::vector<double>& m_yWeights;
  const std::vector<size_t>& m_yIndices;
  size_t m_yKernelWidth;
};

//((scale-1)/2) in output image maps to 0 in input image, and ((3*scale-1)/2) in output
//image maps to 1 in input image.
// 'antialiasing' specifies whether to perform antialiasing when shrinking an image. For the 'nearest' method,
// the parameter 'antialiasingForNearest' is used (default false); for all other methods, the default is true.
template<typename TPixel, typename TPixelOut>
void image2DResize(const TPixel* img, size_t width, size_t height,
                   TPixelOut* imgOut, size_t outWidth, size_t outHeight,
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
  _resizeContributions(width, outWidth, interpolant,
                       interpolant == Interpolant::Nearest ? antialiasingForNearest : antialiasing,
                       xWeights, xIndices, xKernelWidth);
  _resizeContributions(height, outHeight, interpolant,
                       interpolant == Interpolant::Nearest ? antialiasingForNearest : antialiasing,
                       yWeights, yIndices, yKernelWidth);

  Resize2DForOneBlock<TPixel, TPixelOut> func(img, width, height, imgOut, outWidth, outHeight,
                                              xWeights, xIndices, xKernelWidth, yWeights, yIndices, yKernelWidth);
  if (!useMultithreading) {
#ifndef _USE_QTCONCURRENT_
    func(tbb::blocked_range<size_t>(0, outHeight));
#else
    func(std::pair<size_t,size_t>(0, outHeight));
#endif
  } else {
#ifndef _USE_QTCONCURRENT_
    tbb::parallel_for(tbb::blocked_range<size_t>(0, outHeight), func);
#else
    size_t numThreads = QThread::idealThreadCount();
    size_t numBlock = std::min(outHeight, numThreads * 2);
    size_t rowsPerBlock = outHeight / numBlock;
    QList<std::pair<size_t,size_t>> allRange;
    for (size_t i=0; i<numBlock; ++i) {
      allRange.push_back(std::make_pair(i*rowsPerBlock,
                                        (i==numBlock-1) ? outHeight : ((i+1)*rowsPerBlock)));
    }

    QtConcurrent::blockingMap(allRange, func);
#endif
  }
}

} // namespace nim

#endif // ZIMAGE2DUTILS_H
