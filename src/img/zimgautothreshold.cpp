#include "zimgautothreshold.h"

#include "zcancellation.h"
#include "zimgconnectedcomponents.h"
#include "zimgregionalextrema.h"
#include "zbenchtimer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace nim {

namespace {

struct IntHistogram
{
  int minValue = 0;
  std::vector<int> counts;

  [[nodiscard]] bool empty() const
  {
    return counts.empty();
  }

  [[nodiscard]] int maxValue() const
  {
    CHECK(!counts.empty());
    return minValue + static_cast<int>(counts.size()) - 1;
  }

  [[nodiscard]] int sum() const
  {
    int total = 0;
    for (const int c : counts) {
      total += c;
    }
    return total;
  }

  [[nodiscard]] int count(int v) const
  {
    if (counts.empty()) {
      return 0;
    }
    if (v < minValue || v > maxValue()) {
      return 0;
    }
    return counts[static_cast<size_t>(v - minValue)];
  }

  [[nodiscard]] int mode(int minV, int maxV) const
  {
    CHECK(!counts.empty());

    minV = std::max(minV, minValue);
    maxV = std::min(maxV, maxValue());

    int m = minV;
    int maxCount = count(m);
    for (int v = minV; v <= maxV; ++v) {
      const int c = count(v);
      if (c > maxCount) {
        maxCount = c;
        m = v;
      }
    }
    return m;
  }

