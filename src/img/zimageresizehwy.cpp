#include "zimageresizehwy.h"

#include "zimage2dutils.h"
#include "zlog.h"
#include "zsaturateoperation.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <type_traits>
#include <utility>
#include <vector>

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "zimageresizehwy.cpp"
#include "hwy/foreach_target.h"
#include "hwy/highway.h"

HWY_BEFORE_NAMESPACE();

namespace nim {
namespace HWY_NAMESPACE {

namespace hn = hwy::HWY_NAMESPACE;

namespace detail {

constexpr size_t kOutputRowsPerBlock = 32;
// Below this size, the full X-pass scratch usually stays cache-friendly and avoids block overlap recomputation.
constexpr size_t kFullScratchLimitBytes = 16ull * 1024ull * 1024ull;

size_t checkedMulOrDie(size_t a, size_t b)
{
  CHECK(a == 0 || b <= std::numeric_limits<size_t>::max() / a);
  return a * b;
}

template<typename Func>
void runRange(size_t begin, size_t end, bool useMultithreading, Func&& func)
{
  if (useMultithreading) {
    tbb::parallel_for(tbb::blocked_range<size_t>(begin, end), std::forward<Func>(func));
  } else {
    func(tbb::blocked_range<size_t>(begin, end));
  }
}

template<typename T>
void resizePlaneToDouble(const T* HWY_RESTRICT img,
                         size_t width,
                         size_t height,
                         const double* HWY_RESTRICT xWeights,
                         const size_t* HWY_RESTRICT xIndices,
                         size_t xKernelWidth,
                         const double* HWY_RESTRICT yWeights,
                         const size_t* HWY_RESTRICT yIndices,
                         size_t yKernelWidth,
                         double* HWY_RESTRICT imgOut,
                         size_t outWidth,
                         size_t outHeight,
                         double* HWY_RESTRICT xTmp,
                         bool useMultithreading)
{
  runRange(0, height, useMultithreading, [&](const tbb::blocked_range<size_t>& range) {
    for (size_t y = range.begin(); y != range.end(); ++y) {
      const T* srcRow = img + y * width;
      double* xTmpRow = xTmp + y * outWidth;
      for (size_t x = 0; x < outWidth; ++x) {
        double valx = 0.0;
        const double* xWeight = xWeights + x * xKernelWidth;
        const size_t* xIndex = xIndices + x * xKernelWidth;
        for (size_t kx = 0; kx < xKernelWidth; ++kx) {
          valx += xWeight[kx] * static_cast<double>(srcRow[xIndex[kx]]);
        }
        xTmpRow[x] = valx;
      }
    }
  });

  const hn::ScalableTag<double> d;
  const size_t lanes = hn::Lanes(d);

  runRange(0, outHeight, useMultithreading, [&](const tbb::blocked_range<size_t>& range) {
    for (size_t y = range.begin(); y != range.end(); ++y) {
      double* outRow = imgOut + y * outWidth;
      size_t x = 0;
      for (; x + lanes <= outWidth; x += lanes) {
        auto valxy = hn::Zero(d);
        const double* yWeight = yWeights + y * yKernelWidth;
        const size_t* yIndex = yIndices + y * yKernelWidth;
        for (size_t ky = 0; ky < yKernelWidth; ++ky) {
          const double* xTmpRow = xTmp + yIndex[ky] * outWidth + x;
          valxy = hn::MulAdd(hn::LoadU(d, xTmpRow), hn::Set(d, yWeight[ky]), valxy);
        }
        hn::StoreU(valxy, d, outRow + x);
      }

      for (; x < outWidth; ++x) {
        double valxy = 0.0;
        const double* yWeight = yWeights + y * yKernelWidth;
        const size_t* yIndex = yIndices + y * yKernelWidth;
        for (size_t ky = 0; ky < yKernelWidth; ++ky) {
          valxy += yWeight[ky] * xTmp[yIndex[ky] * outWidth + x];
        }
        outRow[x] = valxy;
      }
    }
  });
}

template<typename T>
void resizePlaneToOutput(const T* HWY_RESTRICT img,
                         size_t width,
                         size_t height,
                         const double* HWY_RESTRICT xWeights,
                         const size_t* HWY_RESTRICT xIndices,
                         size_t xKernelWidth,
                         const double* HWY_RESTRICT yWeights,
                         const size_t* HWY_RESTRICT yIndices,
                         size_t yKernelWidth,
                         T* HWY_RESTRICT imgOut,
                         size_t outWidth,
                         size_t outHeight,
                         double* HWY_RESTRICT xTmp,
                         bool useMultithreading)
{
  runRange(0, height, useMultithreading, [&](const tbb::blocked_range<size_t>& range) {
    for (size_t y = range.begin(); y != range.end(); ++y) {
      const T* srcRow = img + y * width;
      double* xTmpRow = xTmp + y * outWidth;
      for (size_t x = 0; x < outWidth; ++x) {
        double valx = 0.0;
        const double* xWeight = xWeights + x * xKernelWidth;
        const size_t* xIndex = xIndices + x * xKernelWidth;
        for (size_t kx = 0; kx < xKernelWidth; ++kx) {
          valx += xWeight[kx] * static_cast<double>(srcRow[xIndex[kx]]);
        }
        xTmpRow[x] = valx;
      }
    }
  });

  const hn::ScalableTag<double> d;
  const size_t lanes = hn::Lanes(d);

  runRange(0, outHeight, useMultithreading, [&](const tbb::blocked_range<size_t>& range) {
    std::vector<double> laneValues(lanes);
    for (size_t y = range.begin(); y != range.end(); ++y) {
      T* outRow = imgOut + y * outWidth;
      size_t x = 0;
      for (; x + lanes <= outWidth; x += lanes) {
        auto valxy = hn::Zero(d);
        const double* yWeight = yWeights + y * yKernelWidth;
        const size_t* yIndex = yIndices + y * yKernelWidth;
        for (size_t ky = 0; ky < yKernelWidth; ++ky) {
          const double* xTmpRow = xTmp + yIndex[ky] * outWidth + x;
          valxy = hn::MulAdd(hn::LoadU(d, xTmpRow), hn::Set(d, yWeight[ky]), valxy);
        }
        hn::StoreU(valxy, d, laneValues.data());
        for (size_t lane = 0; lane < lanes; ++lane) {
          outRow[x + lane] = saturate_cast<T>(laneValues[lane]);
        }
      }

      for (; x < outWidth; ++x) {
        double valxy = 0.0;
        const double* yWeight = yWeights + y * yKernelWidth;
        const size_t* yIndex = yIndices + y * yKernelWidth;
        for (size_t ky = 0; ky < yKernelWidth; ++ky) {
          valxy += yWeight[ky] * xTmp[yIndex[ky] * outWidth + x];
        }
        outRow[x] = saturate_cast<T>(valxy);
      }
    }
  });
}

template<typename Func>
void runOutputBlocks(size_t outHeight, bool useMultithreading, Func&& func)
{
  const size_t numBlocks = (outHeight + kOutputRowsPerBlock - 1) / kOutputRowsPerBlock;
  auto runBlockRange = [&](const tbb::blocked_range<size_t>& range) {
    for (size_t block = range.begin(); block != range.end(); ++block) {
      const size_t yBegin = block * kOutputRowsPerBlock;
      const size_t yEnd = std::min(outHeight, yBegin + kOutputRowsPerBlock);
      func(yBegin, yEnd);
    }
  };

  if (useMultithreading) {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, numBlocks), runBlockRange);
  } else {
    runBlockRange(tbb::blocked_range<size_t>(0, numBlocks));
  }
}

std::pair<size_t, size_t>
sourceRowBounds(const size_t* HWY_RESTRICT yIndices, size_t yKernelWidth, size_t yBegin, size_t yEnd)
{
  CHECK(yBegin < yEnd);
  size_t minSourceY = std::numeric_limits<size_t>::max();
  size_t maxSourceY = 0;
  for (size_t y = yBegin; y < yEnd; ++y) {
    const size_t* yIndex = yIndices + y * yKernelWidth;
    for (size_t ky = 0; ky < yKernelWidth; ++ky) {
      minSourceY = std::min(minSourceY, yIndex[ky]);
      maxSourceY = std::max(maxSourceY, yIndex[ky]);
    }
  }
  CHECK(minSourceY <= maxSourceY);
  return {minSourceY, maxSourceY};
}

template<typename T>
void resize2DToOutputBlocked(const T* HWY_RESTRICT img,
                             size_t width,
                             size_t height,
                             T* HWY_RESTRICT imgOut,
                             size_t outWidth,
                             size_t outHeight,
                             const double* HWY_RESTRICT xWeights,
                             const size_t* HWY_RESTRICT xIndices,
                             size_t xKernelWidth,
                             const double* HWY_RESTRICT yWeights,
                             const size_t* HWY_RESTRICT yIndices,
                             size_t yKernelWidth,
                             bool useMultithreading)
{
  const hn::ScalableTag<double> d;
  const size_t lanes = hn::Lanes(d);

  runOutputBlocks(outHeight, useMultithreading, [&](size_t yBegin, size_t yEnd) {
    const auto [minSourceY, maxSourceY] = sourceRowBounds(yIndices, yKernelWidth, yBegin, yEnd);
    CHECK(maxSourceY < height);
    const size_t xTmpHeight = maxSourceY - minSourceY + 1;
    std::vector<double> xTmp(checkedMulOrDie(xTmpHeight, outWidth));

    for (size_t sourceY = minSourceY; sourceY <= maxSourceY; ++sourceY) {
      const T* srcRow = img + sourceY * width;
      double* xTmpRow = xTmp.data() + (sourceY - minSourceY) * outWidth;
      for (size_t x = 0; x < outWidth; ++x) {
        double valx = 0.0;
        const double* xWeight = xWeights + x * xKernelWidth;
        const size_t* xIndex = xIndices + x * xKernelWidth;
        for (size_t kx = 0; kx < xKernelWidth; ++kx) {
          valx += xWeight[kx] * static_cast<double>(srcRow[xIndex[kx]]);
        }
        xTmpRow[x] = valx;
      }
    }

    std::vector<double> laneValues(lanes);
    for (size_t y = yBegin; y < yEnd; ++y) {
      T* outRow = imgOut + y * outWidth;
      const double* yWeight = yWeights + y * yKernelWidth;
      const size_t* yIndex = yIndices + y * yKernelWidth;
      size_t x = 0;
      for (; x + lanes <= outWidth; x += lanes) {
        auto valxy = hn::Zero(d);
        for (size_t ky = 0; ky < yKernelWidth; ++ky) {
          const double* xTmpRow = xTmp.data() + (yIndex[ky] - minSourceY) * outWidth + x;
          valxy = hn::MulAdd(hn::LoadU(d, xTmpRow), hn::Set(d, yWeight[ky]), valxy);
        }
        hn::StoreU(valxy, d, laneValues.data());
        for (size_t lane = 0; lane < lanes; ++lane) {
          outRow[x + lane] = saturate_cast<T>(laneValues[lane]);
        }
      }

      for (; x < outWidth; ++x) {
        double valxy = 0.0;
        for (size_t ky = 0; ky < yKernelWidth; ++ky) {
          valxy += yWeight[ky] * xTmp[(yIndex[ky] - minSourceY) * outWidth + x];
        }
        outRow[x] = saturate_cast<T>(valxy);
      }
    }
  });
}

void addScaledPlane(double* HWY_RESTRICT accum,
                    const double* HWY_RESTRICT plane,
                    size_t count,
                    double weight,
                    bool useMultithreading)
{
  if (weight == 0.0) {
    return;
  }

  const hn::ScalableTag<double> d;
  const size_t lanes = hn::Lanes(d);
  const auto weightVec = hn::Set(d, weight);

  auto func = [&](const tbb::blocked_range<size_t>& range) {
    size_t i = range.begin();
    for (; i + lanes <= range.end(); i += lanes) {
      const auto accumVec = hn::LoadU(d, accum + i);
      const auto planeVec = hn::LoadU(d, plane + i);
      hn::StoreU(hn::MulAdd(planeVec, weightVec, accumVec), d, accum + i);
    }

    for (; i < range.end(); ++i) {
      accum[i] += weight * plane[i];
    }
  };

  if (useMultithreading) {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, count, 16 * 1024), func);
  } else {
    func(tbb::blocked_range<size_t>(0, count));
  }
}

