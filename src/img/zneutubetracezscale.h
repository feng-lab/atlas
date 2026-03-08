#pragma once

#include "zimginfo.h"

#include "zlog.h"

#include <array>
#include <cmath>

namespace nim {

[[nodiscard]] inline double
preferredZScaleFromVoxelSizeLegacyLike(double voxelSizeX, double voxelSizeY, double voxelSizeZ)
{
  // Mirrors legacy ZStackDoc::getPreferredZScale():
  //   zScale = voxelSizeZ * 2 / (voxelSizeX + voxelSizeY)
  // which reduces to voxelSizeZ / voxelSizeXY when XY spacing is uniform.
  if (!std::isfinite(voxelSizeX) || !std::isfinite(voxelSizeY) || !std::isfinite(voxelSizeZ)) {
    return 1.0;
  }
  if (voxelSizeX <= 0.0 || voxelSizeY <= 0.0 || voxelSizeZ <= 0.0) {
    return 1.0;
  }
  const double xy = (voxelSizeX + voxelSizeY) * 0.5;
  if (!(xy > 0.0)) {
    return 1.0;
  }
  const double zScale = voxelSizeZ / xy;
  if (!std::isfinite(zScale) || !(zScale > 0.0)) {
    return 1.0;
  }
  return zScale;
}

[[nodiscard]] inline double preferredZScaleFromImgInfoLegacyLike(const ZImgInfo& info)
{
  return preferredZScaleFromVoxelSizeLegacyLike(info.voxelSizeX, info.voxelSizeY, info.voxelSizeZ);
}

[[nodiscard]] inline double preferredZScaleFromImgInfoLegacyLike(const ZImgInfo& info,
                                                                 const std::array<size_t, 3>& ratio)
{
  CHECK(ratio[0] > 0);
  CHECK(ratio[1] > 0);
  CHECK(ratio[2] > 0);
  return preferredZScaleFromVoxelSizeLegacyLike(info.voxelSizeX * static_cast<double>(ratio[0]),
                                                info.voxelSizeY * static_cast<double>(ratio[1]),
                                                info.voxelSizeZ * static_cast<double>(ratio[2]));
}

[[nodiscard]] inline std::array<double, 3> traceResolutionFromZScaleLegacyLike(double zScale)
{
  CHECK(std::isfinite(zScale));
  CHECK(zScale > 0.0);
  return {1.0, 1.0, zScale};
}

} // namespace nim