  [[nodiscard]] int upperCount(int v) const
  {
    CHECK(!counts.empty());

    int c = 0;
    const int minV = std::max(v, minValue);
    const int maxV = maxValue();
    for (int i = minV; i <= maxV; ++i) {
      c += count(i);
    }
    return c;
  }
};

template<typename TVoxel>
[[nodiscard]] std::optional<std::pair<int, int>> maskedMinMax(const TVoxel* a, const uint8_t* mask, size_t length)
{
  CHECK(a != nullptr);
  CHECK(length > 0);

  size_t minIdx = length;
  size_t maxIdx = length;

  if (mask == nullptr) {
    minIdx = 0;
    maxIdx = 0;
    for (size_t i = 1; i < length; ++i) {
      if (a[i] < a[minIdx]) {
        minIdx = i;
      }
      if (a[i] > a[maxIdx]) {
        maxIdx = i;
      }
    }
  } else {
    for (size_t i = 0; i < length; ++i) {
      if (mask[i] == 1) {
        minIdx = i;
        maxIdx = i;
        break;
      }
    }
    if (minIdx == length) {
      return std::nullopt;
    }

    for (size_t i = minIdx + 1; i < length; ++i) {
      if (mask[i] != 1) {
        continue;
      }
      if (a[i] < a[minIdx]) {
        minIdx = i;
      }
      if (a[i] > a[maxIdx]) {
        maxIdx = i;
      }
    }
  }

  return std::pair<int, int>(static_cast<int>(a[minIdx]), static_cast<int>(a[maxIdx]));
}

[[nodiscard]] std::optional<IntHistogram>
imageHistogramUintNeuTube(const ZImg& img, const ZImg* mask, const folly::CancellationToken& cancellationToken)
{
  if (img.isEmpty()) {
    throw ZException("imageHistogramUintNeuTube: empty image.");
  }

  CHECK(img.numChannels() == 1);
  CHECK(img.numTimes() == 1);

  const uint8_t* maskData = nullptr;
  if (mask != nullptr) {
    if (mask->isEmpty()) {
      return std::nullopt;
    }
    CHECK(mask->numChannels() == 1);
    CHECK(mask->numTimes() == 1);
    CHECK(mask->width() == img.width() && mask->height() == img.height() && mask->depth() == img.depth());
    CHECK(mask->isType<uint8_t>());
    maskData = mask->timeData<uint8_t>(0);
  }

  maybeCancel(cancellationToken);

  const size_t n = img.voxelNumber();
  if (n == 0) {
    return std::nullopt;
  }

  std::optional<std::pair<int, int>> minMax;

  if (img.isType<uint8_t>()) {
    minMax = maskedMinMax<uint8_t>(img.timeData<uint8_t>(0), maskData, n);
  } else if (img.isType<uint16_t>()) {
    minMax = maskedMinMax<uint16_t>(img.timeData<uint16_t>(0), maskData, n);
  } else {
    throw ZException(fmt::format("imageHistogramUintNeuTube: unsupported voxel type {}", img.info()));
  }

  if (!minMax) {
    return std::nullopt;
  }

  const int minV = minMax->first;
  const int maxV = minMax->second;
  CHECK(minV <= maxV);

  IntHistogram hist;
  hist.minValue = minV;
  hist.counts.assign(static_cast<size_t>(maxV - minV + 1), 0);

  auto addValue = [&](int v) {
    hist.counts[static_cast<size_t>(v - minV)] += 1;
  };

  if (img.isType<uint8_t>()) {
    const auto* a = img.timeData<uint8_t>(0);
    if (maskData == nullptr) {
      for (size_t i = 0; i < n; ++i) {
        if ((i % 4096) == 0) {
          maybeCancel(cancellationToken);
        }
        addValue(static_cast<int>(a[i]));
      }
    } else {
      for (size_t i = 0; i < n; ++i) {
        if ((i % 4096) == 0) {
          maybeCancel(cancellationToken);
        }
        if (maskData[i] == 1) {
          addValue(static_cast<int>(a[i]));
        }
      }
    }
  } else {
    const auto* a = img.timeData<uint16_t>(0);
    if (maskData == nullptr) {
      for (size_t i = 0; i < n; ++i) {
        if ((i % 4096) == 0) {
          maybeCancel(cancellationToken);
        }
        addValue(static_cast<int>(a[i]));
      }
    } else {
      for (size_t i = 0; i < n; ++i) {
        if ((i % 4096) == 0) {
          maybeCancel(cancellationToken);
        }
        if (maskData[i] == 1) {
          addValue(static_cast<int>(a[i]));
        }
      }
    }
  }

  return hist;
}

[[nodiscard]] int iarrayMaxFirst(const int* a, size_t length, size_t* idx)
{
  CHECK(a != nullptr);
  CHECK(length > 0);

  size_t maxIdx = 0;
  for (size_t i = 1; i < length; ++i) {
    if (a[i] > a[maxIdx]) {
      maxIdx = i;
    }
  }
  if (idx != nullptr) {
    *idx = maxIdx;
  }
  return a[maxIdx];
}

[[nodiscard]] int iarrayMinMaskedLastTies(const int* a, size_t length, const int* mask, size_t* idx)
{
  CHECK(a != nullptr);
  CHECK(mask != nullptr);

  size_t minIdx = length;
  for (size_t i = length; i > 0; --i) {
    if (mask[i - 1] != 0) {
      minIdx = i - 1;
      break;
    }
  }

  if (minIdx < length) {
    for (size_t i = minIdx + 1; i > 0; --i) {
      if (mask[i - 1] != 0) {
        if (a[i - 1] < a[minIdx]) {
          minIdx = i - 1;
        }
      }
    }
  }

  if (idx != nullptr) {
    *idx = minIdx;
  }

  if (minIdx >= length) {
    return 0;
  }
  return a[minIdx];
}

[[nodiscard]] int triangleThresholdNeuTube(const IntHistogram& hist, int low, int high)
{
  if (hist.empty()) {
    throw ZException("triangleThresholdNeuTube: empty histogram.");
  }

  int minGrey = hist.minValue;
  int maxGrey = hist.maxValue();

  const int* hist2 = hist.counts.data();
  CHECK(hist2 != nullptr);

  if (minGrey < low) {
    hist2 += low - minGrey;
    minGrey = low;
  }

  if (maxGrey > high) {
    maxGrey = high;
  }

  int length = maxGrey - minGrey + 1;
  CHECK(length >= 1);

  size_t maxIndex = 0;
  (void)iarrayMaxFirst(hist2, static_cast<size_t>(length), &maxIndex);

  size_t minIndex = 0;
  (void)iarrayMinMaskedLastTies(hist2 + maxIndex, static_cast<size_t>(length) - maxIndex, hist2 + maxIndex, &minIndex);
  minIndex += maxIndex;

  if (maxIndex == minIndex) {
    return minGrey - 1;
  }

  length = static_cast<int>(minIndex - maxIndex + 1);
  hist2 += maxIndex;

  std::vector<double> dhist(static_cast<size_t>(length));
  const double denom = static_cast<double>(hist2[0] - hist2[static_cast<size_t>(length) - 1]);
  CHECK(denom != 0.0);
  const double normFactor = static_cast<double>(length - 1) / denom;
  for (int i = 0; i < length; ++i) {
    dhist[static_cast<size_t>(i)] = static_cast<double>(hist2[i] - hist2[static_cast<size_t>(length) - 1]) * normFactor;
  }

  int thre = 0;
  double bestScore = dhist[0];

  for (int i = 1; i < length; ++i) {
    if (hist2[i] > 0) {
      const double score = dhist[static_cast<size_t>(i)] + static_cast<double>(i);
      if (score < bestScore) {
        bestScore = score;
        thre = i;
      }
    }
  }

  thre += static_cast<int>(maxIndex) + minGrey;
  return thre;
}

[[nodiscard]] size_t fgAreaAboveThresholdNeuTube(const ZImg& stack, int thre)
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