template<typename T>
void resize2DToOutput(const T* HWY_RESTRICT img,
                      size_t width,
                      size_t height,
                      T* HWY_RESTRICT imgOut,
                      size_t outWidth,
                      size_t outHeight,
                      const double* HWY_RESTRICT xWeights,
                      const size_t* HWY_RESTRICT xIndices,
                      size_t xKernelWidth,
                      const double* HWY_RESTRICT yWeights,
                      const size_t* HWY_RESTRICT yIndices,
                      size_t yKernelWidth,
                      bool useMultithreading)
{
  const size_t xTmpElements = checkedMulOrDie(height, outWidth);
  const size_t xTmpBytes = checkedMulOrDie(xTmpElements, sizeof(double));
  if (xTmpBytes <= kFullScratchLimitBytes) {
    std::vector<double> xTmp(xTmpElements);
    resizePlaneToOutput(img,
                        width,
                        height,
                        xWeights,
                        xIndices,
                        xKernelWidth,
                        yWeights,
                        yIndices,
                        yKernelWidth,
                        imgOut,
                        outWidth,
                        outHeight,
                        xTmp.data(),
                        useMultithreading);
  } else {
    resize2DToOutputBlocked(img,
                            width,
                            height,
                            imgOut,
                            outWidth,
                            outHeight,
                            xWeights,
                            xIndices,
                            xKernelWidth,
                            yWeights,
                            yIndices,
                            yKernelWidth,
                            useMultithreading);
  }
}

