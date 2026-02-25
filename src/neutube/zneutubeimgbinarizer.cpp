#include "zneutubeimgbinarizer.h"

#include "zneutubeimglocmax.h"
#include "zneutubeinthistogram.h"
#include "zneutubeneighborhood.h"
#include "zneutubeobjlabel.h"

#include "zexception.h"
#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace nim::neutube {

namespace {

[[nodiscard]] ZImg makeUint8VolumeLike(const ZImg& img)
{
  ZImgInfo info = img.info();
  info.setVoxelFormat<uint8_t>();
  info.createDefaultDescriptions();
  ZImg out(info);
  out.fill(0);
  return out;
}

void stackSubcLegacyLike(ZImg& stack, int subtr)
{
  if (stack.isEmpty()) {
    return;
  }
  if (subtr <= 0) {
    return;
  }

  // Use ZImg's saturating arithmetic implementation (same semantics as the legacy Stack_Subc path).
  //
  // Keep the same short-circuit behavior for unsupported types.
  if (!stack.isType<uint8_t>() && !stack.isType<uint16_t>()) {
    throw ZException(fmt::format("stackSubcLegacyLike: unsupported voxel type {}", stack.info()));
  }
  stack -= subtr;
}

[[nodiscard]] size_t fgAreaAboveThresholdLegacyLike(const ZImg& stack, int thre)
{
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  const size_t n = stack.voxelNumber();
  size_t count = 0;

  if (stack.isType<uint8_t>()) {
    const auto* a = stack.timeData<uint8_t>(0);
    const uint8_t t = static_cast<uint8_t>(std::clamp(thre, 0, 255));
    for (size_t i = 0; i < n; ++i) {
      if (a[i] > t) {
        ++count;
      }
    }
    return count;
  }

  if (stack.isType<uint16_t>()) {
    const auto* a = stack.timeData<uint16_t>(0);
    const uint16_t t = static_cast<uint16_t>(std::clamp(thre, 0, 65535));
    for (size_t i = 0; i < n; ++i) {
      if (a[i] > t) {
        ++count;
      }
    }
    return count;
  }

  throw ZException(fmt::format("fgAreaAboveThresholdLegacyLike: unsupported voxel type {}", stack.info()));
}

void thresholdBinarizeToUint8LegacyLike(const ZImg& in, int thre, ZImg& out)
{
  CHECK(in.numChannels() == 1);
  CHECK(in.numTimes() == 1);

  if (in.isEmpty()) {
    out.clear();
    return;
  }

  if (out.isEmpty()) {
    out = makeUint8VolumeLike(in);
  } else {
    CHECK(out.width() == in.width() && out.height() == in.height() && out.depth() == in.depth());
    CHECK(out.numChannels() == 1);
    CHECK(out.numTimes() == 1);
    CHECK(out.isType<uint8_t>());
  }

  const size_t n = in.voxelNumber();
  auto* dst = out.timeData<uint8_t>(0);

  if (in.isType<uint8_t>()) {
    const auto* src = in.timeData<uint8_t>(0);
    const uint8_t t = static_cast<uint8_t>(std::clamp(thre, 0, 255));
    for (size_t i = 0; i < n; ++i) {
      dst[i] = static_cast<uint8_t>(src[i] > t);
    }
    return;
  }

  if (in.isType<uint16_t>()) {
    const auto* src = in.timeData<uint16_t>(0);
    const uint16_t t = static_cast<uint16_t>(std::clamp(thre, 0, 65535));
    for (size_t i = 0; i < n; ++i) {
      dst[i] = static_cast<uint8_t>(src[i] > t);
    }
    return;
  }

  throw ZException(fmt::format("thresholdBinarizeToUint8LegacyLike: unsupported voxel type {}", in.info()));
}

[[nodiscard]] int refineLocmaxThresholdLegacyLike(const ZImg& stack,
                                                  int thre,
                                                  const IntHistogramLegacyLike& hist,
                                                  int upperBound,
                                                  int retryCount)
{
  // Port of ZStackBinarizer::refineLocmaxThreshold().
  constexpr double ratioLowThre = 0.015;
  constexpr double ratioThre = 0.05;

  const double voxelNumber = static_cast<double>(stack.voxelNumber());
  const double fgratio = static_cast<double>(fgAreaAboveThresholdLegacyLike(stack, thre)) / voxelNumber;

  double fgratio2 = fgratio;
  double prevFgratio = fgratio;

  if ((fgratio > ratioLowThre) && (fgratio <= ratioThre)) {
    const int thre2 = triangleThresholdLegacyLike(hist, thre + 1, upperBound - 1);
    fgratio2 = static_cast<double>(fgAreaAboveThresholdLegacyLike(stack, thre2)) / voxelNumber;
    if (fgratio2 / fgratio <= 0.3) {
      thre = thre2;
    }
  } else {
    int nretry = retryCount;
    int thre2 = thre;
    while (fgratio2 > ratioThre) {
      thre2 = triangleThresholdLegacyLike(hist, thre2 + 1, upperBound - 1);
      fgratio2 = static_cast<double>(fgAreaAboveThresholdLegacyLike(stack, thre2)) / voxelNumber;
      if (fgratio2 / prevFgratio <= 0.5) {
        thre = thre2;
      }
      prevFgratio = fgratio2;

      --nretry;
      if (nretry == 0) {
        break;
      }
    }
  }

  return thre;
}

[[nodiscard]] std::optional<IntHistogramLegacyLike> computeLocmaxHistLegacyLike(const ZImg& stack)
{
  // Port of ZStackBinarizer::computeLocmaxHist().
  constexpr int conn = 18;

  ZImg locmax = stackLocmaxRegionMaskLegacyLike(stack, conn);
  CHECK(locmax.isType<uint8_t>());

  // Port of Stack_Label_Objects_Ns(locmax, NULL, 1, 2, 3, conn) + conversion to 0/1.
  {
    auto* a = locmax.timeData<uint8_t>(0);
    const size_t n = locmax.voxelNumber();

    const ZNeighborhood& nb = neighborhoodLegacyOrder(conn);
    std::vector<size_t> q;
    q.reserve(1024);

    for (size_t seed = 0; seed < n; ++seed) {
      if (a[seed] != 1) {
        continue;
      }

      q.clear();
      q.push_back(seed);
      a[seed] = 2;

      while (!q.empty()) {
        const size_t idx = q.back();
        q.pop_back();

        const size_t width = locmax.width();
        const size_t height = locmax.height();
        const size_t depth = locmax.depth();
        const size_t plane = width * height;

        const int z = static_cast<int>(idx / plane);
        const size_t rem = idx - static_cast<size_t>(z) * plane;
        const int y = static_cast<int>(rem / width);
        const int x = static_cast<int>(rem - static_cast<size_t>(y) * width);

        for (size_t i = 0; i < nb.size(); ++i) {
          const ZVoxelCoordinate& o = nb.offset(i);
          const int nx = x + o.x;
          const int ny = y + o.y;
          const int nz = z + o.z;
          if (nx < 0 || ny < 0 || nz < 0 || nx >= static_cast<int>(width) || ny >= static_cast<int>(height) ||
              nz >= static_cast<int>(depth)) {
            continue;
          }
          const size_t nidx =
            static_cast<size_t>(nx) + static_cast<size_t>(ny) * width + static_cast<size_t>(nz) * plane;
          if (a[nidx] == 1) {
            a[nidx] = 2;
            q.push_back(nidx);
          }
        }
      }

      // Mark the seed voxel as 3.
      a[seed] = 3;
    }

    // Convert labeled objects into a mask of the seed voxels only.
    for (size_t i = 0; i < n; ++i) {
      a[i] = (a[i] < 3) ? uint8_t{0} : uint8_t{1};
    }
  }

  return imageHistogramLegacyLike(stack, &locmax);
}

} // namespace

