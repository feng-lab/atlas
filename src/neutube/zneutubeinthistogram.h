#pragma once

#include "zimg.h"

#include <optional>
#include <vector>

namespace nim::neutube {

inline constexpr int IntHistogramMaxCountLegacyLike = 2147483647;

// Minimal C++ wrapper for the legacy int-histogram array format (tz_int_histogram.h):
//
// - hist[0] = length (number of bins)
// - hist[1] = starting value (min value)
// - hist[2 + i] = count for (hist[1] + i)
class IntHistogramLegacyLike
{
public:
  IntHistogramLegacyLike() = default;
  explicit IntHistogramLegacyLike(std::vector<int> data);

  [[nodiscard]] bool empty() const
  {
    return _hist.empty();
  }

  [[nodiscard]] int minValue() const;
  [[nodiscard]] int maxValue() const;
  [[nodiscard]] int length() const;
  [[nodiscard]] int sum() const;

  [[nodiscard]] int count(int v) const;

  // Returns the smallest value in [minV, maxV] with maximal count (matches ZIntHistogram::getMode).
  [[nodiscard]] int mode(int minV, int maxV) const;

  // #{x | I(x) >= v}
  [[nodiscard]] int upperCount(int v) const;

  [[nodiscard]] const std::vector<int>& data() const
  {
    return _hist;
  }

private:
  std::vector<int> _hist;
};

// Port of Image_Array_Hist_M for GREY/GREY16 stacks backed by ZImg.
//
// - Returns nullopt if the mask is present and has no voxels with value == 1.
// - If mask is present, only voxels with mask==1 are counted (matches legacy equality check).
[[nodiscard]] std::optional<IntHistogramLegacyLike> imageHistogramLegacyLike(const ZImg& img, const ZImg* mask);

// Port of tz_int_histogram.c::Int_Histogram_Triangle_Threshold().
[[nodiscard]] int triangleThresholdLegacyLike(const IntHistogramLegacyLike& hist, int low, int high);

// Port of tz_stack_threshold.c::Hist_Rcthre_R().
//
// Returns a threshold value in the same intensity units as the histogram.
// Outputs `c1` and `c2` (centroids) in histogram-bin coordinates (relative to the
// effective histogram minimum after clamping to [low, high]), matching legacy behavior.
[[nodiscard]] int rcthreRLegacyLike(const IntHistogramLegacyLike& hist, int low, int high, double* c1, double* c2);

} // namespace nim::neutube
