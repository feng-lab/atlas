#include "zneutubeimgbinarizer.h"

#include "zneutubeimglocmax.h"
#include "zneutubeinthistogram.h"
#include "zneutubeneighborhood.h"
#include "zneutubeobjlabel.h"

#include "zexception.h"
#include "zlog.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <type_traits>
#include <vector>

namespace nim {

namespace {

[[nodiscard]] ZImg makeUint8VolumeLike(const ZImg& img)
{
  ZImgInfo info = img.info();
  info.setVoxelFormat<uint8_t>();
  info.createDefaultDescriptions();
  ZImg out(info);
  return out;
}

template<typename TVoxel>
[[nodiscard]] TVoxel clampIntToVoxelRangeLegacyLike(int v)
{
  if constexpr (std::is_floating_point_v<TVoxel>) {
    return static_cast<TVoxel>(v);
  } else if constexpr (std::is_signed_v<TVoxel>) {
    const std::int64_t minV = static_cast<std::int64_t>(std::numeric_limits<TVoxel>::lowest());
    const std::int64_t maxV = static_cast<std::int64_t>(std::numeric_limits<TVoxel>::max());
    const std::int64_t vv = static_cast<std::int64_t>(v);
    const std::int64_t clamped = std::clamp(vv, minV, maxV);
    return static_cast<TVoxel>(clamped);
  } else {
    if (v <= 0) {
      return static_cast<TVoxel>(0);
    }
    const std::uint64_t maxV = static_cast<std::uint64_t>(std::numeric_limits<TVoxel>::max());
    const std::uint64_t vv = static_cast<std::uint64_t>(v);
    return static_cast<TVoxel>(std::min(vv, maxV));
  }
}

[[nodiscard]] size_t fgAreaAboveThresholdLegacyLike(const ZImg& stack, int thre)
{
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  const size_t n = stack.voxelNumber();
  return imgTypeDispatcher(stack.info(), [&]<typename TVoxel>() -> size_t {
    const auto* a = stack.timeData<TVoxel>(0);
    const TVoxel t = clampIntToVoxelRangeLegacyLike<TVoxel>(thre);
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
      if (a[i] > t) {
        ++count;
      }
    }
    return count;
  });
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