template<typename T>
void resize2DToDouble(const T* HWY_RESTRICT img,
                      size_t width,
                      size_t height,
                      double* HWY_RESTRICT imgOut,
                      size_t outWidth,
                      size_t outHeight,
                      const double* HWY_RESTRICT xWeights,
                      const size_t* HWY_RESTRICT xIndices,
                      size_t xKernelWidth,
                      const double* HWY_RESTRICT yWeights,
                      const size_t* HWY_RESTRICT yIndices,
                      size_t yKernelWidth,
                      bool useMultithreading)
{
  std::vector<double> xTmp(checkedMulOrDie(height, outWidth));
  resizePlaneToDouble(img,
                      width,
                      height,
                      xWeights,
                      xIndices,
                      xKernelWidth,
                      yWeights,
                      yIndices,
                      yKernelWidth,
                      imgOut,
                      outWidth,
                      outHeight,
                      xTmp.data(),
                      useMultithreading);
}

template<typename T>
void resize3DToDouble(const T* HWY_RESTRICT img,
                      size_t width,
                      size_t height,
                      size_t depth,
                      double* HWY_RESTRICT imgOut,
                      size_t outWidth,
                      size_t outHeight,
                      size_t outDepth,
                      const double* HWY_RESTRICT xWeights,
                      const size_t* HWY_RESTRICT xIndices,
                      size_t xKernelWidth,
                      const double* HWY_RESTRICT yWeights,
                      const size_t* HWY_RESTRICT yIndices,
                      size_t yKernelWidth,
                      const size_t* HWY_RESTRICT zContributionOffsets,
                      const size_t* HWY_RESTRICT zContributionOutIndices,
                      const double* HWY_RESTRICT zContributionWeights,
                      bool useMultithreading)
{
  const size_t sourcePlaneSize = checkedMulOrDie(width, height);
  const size_t targetPlaneSize = checkedMulOrDie(outWidth, outHeight);
  const size_t targetVoxelCount = checkedMulOrDie(targetPlaneSize, outDepth);

  std::fill(imgOut, imgOut + targetVoxelCount, 0.0);

  std::vector<double> xTmp(checkedMulOrDie(height, outWidth));
  std::vector<double> plane(targetPlaneSize);

  for (size_t z = 0; z < depth; ++z) {
    resizePlaneToDouble(img + z * sourcePlaneSize,
                        width,
                        height,
                        xWeights,
                        xIndices,
                        xKernelWidth,
                        yWeights,
                        yIndices,
                        yKernelWidth,
                        plane.data(),
                        outWidth,
                        outHeight,
                        xTmp.data(),
                        useMultithreading);

    for (size_t offset = zContributionOffsets[z]; offset < zContributionOffsets[z + 1]; ++offset) {
      const size_t outZ = zContributionOutIndices[offset];
      const double weight = zContributionWeights[offset];
      addScaledPlane(imgOut + outZ * targetPlaneSize, plane.data(), targetPlaneSize, weight, useMultithreading);
    }
  }
}

} // namespace detail

#define ATLAS_DEFINE_RESIZE_TO_OUTPUT_FUNCTIONS(Suffix, Type)          \
  void resize2D##Suffix##ToOutput(const Type* HWY_RESTRICT img,        \
                                  size_t width,                        \
                                  size_t height,                       \
                                  Type* HWY_RESTRICT imgOut,           \
                                  size_t outWidth,                     \
                                  size_t outHeight,                    \
                                  const double* HWY_RESTRICT xWeights, \
                                  const size_t* HWY_RESTRICT xIndices, \
                                  size_t xKernelWidth,                 \
                                  const double* HWY_RESTRICT yWeights, \
                                  const size_t* HWY_RESTRICT yIndices, \
                                  size_t yKernelWidth,                 \
                                  bool useMultithreading)              \
  {                                                                    \
    detail::resize2DToOutput(img,                                      \
                             width,                                    \
                             height,                                   \
                             imgOut,                                   \
                             outWidth,                                 \
                             outHeight,                                \
                             xWeights,                                 \
                             xIndices,                                 \
                             xKernelWidth,                             \
                             yWeights,                                 \
                             yIndices,                                 \
                             yKernelWidth,                             \
                             useMultithreading);                       \
  }

