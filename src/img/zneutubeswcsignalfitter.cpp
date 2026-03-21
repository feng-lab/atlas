#include "zneutubeswcsignalfitter.h"

#include "zneutubeinthistogram.h"
#include "zneutubemathutils.h"
#include "zneutubeplanaredt.h"

#include "zexception.h"
#include "zimgbinaryopslegacy.h"
#include "zimgbwthinlegacy.h"
#include "zimgmedianfilterlegacy.h"
#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace nim {

namespace {

void shrinkSkeletonOnceLegacyLike(ZImg& skel)
{
  CHECK(skel.numChannels() == 1);
  CHECK(skel.numTimes() == 1);
  CHECK(skel.depth() == 1);
  CHECK(skel.isType<uint8_t>()) << skel.info();

  const int w = static_cast<int>(skel.width());
  const int h = static_cast<int>(skel.height());
  auto* data = skel.timeData<uint8_t>(0);

  auto idxOf = [&](int x, int y) -> size_t {
    return static_cast<size_t>(x) + static_cast<size_t>(y) * static_cast<size_t>(w);
  };

  std::vector<size_t> toClear;
  toClear.reserve(skel.voxelNumber() / 16);

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t idx = idxOf(x, y);
      if (data[idx] == 0) {
        continue;
      }

      int count = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dy == 0) {
            continue;
          }
          const int nx = x + dx;
          const int ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
            continue;
          }
          if (data[idxOf(nx, ny)] > 0) {
            ++count;
          }
        }
      }

      if (count == 1) {
        toClear.push_back(idx);
      }
    }
  }

  for (size_t idx : toClear) {
    data[idx] = 0;
  }
}

void shrinkSkeletonLegacyLike(ZImg& skel, int level)
{
  for (int i = 0; i < level; ++i) {
    shrinkSkeletonOnceLegacyLike(skel);
  }
}

[[nodiscard]] std::optional<ZImg> binarizeSignalFitWorkImageLegacyLike(const ZImg& work, int option)
{
  CHECK(!work.isEmpty());
  CHECK(work.numChannels() == 1) << work.info();
  CHECK(work.numTimes() == 1) << work.info();
  CHECK(work.depth() == 1) << work.info();

  if (work.isType<uint8_t>() || work.isType<uint16_t>()) {
    const std::optional<IntHistogramLegacyLike> histOpt = imageHistogramLegacyLike(work, nullptr);
    if (!histOpt.has_value() || histOpt->empty()) {
      return std::nullopt;
    }

    int thre = 0;
    if (option == 1) {
      thre = rcthreLegacyLike(*histOpt, 0, 65535);
    } else {
      thre = triangleThresholdLegacyLike(*histOpt, 0, 65535);
    }
    return binarizeGreaterThanLegacyLike(work, thre);
  }

  return imgTypeDispatcher(work.info(), [&]<typename TVoxel>() -> std::optional<ZImg> {
    TVoxel minV{};
    TVoxel maxV{};
    work.computeMinMax(minV, maxV);

    const size_t defaultBins = work.bytesPerVoxel() > 1 ? 65536_uz : 256_uz;

    size_t nbins = defaultBins;
    if constexpr (std::is_integral_v<TVoxel>) {
      using U = std::make_unsigned_t<TVoxel>;
      const U rangeU = static_cast<U>(maxV) - static_cast<U>(minV);
      size_t numData = static_cast<size_t>(rangeU);
      if (numData != std::numeric_limits<size_t>::max()) {
        ++numData;
      }
      nbins = std::min(nbins, std::max<size_t>(numData, 1_uz));
    }

    CHECK(nbins >= 1);
    CHECK(nbins <= static_cast<size_t>(std::numeric_limits<int>::max()));

    std::vector<size_t> hist;
    if (maxV == minV) {
      hist.assign(1, work.voxelNumber());
      nbins = 1;
    } else {
      hist = work.histogram(minV, maxV, nbins);
      CHECK(hist.size() == nbins);
    }

    std::vector<int> legacyHist(nbins + 2, 0);
    legacyHist[0] = static_cast<int>(nbins);
    legacyHist[1] = 0;

    for (size_t i = 0; i < nbins; ++i) {
      const size_t c = hist[i];
      legacyHist[2 + i] = static_cast<int>(std::min(c, static_cast<size_t>(IntHistogramMaxCountLegacyLike)));
    }

    const IntHistogramLegacyLike histObj(std::move(legacyHist));

    int threBin = 0;
    if (option == 1) {
      threBin = rcthreLegacyLike(histObj, 0, 65535);
    } else {
      threBin = triangleThresholdLegacyLike(histObj, 0, 65535);
    }

    if (threBin < 0) {
      ZImgInfo outInfo = work.info();
      outInfo.setVoxelFormat<uint8_t>();
      outInfo.createDefaultDescriptions();
      ZImg out(outInfo);
      out.fill(1);
      return out;
    }

    const size_t threBinU = static_cast<size_t>(threBin);
    CHECK(threBinU < nbins);
    const double threValue = work.binRange(threBinU, minV, maxV, nbins).first;
    return work.binarized(threValue, ZImg::ThresholdMode::ExcludeThreshold);
  });
}

