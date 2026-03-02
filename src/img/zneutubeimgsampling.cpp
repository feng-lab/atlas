#include "zneutubeimgsampling.h"

#include "zimg.h"
#include "zvoxelvolume.h"
#include "zlog.h"

#include <limits>

namespace nim {

namespace {

[[nodiscard]] double nanValue()
{
  return std::numeric_limits<double>::quiet_NaN();
}

template<typename TVoxel>
[[nodiscard]] double pointSampleLegacyLikeTyped(const ZImg& img, double x, double y, double z, size_t c, size_t t)
{
  const size_t width = img.width();
  const size_t height = img.height();
  const size_t depth = img.depth();

  // Legacy behavior: return NaN if point is on the border (or out of bounds).
  if (x >= static_cast<double>(width) - 1.0 || x <= 0.0 || y >= static_cast<double>(height) - 1.0 || y <= 0.0 ||
      z >= static_cast<double>(depth) - 1.0 || z <= 0.0) {
    return nanValue();
  }

  // Legacy truncation is (int)x; for x>0 this is equivalent to floor(x).
  const size_t xLow = static_cast<size_t>(x);
  const size_t yLow = static_cast<size_t>(y);
  const size_t zLow = static_cast<size_t>(z);

  const double wxHigh = x - static_cast<double>(xLow);
  const double wxLow = 1.0 - wxHigh;
  const double wyHigh = y - static_cast<double>(yLow);
  const double wyLow = 1.0 - wyHigh;
  const double wzHigh = z - static_cast<double>(zLow);
  const double wzLow = 1.0 - wzHigh;

  const size_t area = width * height;
  const size_t channelVoxelNumber = area * depth;
  const size_t offset = channelVoxelNumber * c + area * zLow + width * yLow + xLow;

  const TVoxel* array = img.timeData<TVoxel>(t);
  array += offset;

  // Matches STACK_POINT_SAMPLING4 in tz_stack_sampling.c (operation order matters for strict parity).
  double sum = wxLow * static_cast<double>(*(array++));
  sum += wxHigh * static_cast<double>(*array);
  sum *= wyLow * wzLow;

  array += width;
  double tmpSum = wxHigh * static_cast<double>(*(array--));
  tmpSum += wxLow * static_cast<double>(*array);
  sum += tmpSum * wyHigh * wzLow;

  array += area;
  tmpSum = wxLow * static_cast<double>(*(array++));
  tmpSum += wxHigh * static_cast<double>(*array);
  sum += tmpSum * wyHigh * wzHigh;

  array -= width;
  tmpSum = wxHigh * static_cast<double>(*(array--));
  tmpSum += wxLow * static_cast<double>(*array);
  sum += tmpSum * wyLow * wzHigh;

  return sum;
}

template<typename TVoxel>
[[nodiscard]] bool pointHitMaskLegacyLikeTyped(const ZImg& img, double x, double y, double z, size_t c, size_t t)
{
  const int xLow = static_cast<int>(x + 0.5);
  const int yLow = static_cast<int>(y + 0.5);
  const int zLow = static_cast<int>(z + 0.5);

  const int width = static_cast<int>(img.width());
  const int height = static_cast<int>(img.height());
  const int depth = static_cast<int>(img.depth());

  if (xLow >= width || xLow < 0 || yLow >= height || yLow < 0 || zLow >= depth || zLow < 0) {
    return false;
  }

  const size_t uWidth = img.width();
  const size_t uHeight = img.height();
  const size_t uDepth = img.depth();

  const size_t area = uWidth * uHeight;
  const size_t channelVoxelNumber = area * uDepth;
  const size_t offset = channelVoxelNumber * c + area * static_cast<size_t>(zLow) + uWidth * static_cast<size_t>(yLow) +
                        static_cast<size_t>(xLow);

  const TVoxel* array = img.timeData<TVoxel>(t);
  return array[offset] != static_cast<TVoxel>(0);
}

} // namespace

double pointSampleLegacyLike(const ZImg& img, double x, double y, double z, size_t c, size_t t)
{
  CHECK(!img.isEmpty());
  CHECK(img.numChannels() > c);
  CHECK(img.numTimes() > t);

  if (img.isType<uint8_t>()) {
    return pointSampleLegacyLikeTyped<uint8_t>(img, x, y, z, c, t);
  }
  if (img.isType<uint16_t>()) {
    return pointSampleLegacyLikeTyped<uint16_t>(img, x, y, z, c, t);
  }
  if (img.isType<float>()) {
    return pointSampleLegacyLikeTyped<float>(img, x, y, z, c, t);
  }
  if (img.isType<double>()) {
    return pointSampleLegacyLikeTyped<double>(img, x, y, z, c, t);
  }

  CHECK(false) << "Unsupported voxel type for pointSampleLegacyLike: " << img.info();
  return nanValue();
}

bool pointHitMaskLegacyLike(const ZImg& img, double x, double y, double z, size_t c, size_t t)
{
  CHECK(!img.isEmpty());
  CHECK(img.numChannels() > c);
  CHECK(img.numTimes() > t);

  if (img.isType<uint8_t>()) {
    return pointHitMaskLegacyLikeTyped<uint8_t>(img, x, y, z, c, t);
  }
  if (img.isType<uint16_t>()) {
    return pointHitMaskLegacyLikeTyped<uint16_t>(img, x, y, z, c, t);
  }
  if (img.isType<float>()) {
    return pointHitMaskLegacyLikeTyped<float>(img, x, y, z, c, t);
  }
  if (img.isType<double>()) {
    return pointHitMaskLegacyLikeTyped<double>(img, x, y, z, c, t);
  }

  CHECK(false) << "Unsupported voxel type for pointHitMaskLegacyLike: " << img.info();
  return false;
}

double pointSampleLegacyLike(const ZVoxelVolume& img, double x, double y, double z)
{
  if (img.isEmpty()) {
    return nanValue();
  }

  const size_t width = img.width();
  const size_t height = img.height();
  const size_t depth = img.depth();

  // Legacy behavior: return NaN if point is on the border (or out of bounds).
  if (x >= static_cast<double>(width) - 1.0 || x <= 0.0 || y >= static_cast<double>(height) - 1.0 || y <= 0.0 ||
      z >= static_cast<double>(depth) - 1.0 || z <= 0.0) {
    return nanValue();
  }

  // Legacy truncation is (int)x; for x>0 this is equivalent to floor(x).
  const size_t xLow = static_cast<size_t>(x);
  const size_t yLow = static_cast<size_t>(y);
  const size_t zLow = static_cast<size_t>(z);

  const double wxHigh = x - static_cast<double>(xLow);
  const double wxLow = 1.0 - wxHigh;
  const double wyHigh = y - static_cast<double>(yLow);
  const double wyLow = 1.0 - wyHigh;
  const double wzHigh = z - static_cast<double>(zLow);
  const double wzLow = 1.0 - wzHigh;

  const int ix = static_cast<int>(xLow);
  const int iy = static_cast<int>(yLow);
  const int iz = static_cast<int>(zLow);

  // Matches STACK_POINT_SAMPLING4 in tz_stack_sampling.c (operation order matters for strict parity).
  double sum = wxLow * img.valueAsDouble(ix, iy, iz);
  sum += wxHigh * img.valueAsDouble(ix + 1, iy, iz);
  sum *= wyLow * wzLow;

  double tmpSum = wxHigh * img.valueAsDouble(ix + 1, iy + 1, iz);
  tmpSum += wxLow * img.valueAsDouble(ix, iy + 1, iz);
  sum += tmpSum * wyHigh * wzLow;

  tmpSum = wxLow * img.valueAsDouble(ix, iy + 1, iz + 1);
  tmpSum += wxHigh * img.valueAsDouble(ix + 1, iy + 1, iz + 1);
  sum += tmpSum * wyHigh * wzHigh;

  tmpSum = wxHigh * img.valueAsDouble(ix + 1, iy, iz + 1);
  tmpSum += wxLow * img.valueAsDouble(ix, iy, iz + 1);
  sum += tmpSum * wyLow * wzHigh;

  return sum;
}

bool pointHitMaskLegacyLike(const ZVoxelVolume& img, double x, double y, double z)
{
  if (img.isEmpty()) {
    return false;
  }

  const int xLow = static_cast<int>(x + 0.5);
  const int yLow = static_cast<int>(y + 0.5);
  const int zLow = static_cast<int>(z + 0.5);

  const int width = static_cast<int>(img.width());
  const int height = static_cast<int>(img.height());
  const int depth = static_cast<int>(img.depth());

  if (xLow >= width || xLow < 0 || yLow >= height || yLow < 0 || zLow >= depth || zLow < 0) {
    return false;
  }

  return img.valueAsDouble(xLow, yLow, zLow) != 0.0;
}

} // namespace nim