  throw ZException(fmt::format("fgAreaAboveThresholdNeuTube: unsupported voxel type {}", stack.info()));
}

[[nodiscard]] int refineLocmaxThresholdNeuTube(const ZImg& stack,
                                               int thre,
                                               const IntHistogram& hist,
                                               int upperBound,
                                               int retryCount,
                                               const folly::CancellationToken& cancellationToken)
{
  constexpr double ratioLowThre = 0.015;
  constexpr double ratioThre = 0.05;

  maybeCancel(cancellationToken);

  const double voxelNumber = static_cast<double>(stack.voxelNumber());
  const double fgratio = static_cast<double>(fgAreaAboveThresholdNeuTube(stack, thre)) / voxelNumber;

  double fgratio2 = fgratio;
  double prevFgratio = fgratio;

  if ((fgratio > ratioLowThre) && (fgratio <= ratioThre)) {
    const int thre2 = triangleThresholdNeuTube(hist, thre + 1, upperBound - 1);
    fgratio2 = static_cast<double>(fgAreaAboveThresholdNeuTube(stack, thre2)) / voxelNumber;
    if (fgratio2 / fgratio <= 0.3) {
      thre = thre2;
    }
  } else {
    int nretry = retryCount;
    int thre2 = thre;
    while (fgratio2 > ratioThre) {
      maybeCancel(cancellationToken);

      thre2 = triangleThresholdNeuTube(hist, thre2 + 1, upperBound - 1);
      fgratio2 = static_cast<double>(fgAreaAboveThresholdNeuTube(stack, thre2)) / voxelNumber;
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

[[nodiscard]] std::optional<IntHistogram> computeLocmaxHistNeuTube(const ZImg& stack,
                                                                   const folly::CancellationToken& cancellationToken)
{
  constexpr size_t conn = 18;

  ZImgRegionalExtrema regionalExtrema;
  regionalExtrema.setCancellationToken(cancellationToken);
  ZImg locmaxMask = regionalExtrema.regionalMax(stack, conn);
  if (locmaxMask.isEmpty()) {
    return std::nullopt;
  }
  CHECK(locmaxMask.isType<uint8_t>());

  ZImgConnectedComponents conncomp;
  conncomp.setCancellationToken(cancellationToken);
  ConnComp cc = conncomp.runLabelModifyInput(locmaxMask, conn);

  locmaxMask.fill(0);
  auto* locmaxMaskData = locmaxMask.timeData<uint8_t>(0);
  for (const auto& vidx : cc.voxelIdxList) {
    if (!vidx.empty()) {
      locmaxMaskData[vidx[0]] = 1;
    }
  }

  return imageHistogramUintNeuTube(stack, &locmaxMask, cancellationToken);
}

} // namespace

template<typename TVoxel>
TVoxel ZImgAutoThreshold::typedTriangleThre(const ZImg& imgIn, size_t c, size_t t)
{
  if (!imgIn.isType<TVoxel>()) {
    throw ZException("input img voxel type doesnot match provided type");
  }

  size_t conn = 18;
  ZImg img = imgIn.createView(c, t);

  this->clearRegisteredSubOperations();
  this->setTotalSubOperationWeight(.8);

  // ZBenchTimer bt;
  // bt.start();
  ZImgRegionalExtrema regionalExtrema;
  this->registerSubOperation(&regionalExtrema, .4);
  ZImg locmaxMask = regionalExtrema.regionalMax(img, conn);
  // bt.stop();
  // LOG(INFO) << bt;

  // bt.reset();
  // bt.start();
  ZImgConnectedComponents conncomp;
  this->registerSubOperation(&conncomp, .4);
  ConnComp CC = conncomp.runLabelModifyInput(locmaxMask, conn);
  // bt.stop();
  // LOG(INFO) << bt;

  locmaxMask.fill(0);
  auto locmaxMaskData = locmaxMask.timeData<uint8_t>(0);
  for (auto& vidx : CC.voxelIdxList) {
    locmaxMaskData[vidx[0]] = 1;
  }
  CC.voxelIdxList.clear();

  std::vector<size_t> hist = img.histogram(0, locmaxMask);
  locmaxMask.clear();

  this->reportProgress(.8);

  size_t low, high;
  histNonZeroRange(hist, low, high);

  if (low == high) { // flat img, return smallest possible value as threshold
    this->reportProgress(1.0);
    return img.dataRangeMin<TVoxel>();
  }

  // search in range [low, high-1]
  // maximum position
  size_t maxIndex = low;
  for (size_t i = low + 1; i < high; ++i) {
    if (hist[i] > hist[maxIndex]) {
      maxIndex = i;
    }
  }
  // minimum position
  size_t minIndex = high - 1;
  while (minIndex > maxIndex && hist[minIndex] == 0) {
    --minIndex;
  }

  for (size_t i = minIndex; i-- > maxIndex;) {
    if (hist[i] && hist[i] < hist[minIndex]) {
      minIndex = i;
    }
  }

  if (maxIndex == minIndex) {
    this->reportProgress(1.0);
    return saturate_cast<TVoxel>(img.binRange(low - 1, hist.size()).first);
  }

  // normalize the histogram
  std::vector<double> nhist(minIndex - maxIndex + 1);
  double scale = (nhist.size() - 1.0) / (hist[maxIndex] * 1.0 - hist[minIndex]);
  for (size_t i = 0; i < nhist.size(); ++i) {
    nhist[i] = (hist[i + maxIndex] * 1.0 - hist[minIndex]) * scale;
  }

  size_t threBin = 0;
  double bestScore = nhist[0];

  for (size_t i = 1; i < nhist.size(); ++i) {
    if (nhist[i] > 0) {
      double score = nhist[i] + i;
      if (score < bestScore) {
        bestScore = score;
        threBin = i;
      }
    }
  }
  threBin += maxIndex;

  this->reportProgress(1.0);
  return saturate_cast<TVoxel>(img.binRange(threBin, hist.size()).first);
}

std::optional<int> ZImgAutoThreshold::locmaxThreNeuTube(const ZImg& img, size_t c, size_t t, int retryCount)
{
  if (img.isEmpty()) {
    return std::nullopt;
  }
  if (retryCount <= 0) {
    throw ZException("locmaxThreNeuTube: retryCount must be positive.");
  }

  ZImg stack = img.createView(static_cast<index_t>(c), static_cast<index_t>(t));
  CHECK(stack.numChannels() == 1);
  CHECK(stack.numTimes() == 1);

  if (!stack.isType<uint8_t>() && !stack.isType<uint16_t>()) {
    throw ZException(fmt::format("locmaxThreNeuTube: unsupported voxel type {}", stack.info()));
  }

  maybeCancel(m_cancellationToken);

  const size_t voxelNumber = stack.voxelNumber();

  const auto histOpt = imageHistogramUintNeuTube(stack, nullptr, m_cancellationToken);
  if (!histOpt) {
    return std::nullopt;
  }
  const IntHistogram& hist = *histOpt;

  const int minV = hist.minValue;
  const int maxV = hist.maxValue();

  if (minV == maxV) {
    return std::nullopt;
  }

  if (hist.count(minV) + hist.count(maxV) == static_cast<int>(voxelNumber)) {
    return minV;
  }

  const auto locHistOpt = computeLocmaxHistNeuTube(stack, m_cancellationToken);
  if (!locHistOpt) {
    return std::nullopt;
  }
  const IntHistogram& locHist = *locHistOpt;

  const int low = locHist.minValue;
  const int high = locHist.maxValue();
  if (low == high) {
    return std::nullopt;
  }

  int threshold = 0;

  if (static_cast<double>(locHist.sum()) / static_cast<double>(voxelNumber) <= 1e-5) {
    threshold = hist.mode(0, high);
  } else {
    threshold = triangleThresholdNeuTube(locHist, low, high - 1);
    threshold = refineLocmaxThresholdNeuTube(stack, threshold, locHist, high, retryCount, m_cancellationToken);
  }

  return threshold;
}

template<typename TVoxel>
TVoxel ZImgAutoThreshold::typedTriangleThre(const QString& filename,
                                            size_t c,
                                            size_t t,
                                            size_t scene,
                                            const std::vector<ZVoxelCoordinate>& mask)
{
  std::vector<ZImgInfo> infos = ZImg::readImgInfos(filename);
  if (scene >= infos.size()) {
    throw ZException("input scene incorrect");
  }
  if (!infos[scene].isType<TVoxel>()) {
    throw ZException("input img voxel type doesnot match provided type");
  }

  size_t conn = 18;

  this->clearRegisteredSubOperations();

  std::vector<size_t> hist;

  std::vector<ZImgRegion> nonexpandRegions;
  std::vector<ZImgRegion> rgns = ZImgRegion::splitBigImage(infos[scene], nonexpandRegions, 4096, 256, c, t);
  for (size_t rgni = 0; rgni < rgns.size(); ++rgni) {
    const ZImgRegion& rgn = rgns[rgni];
    const ZImgRegion& validRgn = nonexpandRegions[rgni];
    ZImg img(filename, rgn, scene);
    if (!mask.empty()) {
      ZImg tmpImg(img.info());
      for (auto coord : mask) {
        if (rgn.xInRegion(coord.x) && rgn.yInRegion(coord.y)) {
          coord -= rgn.start;
          tmpImg.setValue(img.value(coord), coord);
        }
      }
      img.swap(tmpImg);
    }

    // ZBenchTimer bt;
    // bt.start();
    ZImgRegionalExtrema regionalExtrema;
    ZImg locmaxMask = regionalExtrema.regionalMax(img, conn);
    // bt.stop();
    // LOG(INFO) << bt;

    // bt.reset();
    // bt.start();
    ZImgConnectedComponents conncomp;
    ConnComp CC = conncomp.runLabelModifyInput(locmaxMask, conn);
    // bt.stop();
    // LOG(INFO) << bt;

    locmaxMask.fill(0);
    auto locmaxMaskData = locmaxMask.timeData<uint8_t>(0);
    for (auto& vIdx : CC.voxelIdxList) {
      auto index = vIdx[0];
      ZVoxelCoordinate coord = img.indexToCoord(index);
      if (validRgn.xInRegion(coord.x + rgn.xStart()) && validRgn.yInRegion(coord.y + rgn.yStart())) {
        locmaxMaskData[index] = 1;
      }
    }
    CC.voxelIdxList.clear();

    std::vector<size_t> blockHist = img.histogram(0, locmaxMask);

    if (hist.empty()) {
      hist = blockHist;
    } else {
      for (size_t hi = 0; hi < hist.size(); ++hi) {
        hist[hi] += blockHist[hi];
      }
    }

    this->reportProgress(.75 * (rgni + 1) / rgns.size());
  }

  this->reportProgress(.8);

  size_t low, high;
  histNonZeroRange(hist, low, high);

  if (low == high) { // flat img, return smallest possible value as threshold
    this->reportProgress(1.0);
    return infos[scene].dataRangeMin<TVoxel>();
  }

  // search in range [low, high-1]
  // maximum position
  size_t maxIndex = low;
  for (size_t i = low + 1; i < high; ++i) {
    if (hist[i] > hist[maxIndex]) {
      maxIndex = i;
    }
  }
  // minimum position
  size_t minIndex = high - 1;
  while (minIndex > maxIndex && hist[minIndex] == 0) {
    --minIndex;
  }

  for (size_t i = minIndex; i-- > maxIndex;) {
    if (hist[i] && hist[i] < hist[minIndex]) {
      minIndex = i;
    }
  }

  if (maxIndex == minIndex) {
    this->reportProgress(1.0);
    return saturate_cast<TVoxel>(infos[scene].binRange(low - 1, hist.size()).first);
  }

  // normalize the histogram
  std::vector<double> nhist(minIndex - maxIndex + 1);
  double scale = (nhist.size() - 1.0) / (hist[maxIndex] * 1.0 - hist[minIndex]);
  for (size_t i = 0; i < nhist.size(); ++i) {
    nhist[i] = (hist[i + maxIndex] - hist[minIndex]) * scale;
  }

  size_t threBin = 0;
  double bestScore = nhist[0];

  for (size_t i = 1; i < nhist.size(); ++i) {
    if (nhist[i] > 0) {
      double score = nhist[i] + i;
      if (score < bestScore) {
        bestScore = score;
        threBin = i;
      }
    }
  }
  threBin += maxIndex;

  this->reportProgress(1.0);
  return saturate_cast<TVoxel>(infos[scene].binRange(threBin, hist.size()).first);
}

uint8_t ZImgAutoThreshold::u8TriangleThre(const QString& filename,
                                          double minValue,
                                          double maxValue,
                                          size_t c,
                                          size_t t,
                                          size_t scene,
                                          const std::vector<ZVoxelCoordinate>& mask)
{
  std::vector<ZImgInfo> infos = ZImg::readImgInfos(filename);
  if (scene >= infos.size()) {
    throw ZException("input scene incorrect");
  }
  //  if (!infos[scene].isType<uint8_t>()) {
  //    throw ZException("input img voxel type is not uint8_t");
  //  }

  size_t conn = 18;

  this->clearRegisteredSubOperations();

  std::vector<size_t> hist;

  std::vector<ZImgRegion> nonexpandRegions;
  std::vector<ZImgRegion> rgns = ZImgRegion::splitBigImage(infos[scene], nonexpandRegions, 4096, 256, c, t);
  for (size_t rgni = 0; rgni < rgns.size(); ++rgni) {
    const ZImgRegion& rgn = rgns[rgni];
    const ZImgRegion& validRgn = nonexpandRegions[rgni];
    ZImg img(filename, rgn, scene);
    if (!img.isType<uint8_t>()) {
      img = img.convertTo<uint8_t>(minValue, maxValue);
    }
    if (!mask.empty()) {
      ZImg tmpImg(img.info());
      for (auto coord : mask) {
        if (rgn.xInRegion(coord.x) && rgn.yInRegion(coord.y)) {
          coord -= rgn.start;
          tmpImg.setValue(img.value(coord), coord);
        }
      }
      img.swap(tmpImg);
    }

    // ZBenchTimer bt;
    // bt.start();
    ZImgRegionalExtrema regionalExtrema;
    ZImg locmaxMask = regionalExtrema.regionalMax(img, conn);
    // bt.stop();
    // LOG(INFO) << bt;

    // bt.reset();
    // bt.start();
    ZImgConnectedComponents conncomp;
    ConnComp CC = conncomp.runLabelModifyInput(locmaxMask, conn);
    // bt.stop();
    // LOG(INFO) << bt;

    locmaxMask.fill(0);
    auto locmaxMaskData = locmaxMask.timeData<uint8_t>(0);
    for (auto& vIdx : CC.voxelIdxList) {
      auto index = vIdx[0];
      ZVoxelCoordinate coord = img.indexToCoord(index);
      if (validRgn.xInRegion(coord.x + rgn.xStart()) && validRgn.yInRegion(coord.y + rgn.yStart())) {
        locmaxMaskData[index] = 1;
      }
    }
    CC.voxelIdxList.clear();

    std::vector<size_t> blockHist = img.histogram(0, locmaxMask);

    if (hist.empty()) {
      hist = blockHist;
    } else {
      for (size_t hi = 0; hi < hist.size(); ++hi) {
        hist[hi] += blockHist[hi];
      }
    }

    this->reportProgress(.75 * (rgni + 1) / rgns.size());
  }

  this->reportProgress(.8);

  size_t low, high;
  histNonZeroRange(hist, low, high);

  if (low == high) { // flat img, return smallest possible value as threshold
    this->reportProgress(1.0);
    return infos[scene].dataRangeMin<uint8_t>();
  }

  // search in range [low, high-1]
  // maximum position
  size_t maxIndex = low;
  for (size_t i = low + 1; i < high; ++i) {
    if (hist[i] > hist[maxIndex]) {
      maxIndex = i;
    }
  }
  // minimum position
  size_t minIndex = high - 1;
  while (minIndex > maxIndex && hist[minIndex] == 0) {
    --minIndex;
  }

  for (size_t i = minIndex; i-- > maxIndex;) {
    if (hist[i] && hist[i] < hist[minIndex]) {
      minIndex = i;
    }
  }

  if (maxIndex == minIndex) {
    this->reportProgress(1.0);
    return saturate_cast<uint8_t>(infos[scene].binRange(low - 1, hist.size()).first);
  }

  // normalize the histogram
  std::vector<double> nhist(minIndex - maxIndex + 1);
  double scale = (nhist.size() - 1.0) / (hist[maxIndex] * 1.0 - hist[minIndex]);
  for (size_t i = 0; i < nhist.size(); ++i) {
    nhist[i] = (hist[i + maxIndex] - hist[minIndex]) * scale;
  }

  size_t threBin = 0;
  double bestScore = nhist[0];

  for (size_t i = 1; i < nhist.size(); ++i) {
    if (nhist[i] > 0) {
      double score = nhist[i] + i;
      if (score < bestScore) {
        bestScore = score;
        threBin = i;
      }
    }
  }
  threBin += maxIndex;

  this->reportProgress(1.0);
  return saturate_cast<uint8_t>(infos[scene].binRange(threBin, hist.size()).first);
}

template<typename TVoxel>
TVoxel ZImgAutoThreshold::typedCentroidThre(double& cent1, double& cent2, const ZImg& imgIn, size_t c, size_t t)
{
  if (!imgIn.isType<TVoxel>()) {
    throw ZException("input img voxel type doesnot match provided type");
  }

  ZImg img = imgIn.createView(c, t);
  std::vector<size_t> hist = img.histogram();

  this->reportProgress(.8);

  size_t low, high;
  histNonZeroRange(hist, low, high);

  if (low == high) { // flat img, return smallest possible value as threshold
    this->reportProgress(1.0);
    cent1 = 0;
    cent2 = low;
    std::pair<double, double> cent1Range = img.binRange(std::floor(cent1), hist.size());
    cent1 = cent1Range.first + (cent1Range.second - cent1Range.first) * (cent1 - std::floor(cent1));
    std::pair<double, double> cent2Range = img.binRange(std::floor(cent2), hist.size());
    cent2 = cent2Range.first + (cent2Range.second - cent2Range.first) * (cent2 - std::floor(cent2));
    return img.dataRangeMin<TVoxel>();
  }

  size_t threBin = (low + high) / 2;

  std::vector<size_t> weightPos = hist;
  for (size_t i = low; i <= high; ++i) {
    weightPos[i] *= i;
  }

  size_t prevThreBin;
  do {
    if (threBin == high) {
      break;
    }

    prevThreBin = threBin;

    double totalPos = std::accumulate(weightPos.begin() + low, weightPos.begin() + threBin + 1, .0);
    size_t totalWeight = std::accumulate(hist.begin() + low, hist.begin() + threBin + 1, 0_uz);
    if (totalWeight == 0) {
      cent1 = (threBin + low) / 2.0;
    } else {
      cent1 = totalPos / totalWeight;
    }

    totalPos = std::accumulate(weightPos.begin() + threBin + 1, weightPos.begin() + high + 1, .0);
    totalWeight = std::accumulate(hist.begin() + threBin + 1, hist.begin() + high + 1, 0_uz);
    if (totalWeight == 0) {
      cent2 = (high + threBin) / 2.0;
    } else {
      cent2 = totalPos / totalWeight;
    }

    threBin = (cent1 + cent2) / 2;
  } while (threBin != prevThreBin);

  std::pair<double, double> cent1Range = img.binRange(std::floor(cent1), hist.size());
  cent1 = cent1Range.first + (cent1Range.second - cent1Range.first) * (cent1 - std::floor(cent1));
  std::pair<double, double> cent2Range = img.binRange(std::floor(cent2), hist.size());
  cent2 = cent2Range.first + (cent2Range.second - cent2Range.first) * (cent2 - std::floor(cent2));
  this->reportProgress(1.0);
  return saturate_cast<TVoxel>(img.binRange(threBin, hist.size()).first);
}

template<typename TVoxel>
TVoxel ZImgAutoThreshold::typedMaxHistThre(const ZImg& imgIn, size_t c, size_t t)
{
  if (!imgIn.isType<TVoxel>()) {
    throw ZException("input img voxel type doesnot match provided type");
  }

  ZImg img = imgIn.createView(c, t);
  std::vector<size_t> hist = img.histogram();

  this->reportProgress(.8);

  size_t low, high;
  histNonZeroRange(hist, low, high);

  if (low + 2 > high) { // two few elements, return smallest possible value as threshold
    this->reportProgress(1.0);
    return img.dataRangeMin<TVoxel>();
  }

  size_t length = high - low + 1;

  size_t searchLimit = length / 2; // will be at lease 1
  size_t maxBinIdx = std::max_element(hist.begin() + low, hist.begin() + low + searchLimit) - hist.begin();
  this->reportProgress(1.0);
  return saturate_cast<TVoxel>(img.binRange(maxBinIdx + 1, hist.size()).first);
}

// assume hist is not empty
void ZImgAutoThreshold::histNonZeroRange(std::vector<size_t>& hist, size_t& low, size_t& high)
{
  low = 0;
  while (low < hist.size() - 1 && hist[low] == 0) {
    low++;
  }

  high = hist.size() - 1;
  while (high > low && hist[high] == 0) {
    high--;
  }
}

template uint8_t ZImgAutoThreshold::typedTriangleThre<uint8_t>(const ZImg&, size_t, size_t);
template uint16_t ZImgAutoThreshold::typedTriangleThre<uint16_t>(const ZImg&, size_t, size_t);
template uint32_t ZImgAutoThreshold::typedTriangleThre<uint32_t>(const ZImg&, size_t, size_t);
template uint64_t ZImgAutoThreshold::typedTriangleThre<uint64_t>(const ZImg&, size_t, size_t);
template int8_t ZImgAutoThreshold::typedTriangleThre<int8_t>(const ZImg&, size_t, size_t);
template int16_t ZImgAutoThreshold::typedTriangleThre<int16_t>(const ZImg&, size_t, size_t);
template int32_t ZImgAutoThreshold::typedTriangleThre<int32_t>(const ZImg&, size_t, size_t);
template int64_t ZImgAutoThreshold::typedTriangleThre<int64_t>(const ZImg&, size_t, size_t);
template float ZImgAutoThreshold::typedTriangleThre<float>(const ZImg&, size_t, size_t);
template double ZImgAutoThreshold::typedTriangleThre<double>(const ZImg&, size_t, size_t);

template uint8_t ZImgAutoThreshold::typedTriangleThre<uint8_t>(const QString&,
                                                               size_t,
                                                               size_t,
                                                               size_t,
                                                               const std::vector<ZVoxelCoordinate>&);
template uint16_t ZImgAutoThreshold::typedTriangleThre<uint16_t>(const QString&,
                                                                 size_t,
                                                                 size_t,
                                                                 size_t,
                                                                 const std::vector<ZVoxelCoordinate>&);
template uint32_t ZImgAutoThreshold::typedTriangleThre<uint32_t>(const QString&,
                                                                 size_t,
                                                                 size_t,
                                                                 size_t,
                                                                 const std::vector<ZVoxelCoordinate>&);
template uint64_t ZImgAutoThreshold::typedTriangleThre<uint64_t>(const QString&,
                                                                 size_t,
                                                                 size_t,
                                                                 size_t,
                                                                 const std::vector<ZVoxelCoordinate>&);
template int8_t ZImgAutoThreshold::typedTriangleThre<int8_t>(const QString&,
                                                             size_t,
                                                             size_t,
                                                             size_t,
                                                             const std::vector<ZVoxelCoordinate>&);
template int16_t ZImgAutoThreshold::typedTriangleThre<int16_t>(const QString&,
                                                               size_t,
                                                               size_t,
                                                               size_t,
                                                               const std::vector<ZVoxelCoordinate>&);
template int32_t ZImgAutoThreshold::typedTriangleThre<int32_t>(const QString&,
                                                               size_t,
                                                               size_t,
                                                               size_t,
                                                               const std::vector<ZVoxelCoordinate>&);
template int64_t ZImgAutoThreshold::typedTriangleThre<int64_t>(const QString&,
                                                               size_t,
                                                               size_t,
                                                               size_t,
                                                               const std::vector<ZVoxelCoordinate>&);
template float ZImgAutoThreshold::typedTriangleThre<float>(const QString&,
                                                           size_t,
                                                           size_t,
                                                           size_t,
                                                           const std::vector<ZVoxelCoordinate>&);
template double ZImgAutoThreshold::typedTriangleThre<double>(const QString&,
                                                             size_t,
                                                             size_t,
                                                             size_t,
                                                             const std::vector<ZVoxelCoordinate>&);

template uint8_t ZImgAutoThreshold::typedCentroidThre<uint8_t>(double&, double&, const ZImg&, size_t, size_t);
template uint16_t ZImgAutoThreshold::typedCentroidThre<uint16_t>(double&, double&, const ZImg&, size_t, size_t);
template uint32_t ZImgAutoThreshold::typedCentroidThre<uint32_t>(double&, double&, const ZImg&, size_t, size_t);
template uint64_t ZImgAutoThreshold::typedCentroidThre<uint64_t>(double&, double&, const ZImg&, size_t, size_t);
template int8_t ZImgAutoThreshold::typedCentroidThre<int8_t>(double&, double&, const ZImg&, size_t, size_t);
template int16_t ZImgAutoThreshold::typedCentroidThre<int16_t>(double&, double&, const ZImg&, size_t, size_t);
template int32_t ZImgAutoThreshold::typedCentroidThre<int32_t>(double&, double&, const ZImg&, size_t, size_t);
template int64_t ZImgAutoThreshold::typedCentroidThre<int64_t>(double&, double&, const ZImg&, size_t, size_t);
template float ZImgAutoThreshold::typedCentroidThre<float>(double&, double&, const ZImg&, size_t, size_t);
template double ZImgAutoThreshold::typedCentroidThre<double>(double&, double&, const ZImg&, size_t, size_t);

template uint8_t ZImgAutoThreshold::typedMaxHistThre<uint8_t>(const ZImg&, size_t, size_t);
template uint16_t ZImgAutoThreshold::typedMaxHistThre<uint16_t>(const ZImg&, size_t, size_t);
template uint32_t ZImgAutoThreshold::typedMaxHistThre<uint32_t>(const ZImg&, size_t, size_t);
template uint64_t ZImgAutoThreshold::typedMaxHistThre<uint64_t>(const ZImg&, size_t, size_t);
template int8_t ZImgAutoThreshold::typedMaxHistThre<int8_t>(const ZImg&, size_t, size_t);
template int16_t ZImgAutoThreshold::typedMaxHistThre<int16_t>(const ZImg&, size_t, size_t);
template int32_t ZImgAutoThreshold::typedMaxHistThre<int32_t>(const ZImg&, size_t, size_t);
template int64_t ZImgAutoThreshold::typedMaxHistThre<int64_t>(const ZImg&, size_t, size_t);
template float ZImgAutoThreshold::typedMaxHistThre<float>(const ZImg&, size_t, size_t);
template double ZImgAutoThreshold::typedMaxHistThre<double>(const ZImg&, size_t, size_t);

} // namespace nim
