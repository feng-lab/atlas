#pragma once

#include "zimginfo.h"

#include "zlog.h"

#include <array>
#include <cmath>

namespace nim {

[[nodiscard]] inline double
preferredZToXYRatioFromVoxelSizeLegacyLike(double voxelSizeX, double voxelSizeY, double voxelSizeZ)
{
  // zToXYRatio = voxelSizeZ / voxelSizeXY where voxelSizeXY = (voxelSizeX + voxelSizeY) / 2.
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
  const double zToXYRatio = voxelSizeZ / xy;
  if (!std::isfinite(zToXYRatio) || !(zToXYRatio > 0.0)) {
    return 1.0;
  }
  return zToXYRatio;
}

[[nodiscard]] inline double preferredZToXYRatioFromImgInfoLegacyLike(const ZImgInfo& info)
{
  return preferredZToXYRatioFromVoxelSizeLegacyLike(info.voxelSizeX, info.voxelSizeY, info.voxelSizeZ);
}

[[nodiscard]] inline double preferredZToXYRatioFromImgInfoLegacyLike(const ZImgInfo& info,
                                                                     const std::array<size_t, 3>& ratio)
{
  CHECK(ratio[0] > 0);
  CHECK(ratio[1] > 0);
  CHECK(ratio[2] > 0);
  return preferredZToXYRatioFromVoxelSizeLegacyLike(info.voxelSizeX * static_cast<double>(ratio[0]),
                                                    info.voxelSizeY * static_cast<double>(ratio[1]),
                                                    info.voxelSizeZ * static_cast<double>(ratio[2]));
}

[[nodiscard]] inline std::array<double, 3> traceResolutionFromZToXYRatioLegacyLike(double zToXYRatio)
{
  CHECK(std::isfinite(zToXYRatio));
  CHECK(zToXYRatio > 0.0);
  // Return a normalized step-length metric, not raw voxel-size metadata.
  //
  // The legacy tracing ports that consume `resolution` only need the relative Z-vs-XY spacing, so we keep XY at the
  // tracing baseline `1` and encode anisotropy in Z as `{1, 1, zToXYRatio}`. This tuple is appropriate for
  // resolution-aware routing / connection workspaces and for trace workspaces that intentionally opt into their
  // resolution-aware size checks. It must not be treated as a drop-in replacement for every `TraceWorkspace`
  // initialization site, because leaving `TraceWorkspace::resolution` unset still has distinct legacy semantics.
  return {1.0, 1.0, zToXYRatio};
}

} // namespace nim
