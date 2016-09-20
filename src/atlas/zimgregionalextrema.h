#pragma once

#include "zimg.h"
#include "zimgalgorithm.h"
#include <functional>

namespace nim {

template<bool ReportProgress = false>
class ZImgRegionalExtrema : public ZImgAlgorithm<ReportProgress>
{
public:

  // regional min max
  // finds the regional maxima, returns 8-bit unsigned image mask that identifies the locations of the regional maxima.
  // In mask, pixels that are set to 1 identify regional maxima; all other pixels are set to 0.
  // conn can be 4, 8 (2D) or 6, 18, 26 (3D)
  // if conn == 0, uses a default connectivity of 8 for two dimensions, 26 for three dimensions
  inline ZImg regionalMax(const ZImg& img, size_t conn = 0)
  {
    return regionalExtrema<std::greater>(img, conn);
  }

  // regional minima
  inline ZImg regionalMin(const ZImg& img, size_t conn = 0)
  {
    return regionalExtrema<std::less>(img, conn);
  }

private:
  //
  template<template<typename> class Compare>
  ZImg regionalExtrema(const ZImg& img, size_t conn = 0);

  template<typename TVoxel, template<typename> class Compare>
  void regionalExtrema_Impl(ZImg& res, const ZImg& img, size_t conn);

};

} // namespace nim

