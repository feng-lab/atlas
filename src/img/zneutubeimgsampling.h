#pragma once

#include <cstddef>
#include <limits>

namespace nim {

class ZImg;
class ZVoxelVolume;
}

namespace nim {

// Port of legacy tz_stack_sampling.c::Stack_Point_Sampling().
//
// Semantics:
// - Returns NaN if (x,y,z) lies on or outside the 1-voxel border of the volume.
// - Uses trilinear interpolation from 8 surrounding voxels (x_low/y_low/z_low).
// - Matches the legacy floating-point operation order (for strict A/B parity).
[[nodiscard]] double pointSampleLegacyLike(const ZImg& img, double x, double y, double z, size_t c = 0, size_t t = 0);

// Overload for read-only voxel volumes (single-channel/time view).
[[nodiscard]] double pointSampleLegacyLike(const ZVoxelVolume& img, double x, double y, double z);

// Port of legacy tz_stack_sampling.c::Stack_Point_Hit_Mask().
//
// Semantics:
// - Uses integer rounding via `int(x + 0.5)` (legacy truncation toward 0).
// - Returns false when out of bounds.
// - Returns true if the rounded voxel value is non-zero.
[[nodiscard]] bool pointHitMaskLegacyLike(const ZImg& img, double x, double y, double z, size_t c = 0, size_t t = 0);

// Overload for read-only voxel volumes (single-channel/time view).
[[nodiscard]] bool pointHitMaskLegacyLike(const ZVoxelVolume& img, double x, double y, double z);

// Fast helper for hot loops where caller has already hoisted per-call invariants
// (array pointer, dimensions, channelVoxelNumber, and channel index).
//
// This matches the operation order of STACK_POINT_SAMPLING4 in tz_stack_sampling.c.
template<typename TVoxel>
[[nodiscard]] double pointSampleLegacyLikeTypedFast(const TVoxel* array,
                                                    size_t width,
                                                    size_t height,
                                                    size_t depth,
                                                    size_t channelVoxelNumber,
                                                    size_t c,
                                                    double x,
                                                    double y,
                                                    double z)
{
  // Legacy behavior: return NaN if point is on the border (or out of bounds).
  if (x >= static_cast<double>(width) - 1.0 || x <= 0.0 || y >= static_cast<double>(height) - 1.0 || y <= 0.0 ||
      z >= static_cast<double>(depth) - 1.0 || z <= 0.0) {
    return std::numeric_limits<double>::quiet_NaN();
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
  const size_t offset = channelVoxelNumber * c + area * zLow + width * yLow + xLow;

  const TVoxel* p = array + offset;

  double sum = wxLow * static_cast<double>(*(p++));
  sum += wxHigh * static_cast<double>(*p);
  sum *= wyLow * wzLow;

  p += width;
  double tmpSum = wxHigh * static_cast<double>(*(p--));
  tmpSum += wxLow * static_cast<double>(*p);
  sum += tmpSum * wyHigh * wzLow;

  p += area;
  tmpSum = wxLow * static_cast<double>(*(p++));
  tmpSum += wxHigh * static_cast<double>(*p);
  sum += tmpSum * wyHigh * wzHigh;

  p -= width;
  tmpSum = wxHigh * static_cast<double>(*(p--));
  tmpSum += wxLow * static_cast<double>(*p);
  sum += tmpSum * wyLow * wzHigh;

  return sum;
}

} // namespace nim