int subtractBackgroundLegacyLike(ZImg& stack, double minFr, int maxIter)
{
  if (stack.isEmpty()) {
    return 0;
  }

  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  const auto histOpt = imageHistogramLegacyLike(stack, nullptr);
  if (!histOpt) {
    return 0;
  }
  const IntHistogramLegacyLike& hist = *histOpt;

  const int maxV = hist.maxValue();
  const int totalCount = hist.upperCount(0);
  CHECK(totalCount > 0);

  int commonIntensity = 0;

  const int darkCount = hist.count(0);
  if (static_cast<double>(darkCount) / static_cast<double>(totalCount) > 0.9) {
    commonIntensity = hist.minValue();
  } else {
    for (int iter = 0; iter < maxIter; ++iter) {
      const int mode = hist.mode(commonIntensity + 1, maxV);
      if (mode == maxV) {
        break;
      }
      commonIntensity = mode;

      const double fgRatio =
        static_cast<double>(hist.upperCount(commonIntensity + 1)) / static_cast<double>(totalCount);
      if (fgRatio < minFr) {
        break;
      }
    }
  }

  if (commonIntensity > 0) {
    stackSubcLegacyLike(stack, commonIntensity);
  }

  return commonIntensity;
}

BinarizeResultLegacyLike binarizeLocmaxLegacyLike(const ZImg& stack, int retryCount)
{
  BinarizeResultLegacyLike res;

  if (stack.isEmpty()) {
    return res;
  }

  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  if (!stack.isType<uint8_t>() && !stack.isType<uint16_t>()) {
    throw ZException(fmt::format("binarizeLocmaxLegacyLike: unsupported voxel type {}", stack.info()));
  }

  const size_t voxelNumber = stack.voxelNumber();

  const auto histOpt = imageHistogramLegacyLike(stack, nullptr);
  if (!histOpt) {
    return res;
  }
  const IntHistogramLegacyLike& hist = *histOpt;

  const int minV = hist.minValue();
  const int maxV = hist.maxValue();

  if (minV == maxV) {
    return res;
  }

  if (hist.count(minV) + hist.count(maxV) == static_cast<int>(voxelNumber)) {
    // Only two values: legacy short-circuit.
    res.actualThreshold = minV;
    thresholdBinarizeToUint8LegacyLike(stack, minV, res.binary);
    res.success = true;
    return res;
  }

  // LOCMAX thresholding (ZStackBinarizer::EMethod::LOCMAX).
  const auto locHistOpt = computeLocmaxHistLegacyLike(stack);
  if (!locHistOpt) {
    return res;
  }
  const IntHistogramLegacyLike& locHist = *locHistOpt;

  int low = locHist.minValue();
  int high = locHist.maxValue();

  if (low == high) {
    return res;
  }

  int threshold = 0;

  if (static_cast<double>(locHist.sum()) / static_cast<double>(voxelNumber) <= 1e-5) {
    threshold = hist.mode(0, high);
  } else {
    threshold = triangleThresholdLegacyLike(locHist, low, high - 1);
    threshold = refineLocmaxThresholdLegacyLike(stack, threshold, locHist, high, retryCount);
  }

  res.actualThreshold = threshold;
  thresholdBinarizeToUint8LegacyLike(stack, threshold, res.binary);
  res.success = true;
  return res;
}

} // namespace nim::neutube