ATLAS_DEFINE_RESIZE_TO_OUTPUT_FUNCTIONS(U8, uint8_t)
ATLAS_DEFINE_RESIZE_TO_OUTPUT_FUNCTIONS(U16, uint16_t)
ATLAS_DEFINE_RESIZE_TO_OUTPUT_FUNCTIONS(U32, uint32_t)
ATLAS_DEFINE_RESIZE_TO_OUTPUT_FUNCTIONS(U64, uint64_t)
ATLAS_DEFINE_RESIZE_TO_OUTPUT_FUNCTIONS(I8, int8_t)
ATLAS_DEFINE_RESIZE_TO_OUTPUT_FUNCTIONS(I16, int16_t)
ATLAS_DEFINE_RESIZE_TO_OUTPUT_FUNCTIONS(I32, int32_t)
ATLAS_DEFINE_RESIZE_TO_OUTPUT_FUNCTIONS(I64, int64_t)
ATLAS_DEFINE_RESIZE_TO_OUTPUT_FUNCTIONS(F32, float)
ATLAS_DEFINE_RESIZE_TO_OUTPUT_FUNCTIONS(F64, double)

#undef ATLAS_DEFINE_RESIZE_TO_OUTPUT_FUNCTIONS

#define ATLAS_DEFINE_RESIZE_TO_DOUBLE_FUNCTIONS(Suffix, Type)                         \
  void resize2D##Suffix##ToDouble(const Type* HWY_RESTRICT img,                       \
                                  size_t width,                                       \
                                  size_t height,                                      \
                                  double* HWY_RESTRICT imgOut,                        \
                                  size_t outWidth,                                    \
                                  size_t outHeight,                                   \
                                  const double* HWY_RESTRICT xWeights,                \
                                  const size_t* HWY_RESTRICT xIndices,                \
                                  size_t xKernelWidth,                                \
                                  const double* HWY_RESTRICT yWeights,                \
                                  const size_t* HWY_RESTRICT yIndices,                \
                                  size_t yKernelWidth,                                \
                                  bool useMultithreading)                             \
  {                                                                                   \
    detail::resize2DToDouble(img,                                                     \
                             width,                                                   \
                             height,                                                  \
                             imgOut,                                                  \
                             outWidth,                                                \
                             outHeight,                                               \
                             xWeights,                                                \
                             xIndices,                                                \
                             xKernelWidth,                                            \
                             yWeights,                                                \
                             yIndices,                                                \
                             yKernelWidth,                                            \
                             useMultithreading);                                      \
  }                                                                                   \
  void resize3D##Suffix##ToDouble(const Type* HWY_RESTRICT img,                       \
                                  size_t width,                                       \
                                  size_t height,                                      \
                                  size_t depth,                                       \
                                  double* HWY_RESTRICT imgOut,                        \
                                  size_t outWidth,                                    \
                                  size_t outHeight,                                   \
                                  size_t outDepth,                                    \
                                  const double* HWY_RESTRICT xWeights,                \
                                  const size_t* HWY_RESTRICT xIndices,                \
                                  size_t xKernelWidth,                                \
                                  const double* HWY_RESTRICT yWeights,                \
                                  const size_t* HWY_RESTRICT yIndices,                \
                                  size_t yKernelWidth,                                \
                                  const size_t* HWY_RESTRICT zContributionOffsets,    \
                                  const size_t* HWY_RESTRICT zContributionOutIndices, \
                                  const double* HWY_RESTRICT zContributionWeights,    \
                                  bool useMultithreading)                             \
  {                                                                                   \
    detail::resize3DToDouble(img,                                                     \
                             width,                                                   \
                             height,                                                  \
                             depth,                                                   \
                             imgOut,                                                  \
                             outWidth,                                                \
                             outHeight,                                               \
                             outDepth,                                                \
                             xWeights,                                                \
                             xIndices,                                                \
                             xKernelWidth,                                            \
                             yWeights,                                                \
                             yIndices,                                                \
                             yKernelWidth,                                            \
                             zContributionOffsets,                                    \
                             zContributionOutIndices,                                 \
                             zContributionWeights,                                    \
                             useMultithreading);                                      \
  }

ATLAS_DEFINE_RESIZE_TO_DOUBLE_FUNCTIONS(U8, uint8_t)
ATLAS_DEFINE_RESIZE_TO_DOUBLE_FUNCTIONS(U16, uint16_t)
ATLAS_DEFINE_RESIZE_TO_DOUBLE_FUNCTIONS(U32, uint32_t)
ATLAS_DEFINE_RESIZE_TO_DOUBLE_FUNCTIONS(U64, uint64_t)
ATLAS_DEFINE_RESIZE_TO_DOUBLE_FUNCTIONS(I8, int8_t)
ATLAS_DEFINE_RESIZE_TO_DOUBLE_FUNCTIONS(I16, int16_t)
ATLAS_DEFINE_RESIZE_TO_DOUBLE_FUNCTIONS(I32, int32_t)
ATLAS_DEFINE_RESIZE_TO_DOUBLE_FUNCTIONS(I64, int64_t)
ATLAS_DEFINE_RESIZE_TO_DOUBLE_FUNCTIONS(F32, float)
ATLAS_DEFINE_RESIZE_TO_DOUBLE_FUNCTIONS(F64, double)