  imgTypeDispatcher(in.info(), [&]<typename TVoxel>() {
    const auto* src = in.timeData<TVoxel>(0);
    const TVoxel t = clampIntToVoxelRangeLegacyLike<TVoxel>(thre);
    for (size_t i = 0; i < n; ++i) {
      dst[i] = static_cast<uint8_t>(src[i] > t);
    }
  });
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

[[nodiscard]] std::optional<IntHistogramLegacyLike>
computeLocmaxHistLegacyLike(const ZImg& stack, /*nullable*/ BinarizeLocmaxDiagnosticsLegacyLike* diag)
{
  // Port of ZStackBinarizer::computeLocmaxHist().
  constexpr int conn = 18;

  const bool collectTiming = (diag != nullptr) && diag->collectTiming;

  const auto t0 = collectTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  ZImg locmax = stackLocmaxRegionMaskLegacyLike(stack, conn);
  if (collectTiming) {
    diag->ms_locmax_region_mask =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    const auto* d = locmax.timeData<uint8_t>(0);
    diag->locmax_region_nonzero = static_cast<size_t>(std::count(d, d + locmax.voxelNumber(), uint8_t{1}));
  }
  CHECK(locmax.isType<uint8_t>());

  // Port of Stack_Label_Objects_Ns(locmax, NULL, 1, 2, 3, conn) + conversion to 0/1.
  {
    const auto t1 = collectTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    auto* a = locmax.timeData<uint8_t>(0);
    const size_t n = locmax.voxelNumber();

    const size_t width = locmax.width();
    const size_t height = locmax.height();
    const size_t depth = locmax.depth();
    const size_t plane = width * height;
    const int widthI = static_cast<int>(width);
    const int heightI = static_cast<int>(height);
    const int depthI = static_cast<int>(depth);

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

        const int z = static_cast<int>(idx / plane);
        const size_t rem = idx - static_cast<size_t>(z) * plane;
        const int y = static_cast<int>(rem / width);
        const int x = static_cast<int>(rem - static_cast<size_t>(y) * width);

        for (size_t i = 0; i < nb.size(); ++i) {
          const ZVoxelCoordinate& o = nb.offset(i);
          const int nx = x + o.x;
          const int ny = y + o.y;
          const int nz = z + o.z;
          if (nx < 0 || ny < 0 || nz < 0 || nx >= widthI || ny >= heightI || nz >= depthI) {
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

    if (collectTiming) {
      diag->ms_locmax_seed_select =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
      diag->locmax_seed_nonzero = static_cast<size_t>(std::count(a, a + n, uint8_t{1}));
    }
  }

  const auto t2 = collectTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  auto hist = imageHistogramLegacyLike(stack, &locmax);
  if (collectTiming) {
    diag->ms_locmax_hist_masked =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t2).count();
  }
  return hist;
}

} // namespace

int subtractBackgroundLegacyLike(ZImg& stack, double minFr, int maxIter)
{
  if (stack.isEmpty()) {
    return 0;
  }

  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  auto computeCommonIndexFromHistogram = [&](const std::span<const size_t> counts, int maxIterLocal) -> int {
    if (counts.empty()) {
      return 0;
    }

    const size_t totalCount = std::accumulate(counts.begin(), counts.end(), size_t{0});
    CHECK(totalCount > 0);

    int common = 0;

    const size_t darkCount = counts[0];
    if (static_cast<double>(darkCount) / static_cast<double>(totalCount) > 0.9) {
      common = 0;
      return common;
    }

    std::vector<size_t> suffix(counts.size() + 1, size_t{0});
    for (size_t i = counts.size(); i > 0; --i) {
      suffix[i - 1] = suffix[i] + counts[i - 1];
    }

    auto modeInRangeFirstMax = [&](int low, int high) -> int {
      low = std::max(low, 0);
      high = std::min(high, static_cast<int>(counts.size()) - 1);
      if (low > high) {
        return high;
      }

      int best = low;
      size_t bestCount = counts[static_cast<size_t>(low)];
      for (int v = low; v <= high; ++v) {
        const size_t c = counts[static_cast<size_t>(v)];
        if (c > bestCount) {
          bestCount = c;
          best = v;
        }
      }
      return best;
    };

    const int maxV = static_cast<int>(counts.size()) - 1;
    for (int iter = 0; iter < maxIterLocal; ++iter) {
      const int mode = modeInRangeFirstMax(common + 1, maxV);
      if (mode == maxV) {
        break;
      }
      common = mode;

      const double fgRatio =
        static_cast<double>(suffix[static_cast<size_t>(common) + 1]) / static_cast<double>(totalCount);
      if (fgRatio < minFr) {
        break;
      }
    }

    return common;
  };

  // Fast path for legacy tracing inputs (uint8/uint16). Legacy neuTube uses exact-intensity histograms for these types.
  if (stack.isType<uint8_t>()) {
    constexpr size_t kBins = 256;
    std::array<size_t, kBins> counts{};
    const size_t n = stack.voxelNumber();
    const auto* data = stack.timeData<uint8_t>(0);
    for (size_t i = 0; i < n; ++i) {
      ++counts[data[i]];
    }

    const int commonIntensity =
      computeCommonIndexFromHistogram(std::span<const size_t>(counts.data(), counts.size()), maxIter);
    if (commonIntensity > 0) {
      stack -= commonIntensity;
    }
    return commonIntensity;
  }

  if (stack.isType<uint16_t>()) {
    constexpr size_t kBins = 65536;
    std::vector<size_t> counts(kBins, size_t{0});
    const size_t n = stack.voxelNumber();
    const auto* data = stack.timeData<uint16_t>(0);
    for (size_t i = 0; i < n; ++i) {
      ++counts[data[i]];
    }

    const int commonIntensity = computeCommonIndexFromHistogram(counts, maxIter);
    if (commonIntensity > 0) {
      stack -= commonIntensity;
    }
    return commonIntensity;
  }

  // General path for other voxel types:
  // - Build a binned histogram over the current data range.
  // - Run the legacy threshold-selection logic on the histogram bins.
  // - Convert the chosen bin back into a meaningful intensity value and subtract it.
  //
  // This avoids UB from `static_cast<int>(float)` on out-of-range floats and avoids giant `max-min` allocations for
  // wide-range integer types.
  return imgTypeDispatcher(stack.info(), [&]<typename TVoxel>() -> int {
    TVoxel minV{};
    TVoxel maxV{};
    stack.computeMinMax(minV, maxV);

    const size_t defaultBins = stack.bytesPerVoxel() > 1 ? 65536_uz : 256_uz;

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

    std::vector<size_t> counts;
    if (maxV == minV) {
      counts.assign(1, stack.voxelNumber());
      nbins = 1;
    } else {
      counts = stack.histogram(minV, maxV, nbins);
      CHECK(counts.size() == nbins);
    }

    const int commonBin = computeCommonIndexFromHistogram(counts, maxIter);
    if (commonBin <= 0) {
      return 0;
    }

    const size_t commonBinU = static_cast<size_t>(commonBin);
    CHECK(commonBinU < nbins);
    const double commonValue = stack.binRange(commonBinU, minV, maxV, nbins).first;
    stack -= commonValue;

    return static_cast<int>(std::lrint(commonValue));
  });
}

BinarizeResultLegacyLike
binarizeLocmaxLegacyLike(const ZImg& stack, int retryCount, BinarizeLocmaxDiagnosticsLegacyLike* diag)
{
  BinarizeResultLegacyLike res;

  if (stack.isEmpty()) {
    return res;
  }

  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  const size_t voxelNumber = stack.voxelNumber();

  const bool collectTiming = (diag != nullptr) && diag->collectTiming;

  const auto t0 = collectTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  const auto histOpt = imageHistogramLegacyLike(stack, nullptr);
  if (collectTiming) {
    diag->ms_hist_full =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  }
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
  const auto t1 = collectTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  const auto locHistOpt = computeLocmaxHistLegacyLike(stack, diag);
  if (collectTiming) {
    diag->ms_locmax_hist =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
  }
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

  {
    const auto t2 = collectTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    if (static_cast<double>(locHist.sum()) / static_cast<double>(voxelNumber) <= 1e-5) {
      threshold = hist.mode(0, high);
    } else {
      threshold = triangleThresholdLegacyLike(locHist, low, high - 1);
      threshold = refineLocmaxThresholdLegacyLike(stack, threshold, locHist, high, retryCount);
    }

    if (collectTiming) {
      diag->ms_threshold =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t2).count();
    }
  }

  res.actualThreshold = threshold;
  {
    const auto t3 = collectTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    thresholdBinarizeToUint8LegacyLike(stack, threshold, res.binary);
    if (collectTiming) {
      diag->ms_threshold_binarize =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t3).count();
    }
  }
  res.success = true;
  return res;
}

} // namespace nim