[[nodiscard]] size_t closestForegroundPixelLegacyLike(const ZImg& mask, double cx, double cy, double cz)
{
  CHECK(mask.numChannels() == 1);
  CHECK(mask.numTimes() == 1);
  CHECK(mask.isType<uint8_t>()) << mask.info();

  const int w = static_cast<int>(mask.width());
  const int h = static_cast<int>(mask.height());
  const int d = static_cast<int>(mask.depth());
  const auto* data = mask.timeData<uint8_t>(0);

  size_t targetIndex = 0;
  double minDist2 = cx * cx + cy * cy + cz * cz;

  size_t index = 0;
  for (int z = 0; z < d; ++z) {
    const double dz = cz - static_cast<double>(z);
    for (int y = 0; y < h; ++y) {
      const double dy = cy - static_cast<double>(y);
      for (int x = 0; x < w; ++x) {
        if (data[index] > 0) {
          const double dx = cx - static_cast<double>(x);
          const double dist2 = dx * dx + dy * dy + dz * dz;
          if (dist2 < minDist2) {
            targetIndex = index;
            minDist2 = dist2;
          }
        }
        ++index;
      }
    }
  }

  return targetIndex;
}

} // namespace

bool fitSwcNodeSignalLegacyLike(SwcNode& node, const ZImg& stack, ZNeutubeImageBackgroundLegacyLike bg, int option)
{
  if (stack.isEmpty()) {
    return false;
  }

  CHECK(stack.numChannels() == 1) << stack.info();
  CHECK(stack.numTimes() == 1) << stack.info();

  if (node.radius <= 0.0) {
    return false;
  }

  const double expandScale = 2.0;
  const double expandRadius = node.radius * expandScale + 3.0;

  int x1 = iroundLegacyLike(node.x - expandRadius);
  int y1 = iroundLegacyLike(node.y - expandRadius);
  int x2 = iroundLegacyLike(node.x + expandRadius);
  int y2 = iroundLegacyLike(node.y + expandRadius);

  x1 = std::max(x1, 0);
  y1 = std::max(y1, 0);
  x2 = std::min(x2, static_cast<int>(stack.width()) - 1);
  y2 = std::min(y2, static_cast<int>(stack.height()) - 1);

  const int cz = iroundLegacyLike(node.z);
  if (cz < 0 || cz >= static_cast<int>(stack.depth())) {
    return false;
  }

  const ZVoxelCoordinate start(static_cast<index_t>(x1), static_cast<index_t>(y1), static_cast<index_t>(cz), 0, 0);
  const ZVoxelCoordinate end(static_cast<index_t>(x2 + 1),
                             static_cast<index_t>(y2 + 1),
                             static_cast<index_t>(cz + 1),
                             1,
                             1);
  ZImg slice = stack.crop(ZImgRegion(start, end));
  if (slice.isEmpty()) {
    return false;
  }

  return fitSwcNodeSignalInCroppedSliceLegacyLike(node, slice, x1, y1, bg, option);
}