#undef ATLAS_DEFINE_RESIZE_TO_DOUBLE_FUNCTIONS

} // namespace HWY_NAMESPACE
} // namespace nim

HWY_AFTER_NAMESPACE();

#if HWY_ONCE

namespace nim {

HWY_EXPORT(resize2DU8ToOutput);
HWY_EXPORT(resize2DU16ToOutput);
HWY_EXPORT(resize2DU32ToOutput);
HWY_EXPORT(resize2DU64ToOutput);
HWY_EXPORT(resize2DI8ToOutput);
HWY_EXPORT(resize2DI16ToOutput);
HWY_EXPORT(resize2DI32ToOutput);
HWY_EXPORT(resize2DI64ToOutput);
HWY_EXPORT(resize2DF32ToOutput);
HWY_EXPORT(resize2DF64ToOutput);
HWY_EXPORT(resize2DU8ToDouble);
HWY_EXPORT(resize2DU16ToDouble);
HWY_EXPORT(resize2DU32ToDouble);
HWY_EXPORT(resize2DU64ToDouble);
HWY_EXPORT(resize2DI8ToDouble);
HWY_EXPORT(resize2DI16ToDouble);
HWY_EXPORT(resize2DI32ToDouble);
HWY_EXPORT(resize2DI64ToDouble);
HWY_EXPORT(resize2DF32ToDouble);
HWY_EXPORT(resize2DF64ToDouble);
HWY_EXPORT(resize3DU8ToDouble);
HWY_EXPORT(resize3DU16ToDouble);
HWY_EXPORT(resize3DU32ToDouble);
HWY_EXPORT(resize3DU64ToDouble);
HWY_EXPORT(resize3DI8ToDouble);
HWY_EXPORT(resize3DI16ToDouble);
HWY_EXPORT(resize3DI32ToDouble);
HWY_EXPORT(resize3DI64ToDouble);
HWY_EXPORT(resize3DF32ToDouble);
HWY_EXPORT(resize3DF64ToDouble);

namespace {

struct ResizeContributions
{
  std::vector<double> weights;
  std::vector<size_t> indices;
  size_t kernelWidth = 0;
  bool kernelIsTrivial = false;
};

struct SourceZContributionTable
{
  std::vector<size_t> offsets;
  std::vector<size_t> outIndices;
  std::vector<double> weights;
};

size_t checkedMulSizeOrDie(size_t a, size_t b)
{
  CHECK(a == 0 || b <= std::numeric_limits<size_t>::max() / a);
  return a * b;
}

bool checkedMulUint64(uint64_t a, uint64_t b, uint64_t& out)
{
  if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) {
    return false;
  }
  out = a * b;
  return true;
}

bool checkedAddUint64(uint64_t a, uint64_t b, uint64_t& out)
{
  if (b > std::numeric_limits<uint64_t>::max() - a) {
    return false;
  }
  out = a + b;
  return true;
}

bool checkedElementBytes(uint64_t count, size_t elementSize, uint64_t& out)
{
  return checkedMulUint64(count, static_cast<uint64_t>(elementSize), out);
}

bool effectiveAntialiasing(Interpolant interpolant, bool antialiasing, bool antialiasingForNearest)
{
  return interpolant == Interpolant::Nearest ? antialiasingForNearest : antialiasing;
}

ResizeContributions resizeContributions(size_t inLength,
                                        size_t outLength,
                                        Interpolant interpolant,
                                        bool antialiasing,
                                        bool antialiasingForNearest)
{
  ResizeContributions result;
  _resizeContributions(inLength,
                       outLength,
                       interpolant,
                       effectiveAntialiasing(interpolant, antialiasing, antialiasingForNearest),
                       result.weights,
                       result.indices,
                       result.kernelWidth,
                       result.kernelIsTrivial);
  return result;
}

SourceZContributionTable
buildSourceZContributionTable(size_t depth, size_t outDepth, const ResizeContributions& zContributions)
{
  SourceZContributionTable table;
  table.offsets.assign(depth + 1, 0);

  for (size_t outZ = 0; outZ < outDepth; ++outZ) {
    const double* zWeight = zContributions.weights.data() + outZ * zContributions.kernelWidth;
    const size_t* zIndex = zContributions.indices.data() + outZ * zContributions.kernelWidth;
    for (size_t kz = 0; kz < zContributions.kernelWidth; ++kz) {
      if (zWeight[kz] == 0.0) {
        continue;
      }
      CHECK(zIndex[kz] < depth);
      ++table.offsets[zIndex[kz] + 1];
    }
  }

  std::partial_sum(table.offsets.begin(), table.offsets.end(), table.offsets.begin());
  table.outIndices.resize(table.offsets.back());
  table.weights.resize(table.offsets.back());

  std::vector<size_t> cursors = table.offsets;
  for (size_t outZ = 0; outZ < outDepth; ++outZ) {
    const double* zWeight = zContributions.weights.data() + outZ * zContributions.kernelWidth;
    const size_t* zIndex = zContributions.indices.data() + outZ * zContributions.kernelWidth;
    for (size_t kz = 0; kz < zContributions.kernelWidth; ++kz) {
      if (zWeight[kz] == 0.0) {
        continue;
      }
      const size_t offset = cursors[zIndex[kz]]++;
      table.outIndices[offset] = outZ;
      table.weights[offset] = zWeight[kz];
    }
  }

  return table;
}

template<typename T>
void convertFromDouble(const double* src, size_t count, T* dst, bool useMultithreading)
{
  auto func = [&](const tbb::blocked_range<size_t>& range) {
    for (size_t i = range.begin(); i != range.end(); ++i) {
      dst[i] = saturate_cast<T>(src[i]);
    }
  };
  if (useMultithreading) {
    tbb::parallel_for(tbb::blocked_range<size_t>(0, count), func);
  } else {
    func(tbb::blocked_range<size_t>(0, count));
  }
}

template<typename T>
inline constexpr bool AlwaysFalse = false;

template<typename T>
void dispatchResize2DToOutput(const T* img,
                              size_t width,
                              size_t height,
                              T* imgOut,
                              size_t outWidth,
                              size_t outHeight,
                              const ResizeContributions& xContributions,
                              const ResizeContributions& yContributions,
                              bool useMultithreading)
{
#define ATLAS_DISPATCH_RESIZE_2D(Type, Function)                  \
  if constexpr (std::is_same_v<T, Type>) {                        \
    HWY_DYNAMIC_DISPATCH(Function)(img,                           \
                                   width,                         \
                                   height,                        \
                                   imgOut,                        \
                                   outWidth,                      \
                                   outHeight,                     \
                                   xContributions.weights.data(), \
                                   xContributions.indices.data(), \
                                   xContributions.kernelWidth,    \
                                   yContributions.weights.data(), \
                                   yContributions.indices.data(), \
                                   yContributions.kernelWidth,    \
                                   useMultithreading);            \
  }

  ATLAS_DISPATCH_RESIZE_2D(uint8_t, resize2DU8ToOutput)
  else ATLAS_DISPATCH_RESIZE_2D(uint16_t, resize2DU16ToOutput) else ATLAS_DISPATCH_RESIZE_2D(uint32_t, resize2DU32ToOutput) else ATLAS_DISPATCH_RESIZE_2D(uint64_t, resize2DU64ToOutput) else ATLAS_DISPATCH_RESIZE_2D(int8_t, resize2DI8ToOutput) else ATLAS_DISPATCH_RESIZE_2D(
    int16_t,
    resize2DI16ToOutput) else ATLAS_DISPATCH_RESIZE_2D(int32_t,
                                                       resize2DI32ToOutput) else ATLAS_DISPATCH_RESIZE_2D(int64_t,
                                                                                                          resize2DI64ToOutput) else ATLAS_DISPATCH_RESIZE_2D(float,
                                                                                                                                                             resize2DF32ToOutput) else ATLAS_DISPATCH_RESIZE_2D(double,
                                                                                                                                                                                                                resize2DF64ToOutput) else
  {
    static_assert(AlwaysFalse<T>, "Unsupported Highway resize output type");
  }

#undef ATLAS_DISPATCH_RESIZE_2D
}

template<typename T>
void dispatchResize2DToDouble(const T* img,
                              size_t width,
                              size_t height,
                              double* imgOut,
                              size_t outWidth,
                              size_t outHeight,
                              const ResizeContributions& xContributions,
                              const ResizeContributions& yContributions,
                              bool useMultithreading)
{
#define ATLAS_DISPATCH_RESIZE_2D(Type, Function)                  \
  if constexpr (std::is_same_v<T, Type>) {                        \
    HWY_DYNAMIC_DISPATCH(Function)(img,                           \
                                   width,                         \
                                   height,                        \
                                   imgOut,                        \
                                   outWidth,                      \
                                   outHeight,                     \
                                   xContributions.weights.data(), \
                                   xContributions.indices.data(), \
                                   xContributions.kernelWidth,    \
                                   yContributions.weights.data(), \
                                   yContributions.indices.data(), \
                                   yContributions.kernelWidth,    \
                                   useMultithreading);            \
  }

  ATLAS_DISPATCH_RESIZE_2D(uint8_t, resize2DU8ToDouble)
  else ATLAS_DISPATCH_RESIZE_2D(uint16_t, resize2DU16ToDouble) else ATLAS_DISPATCH_RESIZE_2D(uint32_t, resize2DU32ToDouble) else ATLAS_DISPATCH_RESIZE_2D(uint64_t, resize2DU64ToDouble) else ATLAS_DISPATCH_RESIZE_2D(int8_t, resize2DI8ToDouble) else ATLAS_DISPATCH_RESIZE_2D(
    int16_t,
    resize2DI16ToDouble) else ATLAS_DISPATCH_RESIZE_2D(int32_t,
                                                       resize2DI32ToDouble) else ATLAS_DISPATCH_RESIZE_2D(int64_t,
                                                                                                          resize2DI64ToDouble) else ATLAS_DISPATCH_RESIZE_2D(float,
                                                                                                                                                             resize2DF32ToDouble) else ATLAS_DISPATCH_RESIZE_2D(double,
                                                                                                                                                                                                                resize2DF64ToDouble) else
  {
    static_assert(AlwaysFalse<T>, "Unsupported Highway resize type");
  }

#undef ATLAS_DISPATCH_RESIZE_2D
}

template<typename T>
void dispatchResize3DToDouble(const T* img,
                              size_t width,
                              size_t height,
                              size_t depth,
                              double* imgOut,
                              size_t outWidth,
                              size_t outHeight,
                              size_t outDepth,
                              const ResizeContributions& xContributions,
                              const ResizeContributions& yContributions,
                              const SourceZContributionTable& sourceZContributions,
                              bool useMultithreading)
{
#define ATLAS_DISPATCH_RESIZE_3D(Type, Function)                           \
  if constexpr (std::is_same_v<T, Type>) {                                 \
    HWY_DYNAMIC_DISPATCH(Function)(img,                                    \
                                   width,                                  \
                                   height,                                 \
                                   depth,                                  \
                                   imgOut,                                 \
                                   outWidth,                               \
                                   outHeight,                              \
                                   outDepth,                               \
                                   xContributions.weights.data(),          \
                                   xContributions.indices.data(),          \
                                   xContributions.kernelWidth,             \
                                   yContributions.weights.data(),          \
                                   yContributions.indices.data(),          \
                                   yContributions.kernelWidth,             \
                                   sourceZContributions.offsets.data(),    \
                                   sourceZContributions.outIndices.data(), \
                                   sourceZContributions.weights.data(),    \
                                   useMultithreading);                     \
  }

  ATLAS_DISPATCH_RESIZE_3D(uint8_t, resize3DU8ToDouble)
  else ATLAS_DISPATCH_RESIZE_3D(uint16_t, resize3DU16ToDouble) else ATLAS_DISPATCH_RESIZE_3D(uint32_t, resize3DU32ToDouble) else ATLAS_DISPATCH_RESIZE_3D(uint64_t, resize3DU64ToDouble) else ATLAS_DISPATCH_RESIZE_3D(int8_t, resize3DI8ToDouble) else ATLAS_DISPATCH_RESIZE_3D(
    int16_t,
    resize3DI16ToDouble) else ATLAS_DISPATCH_RESIZE_3D(int32_t,
                                                       resize3DI32ToDouble) else ATLAS_DISPATCH_RESIZE_3D(int64_t,
                                                                                                          resize3DI64ToDouble) else ATLAS_DISPATCH_RESIZE_3D(float,
                                                                                                                                                             resize3DF32ToDouble) else ATLAS_DISPATCH_RESIZE_3D(double,
                                                                                                                                                                                                                resize3DF64ToDouble) else
  {
    static_assert(AlwaysFalse<T>, "Unsupported Highway resize type");
  }

#undef ATLAS_DISPATCH_RESIZE_3D
}

template<typename T>
void image2DResizeHighwayImpl(const T* img,
                              size_t width,
                              size_t height,
                              T* imgOut,
                              size_t outWidth,
                              size_t outHeight,
                              Interpolant interpolant,
                              bool antialiasing,
                              bool antialiasingForNearest,
                              bool useMultithreading)
{
  CHECK(img != nullptr);
  CHECK(imgOut != nullptr);
  CHECK(width > 0);
  CHECK(height > 0);
  CHECK(outWidth > 0);
  CHECK(outHeight > 0);

  const auto xContributions = resizeContributions(width, outWidth, interpolant, antialiasing, antialiasingForNearest);
  const auto yContributions = resizeContributions(height, outHeight, interpolant, antialiasing, antialiasingForNearest);

  dispatchResize2DToOutput(img,
                           width,
                           height,
                           imgOut,
                           outWidth,
                           outHeight,
                           xContributions,
                           yContributions,
                           useMultithreading);
}

template<typename T>
void image3DResizeHighwayImpl(const T* img,
                              size_t width,
                              size_t height,
                              size_t depth,
                              T* imgOut,
                              size_t outWidth,
                              size_t outHeight,
                              size_t outDepth,
                              Interpolant interpolant,
                              bool antialiasing,
                              bool antialiasingForNearest,
                              bool useMultithreading)
{
  CHECK(img != nullptr);
  CHECK(imgOut != nullptr);
  CHECK(width > 0);
  CHECK(height > 0);
  CHECK(depth > 0);
  CHECK(outWidth > 0);
  CHECK(outHeight > 0);
  CHECK(outDepth > 0);

  const auto xContributions = resizeContributions(width, outWidth, interpolant, antialiasing, antialiasingForNearest);
  const auto yContributions = resizeContributions(height, outHeight, interpolant, antialiasing, antialiasingForNearest);
  const auto zContributions = resizeContributions(depth, outDepth, interpolant, antialiasing, antialiasingForNearest);
  const auto sourceZContributions = buildSourceZContributionTable(depth, outDepth, zContributions);
  const size_t outVoxelCount = checkedMulSizeOrDie(checkedMulSizeOrDie(outWidth, outHeight), outDepth);

  if constexpr (std::is_same_v<T, double>) {
    dispatchResize3DToDouble(img,
                             width,
                             height,
                             depth,
                             imgOut,
                             outWidth,
                             outHeight,
                             outDepth,
                             xContributions,
                             yContributions,
                             sourceZContributions,
                             useMultithreading);
  } else {
    std::vector<double> tmp(outVoxelCount);
    dispatchResize3DToDouble(img,
                             width,
                             height,
                             depth,
                             tmp.data(),
                             outWidth,
                             outHeight,
                             outDepth,
                             xContributions,
                             yContributions,
                             sourceZContributions,
                             useMultithreading);
    convertFromDouble(tmp.data(), tmp.size(), imgOut, useMultithreading);
  }
}

} // namespace

