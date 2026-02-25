#include "zneutubeinthistogram.h"

#include "zexception.h"
#include "zlog.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace nim::neutube {

namespace {

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

[[nodiscard]] double iarrayCentroidD(const int* a, size_t length)
{
  CHECK(a != nullptr);
  CHECK(length > 0);

  double totalWeight = 0.0;
  double totalPos = 0.0;

  for (size_t i = 0; i < length; ++i) {
    totalWeight += static_cast<double>(a[i]);
    totalPos += static_cast<double>(a[i]) * static_cast<double>(i);
  }

  if (totalWeight == 0.0) {
    return static_cast<double>(length) / 2.0;
  }
  return totalPos / totalWeight;
}

} // namespace

IntHistogramLegacyLike::IntHistogramLegacyLike(std::vector<int> data)
  : _hist(std::move(data))
{}

int IntHistogramLegacyLike::minValue() const
{
  CHECK(!_hist.empty());
  CHECK(_hist.size() >= 2);
  return _hist[1];
}

int IntHistogramLegacyLike::maxValue() const
{
  CHECK(!_hist.empty());
  CHECK(_hist.size() >= 2);
  return _hist[0] + _hist[1] - 1;
}

int IntHistogramLegacyLike::length() const
{
  CHECK(!_hist.empty());
  return _hist[0];
}

int IntHistogramLegacyLike::sum() const
{
  CHECK(!_hist.empty());
  const int len = _hist[0];
  CHECK(static_cast<size_t>(len + 2) == _hist.size());

  int total = 0;
  for (int i = 0; i < len; ++i) {
    total += _hist[2 + i];
  }
  return total;
}

int IntHistogramLegacyLike::count(int v) const
{
  if (_hist.empty()) {
    return 0;
  }

  const int minV = minValue();
  const int maxV = maxValue();
  if (v < minV || v > maxV) {
    return 0;
  }

  return _hist[v + 2 - minV];
}

int IntHistogramLegacyLike::mode(int minV, int maxV) const
{
  CHECK(!_hist.empty());

  minV = std::max(minV, this->minValue());
  maxV = std::min(maxV, this->maxValue());

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

int IntHistogramLegacyLike::upperCount(int v) const
{
  CHECK(!_hist.empty());

  int c = 0;
  const int minV = std::max(v, this->minValue());
  const int maxV = this->maxValue();
  for (int i = minV; i <= maxV; ++i) {
    c += count(i);
  }
  return c;
}

std::optional<IntHistogramLegacyLike> imageHistogramLegacyLike(const ZImg& img, const ZImg* mask)
{
  if (img.isEmpty()) {
    throw ZException("imageHistogramLegacyLike: empty image.");
  }

  if (img.numChannels() != 1 || img.numTimes() != 1) {
    throw ZException(fmt::format("imageHistogramLegacyLike: expected 1 channel/time, got channels={} times={}.",
                                 img.numChannels(),
                                 img.numTimes()));
  }

  const uint8_t* maskData = nullptr;
  if (mask != nullptr) {
    if (mask->isEmpty()) {
      return std::nullopt;
    }
    if (mask->numChannels() != 1 || mask->numTimes() != 1) {
      throw ZException("imageHistogramLegacyLike: mask must be single channel/time.");
    }
    if (mask->width() != img.width() || mask->height() != img.height() || mask->depth() != img.depth()) {
      throw ZException("imageHistogramLegacyLike: mask size mismatch.");
    }
    if (!mask->isType<uint8_t>()) {
      throw ZException("imageHistogramLegacyLike: mask must be uint8.");
    }
    maskData = mask->timeData<uint8_t>(0);
  }

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
    throw ZException(fmt::format("imageHistogramLegacyLike: unsupported voxel type {}", img.info()));
  }

  if (!minMax) {
    return std::nullopt;
  }

  const int minV = minMax->first;
  const int maxV = minMax->second;
  CHECK(maxV >= minV);

  const int range = maxV - minV;
  const size_t histSize = static_cast<size_t>(range + 3);

  std::vector<int> hist(histSize, 0);
  hist[0] = range + 1;
  hist[1] = minV;

  const size_t bins = static_cast<size_t>(hist[0]);
  CHECK(hist.size() == bins + 2);

  auto addValue = [&](int v) {
    const size_t idx = static_cast<size_t>(v - minV);
    CHECK(idx < bins);
    int& c = hist[2 + idx];
    if (c < IntHistogramMaxCountLegacyLike) {
      ++c;
    }
  };

  if (img.isType<uint8_t>()) {
    const auto* a = img.timeData<uint8_t>(0);
    if (maskData == nullptr) {
      for (size_t i = 0; i < n; ++i) {
        addValue(static_cast<int>(a[i]));
      }
    } else {
      for (size_t i = 0; i < n; ++i) {
        if (maskData[i] == 1) {
          addValue(static_cast<int>(a[i]));
        }
      }
    }
  } else {
    const auto* a = img.timeData<uint16_t>(0);
    if (maskData == nullptr) {
      for (size_t i = 0; i < n; ++i) {
        addValue(static_cast<int>(a[i]));
      }
    } else {
      for (size_t i = 0; i < n; ++i) {
        if (maskData[i] == 1) {
          addValue(static_cast<int>(a[i]));
        }
      }
    }
  }

  return IntHistogramLegacyLike(std::move(hist));
}

int triangleThresholdLegacyLike(const IntHistogramLegacyLike& hist, int low, int high)
{
  if (hist.empty()) {
    throw ZException("triangleThresholdLegacyLike: empty histogram.");
  }

  int minGrey = hist.minValue();
  int maxGrey = hist.maxValue();

  const std::vector<int>& h = hist.data();
  const int* hist2 = h.data() + 2;

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
  const double normFactor =
    static_cast<double>(length - 1) / (static_cast<double>(hist2[0] - hist2[static_cast<size_t>(length) - 1]));
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

int rcthreRLegacyLike(const IntHistogramLegacyLike& hist, int low, int high, double* c1, double* c2)
{
  // Port of tz_stack_threshold.c::Hist_Rcthre_R().
  CHECK(c1 != nullptr);
  CHECK(c2 != nullptr);

  if (hist.empty()) {
    throw ZException("rcthreRLegacyLike: empty histogram.");
  }

  const std::vector<int>& h = hist.data();
  CHECK(h.size() >= 2);
  CHECK(h[0] >= 1);
  CHECK(static_cast<size_t>(h[0] + 2) == h.size());

  const int* hist2 = h.data() + 2;
  int length = h[0];
  int minGrey = h[1];
  int maxGrey = h[0] + h[1] - 1;

  if (low > minGrey) {
    hist2 += low - minGrey;
    minGrey = low;
  }

  if (high < maxGrey) {
    maxGrey = high;
  }

  length = maxGrey - minGrey + 1;
  CHECK(length >= 1);

  int thre = static_cast<int>((static_cast<double>(minGrey + maxGrey) / 2.0) - static_cast<double>(minGrey));

  *c1 = 0.0;
  *c2 = 0.0;

  int prevThre = -1;
  do {
    if (thre == length - 1) {
      break;
    }

    prevThre = thre;

    *c1 = iarrayCentroidD(hist2, static_cast<size_t>(thre) + 1);
    *c2 = iarrayCentroidD(hist2 + thre + 1, static_cast<size_t>(length - thre - 1));

    *c2 += static_cast<double>(thre + 1);
    thre = static_cast<int>((*c1 + *c2) / 2.0);
  } while (thre != prevThre);

  thre += minGrey;
  return thre;
}

} // namespace nim::neutube