bool fitSwcNodeSignalInCroppedSliceLegacyLike(SwcNode& node,
                                              const ZImg& slice,
                                              int x0,
                                              int y0,
                                              ZNeutubeImageBackgroundLegacyLike bg,
                                              int option)
{
  if (slice.isEmpty()) {
    return false;
  }

  CHECK(slice.numChannels() == 1) << slice.info();
  CHECK(slice.numTimes() == 1) << slice.info();
  CHECK(slice.depth() == 1) << slice.info();

  ZImg work = slice;
  if (bg == ZNeutubeImageBackgroundLegacyLike::Bright) {
    invertValueInPlaceLegacyLike(work);
  }

  work = medianFilterConn8LegacyLike(work);

  const auto binaryOpt = binarizeSignalFitWorkImageLegacyLike(work, option);
  if (!binaryOpt) {
    return false;
  }
  const ZImg& binary = *binaryOpt;
  ZImg skel = bwthinLegacyLike(binary);

  ZImg dist = planarBwdistSquaredU16P(binary);
  CHECK(dist.isType<uint16_t>()) << dist.info();

  const size_t voxelNumber = dist.voxelNumber();
  auto* distData = dist.timeData<uint16_t>(0);
  const auto* skelData = skel.timeData<uint8_t>(0);
  for (size_t i = 0; i < voxelNumber; ++i) {
    if (skelData[i] == 0) {
      distData[i] = 0;
    }
  }

  shrinkSkeletonLegacyLike(skel, 3);

  const double localX = node.x - static_cast<double>(x0);
  const double localY = node.y - static_cast<double>(y0);
  const size_t index = closestForegroundPixelLegacyLike(skel, localX, localY, 0.0);

  const int w = static_cast<int>(skel.width());
  const int h = static_cast<int>(skel.height());
  const int nx = static_cast<int>(index % static_cast<size_t>(w));
  const int ny = static_cast<int>((index / static_cast<size_t>(w)) % static_cast<size_t>(h));

  const double r2 = static_cast<double>(distData[index]);
  if (r2 > 0.0) {
    const double dx = static_cast<double>(nx) - localX;
    const double dy = static_cast<double>(ny) - localY;
    const double squareDistShift = dx * dx + dy * dy;
    const double limit = (r2 + 3.0) * (r2 + 3.0);
    if (squareDistShift <= limit) {
      node.radius = std::sqrt(r2);
      node.x = static_cast<double>(nx + x0);
      node.y = static_cast<double>(ny + y0);
      return true;
    }
  }

  return false;
}

bool fitSwcNodeSignalWithFallbackLegacyLike(SwcNode& node, const ZImg& stack, ZNeutubeImageBackgroundLegacyLike bg)
{
  if (fitSwcNodeSignalLegacyLike(node, stack, bg, 1)) {
    return true;
  }
  return fitSwcNodeSignalLegacyLike(node, stack, bg, 2);
}

bool fitSwcNodeSignalWithFallbackInCroppedSliceLegacyLike(SwcNode& node,
                                                          const ZImg& slice,
                                                          int x0,
                                                          int y0,
                                                          ZNeutubeImageBackgroundLegacyLike bg)
{
  if (fitSwcNodeSignalInCroppedSliceLegacyLike(node, slice, x0, y0, bg, 1)) {
    return true;
  }
  return fitSwcNodeSignalInCroppedSliceLegacyLike(node, slice, x0, y0, bg, 2);
}

} // namespace nim