uint64_t image2DResizeHighwayExtraBytes(size_t height,
                                        size_t outWidth,
                                        size_t outHeight,
                                        Interpolant interpolant,
                                        bool antialiasing,
                                        bool antialiasingForNearest,
                                        size_t maxConcurrency)
{
  CHECK(height > 0);
  CHECK(outWidth > 0);
  CHECK(outHeight > 0);
  constexpr size_t kPolicyOutputRowsPerBlock = 32;
  constexpr size_t kPolicyFullScratchLimitBytes = 16ull * 1024ull * 1024ull;

  uint64_t fullScratchElements = 0;
  uint64_t fullScratchBytes = 0;
  if (!checkedMulUint64(static_cast<uint64_t>(height), static_cast<uint64_t>(outWidth), fullScratchElements) ||
      !checkedElementBytes(fullScratchElements, sizeof(double), fullScratchBytes)) {
    return std::numeric_limits<uint64_t>::max();
  }
  if (fullScratchBytes <= kPolicyFullScratchLimitBytes) {
    return fullScratchBytes;
  }

  const ResizeContributions yContributions =
    resizeContributions(height, outHeight, interpolant, antialiasing, antialiasingForNearest);
  const uint64_t numBlocks =
    (static_cast<uint64_t>(outHeight) + kPolicyOutputRowsPerBlock - 1) / kPolicyOutputRowsPerBlock;
  uint64_t maxBlockBytes = 0;
  for (uint64_t block = 0; block < numBlocks; ++block) {
    const size_t yBegin = static_cast<size_t>(block) * kPolicyOutputRowsPerBlock;
    const size_t yEnd = std::min(outHeight, yBegin + kPolicyOutputRowsPerBlock);
    size_t minSourceY = std::numeric_limits<size_t>::max();
    size_t maxSourceY = 0;
    for (size_t y = yBegin; y < yEnd; ++y) {
      const size_t* yIndex = yContributions.indices.data() + y * yContributions.kernelWidth;
      for (size_t ky = 0; ky < yContributions.kernelWidth; ++ky) {
        minSourceY = std::min(minSourceY, yIndex[ky]);
        maxSourceY = std::max(maxSourceY, yIndex[ky]);
      }
    }
    CHECK(minSourceY <= maxSourceY);

    uint64_t blockElements = 0;
    uint64_t blockBytes = 0;
    if (!checkedMulUint64(static_cast<uint64_t>(maxSourceY - minSourceY + 1),
                          static_cast<uint64_t>(outWidth),
                          blockElements) ||
        !checkedElementBytes(blockElements, sizeof(double), blockBytes)) {
      return std::numeric_limits<uint64_t>::max();
    }
    maxBlockBytes = std::max(maxBlockBytes, blockBytes);
  }

  const uint64_t concurrentBlocks =
    std::min<uint64_t>(numBlocks, std::max<uint64_t>(1, static_cast<uint64_t>(maxConcurrency)));
  uint64_t totalBytes = 0;
  if (!checkedMulUint64(maxBlockBytes, concurrentBlocks, totalBytes)) {
    return std::numeric_limits<uint64_t>::max();
  }
  return totalBytes;
}

template<typename T>
uint64_t image3DResizeHighwayExtraBytes(size_t height, size_t outWidth, size_t outHeight, size_t outDepth)
{
  CHECK(height > 0);
  CHECK(outWidth > 0);
  CHECK(outHeight > 0);
  CHECK(outDepth > 0);

  uint64_t xTmpElements = 0;
  uint64_t planeElements = 0;
  uint64_t targetElements = 0;
  uint64_t xTmpBytes = 0;
  uint64_t planeBytes = 0;
  uint64_t targetTmpBytes = 0;
  uint64_t totalBytes = 0;
  if (!checkedMulUint64(static_cast<uint64_t>(height), static_cast<uint64_t>(outWidth), xTmpElements) ||
      !checkedMulUint64(static_cast<uint64_t>(outWidth), static_cast<uint64_t>(outHeight), planeElements) ||
      !checkedElementBytes(xTmpElements, sizeof(double), xTmpBytes) ||
      !checkedElementBytes(planeElements, sizeof(double), planeBytes) ||
      !checkedAddUint64(xTmpBytes, planeBytes, totalBytes)) {
    return std::numeric_limits<uint64_t>::max();
  }

  if constexpr (!std::is_same_v<T, double>) {
    uint64_t withAccumulator = 0;
    if (!checkedMulUint64(planeElements, static_cast<uint64_t>(outDepth), targetElements) ||
        !checkedElementBytes(targetElements, sizeof(double), targetTmpBytes) ||
        !checkedAddUint64(totalBytes, targetTmpBytes, withAccumulator)) {
      return std::numeric_limits<uint64_t>::max();
    }
    totalBytes = withAccumulator;
  }

  return totalBytes;
}

template<typename T>
void image2DResizeHighway(const T* img,
                          size_t width,
                          size_t height,
                          T* imgOut,
                          size_t outWidth,
                          size_t outHeight,
                          Interpolant interpolant,
                          bool antialiasing,
                          bool antialiasingForNearest,
                          bool useMultithreading)
{
  image2DResizeHighwayImpl(img,
                           width,
                           height,
                           imgOut,
                           outWidth,
                           outHeight,
                           interpolant,
                           antialiasing,
                           antialiasingForNearest,
                           useMultithreading);
}

template<typename T>
void image3DResizeHighway(const T* img,
                          size_t width,
                          size_t height,
                          size_t depth,
                          T* imgOut,
                          size_t outWidth,
                          size_t outHeight,
                          size_t outDepth,
                          Interpolant interpolant,
                          bool antialiasing,
                          bool antialiasingForNearest,
                          bool useMultithreading)
{
  image3DResizeHighwayImpl(img,
                           width,
                           height,
                           depth,
                           imgOut,
                           outWidth,
                           outHeight,
                           outDepth,
                           interpolant,
                           antialiasing,
                           antialiasingForNearest,
                           useMultithreading);
}

#define ATLAS_INSTANTIATE_RESIZE_HIGHWAY(Type)                                                                   \
  template uint64_t image3DResizeHighwayExtraBytes<Type>(size_t, size_t, size_t, size_t);                        \
  template void                                                                                                  \
  image2DResizeHighway<Type>(const Type*, size_t, size_t, Type*, size_t, size_t, Interpolant, bool, bool, bool); \
  template void image3DResizeHighway<                                                                            \
    Type>(const Type*, size_t, size_t, size_t, Type*, size_t, size_t, size_t, Interpolant, bool, bool, bool);

ATLAS_INSTANTIATE_RESIZE_HIGHWAY(uint8_t)
ATLAS_INSTANTIATE_RESIZE_HIGHWAY(uint16_t)
ATLAS_INSTANTIATE_RESIZE_HIGHWAY(uint32_t)
ATLAS_INSTANTIATE_RESIZE_HIGHWAY(uint64_t)
ATLAS_INSTANTIATE_RESIZE_HIGHWAY(int8_t)
ATLAS_INSTANTIATE_RESIZE_HIGHWAY(int16_t)
ATLAS_INSTANTIATE_RESIZE_HIGHWAY(int32_t)
ATLAS_INSTANTIATE_RESIZE_HIGHWAY(int64_t)
ATLAS_INSTANTIATE_RESIZE_HIGHWAY(float)
ATLAS_INSTANTIATE_RESIZE_HIGHWAY(double)

#undef ATLAS_INSTANTIATE_RESIZE_HIGHWAY

} // namespace nim

#endif // HWY_ONCE
