#pragma once

#include <array>
#include <cstdint>
#include <cmath>

namespace nim {

// True if point `p` is outside the voxel index range addressable by sampling:
//   x ∈ [0, width-1], y ∈ [0, height-1], z ∈ [0, depth-1]
//
// This is intentionally stricter than `p >= dim` for fractional coordinates:
// a position like `x = width - 0.1` is still in range, but `x = width - 0.5`
// is only safe if `width-1` is the last valid voxel index. When tracing uses
// continuous positions but samples discrete voxels via rounding, anything
// slightly above (dim-1) can already round out-of-range.
[[nodiscard]] inline bool
outOfImageForVoxelSampling(int64_t width, int64_t height, int64_t depth, const std::array<double, 3>& p)
{
  const double maxX = static_cast<double>(width) - 1.0;
  const double maxY = static_cast<double>(height) - 1.0;
  const double maxZ = static_cast<double>(depth) - 1.0;

  return p[0] < 0.0 || p[1] < 0.0 || p[2] < 0.0 || p[0] > maxX || p[1] > maxY || p[2] > maxZ;
}

// Helper used by blocked auto trace to decide whether an OutOfBound endpoint corresponds to the dataset boundary
// (in which case there is no "next block" to continue in) vs. a block boundary (in which case we should emit a
// pending task for an adjacent unvisited block).
//
// This is intentionally conservative and uses only cheap geometric probes:
// - `anchor` is the locseg endpoint (top or bottom) in global voxel coordinates.
// - `axisDir` points in the continuation direction in voxel coordinates (need not be normalized).
// - `step` is a probe distance along the axis (voxels).
// - `radiusXY`/`radiusZ` approximate the sampling footprint extents around `anchor`.
//
// Returns true if the continuation would necessarily sample outside the dataset bounds under voxel-sampling
// semantics and therefore should terminate instead of creating a pending task.
[[nodiscard]] inline bool continuationWouldLeaveImageForVoxelSampling(int64_t width,
                                                                      int64_t height,
                                                                      int64_t depth,
                                                                      const std::array<double, 3>& anchor,
                                                                      const std::array<double, 3>& axisDir,
                                                                      double step,
                                                                      double radiusXY,
                                                                      double radiusZ)
{
  if (outOfImageForVoxelSampling(width, height, depth, anchor)) {
    return true;
  }

  const double rxy = std::max(0.0, radiusXY);
  const double rz = std::max(0.0, radiusZ);

  // Radial footprint probes (axis-aligned, conservative).
  if (rxy > 0.0) {
    if (outOfImageForVoxelSampling(width,
                                   height,
                                   depth,
                                   std::array<double, 3>{anchor[0] + rxy, anchor[1], anchor[2]})) {
      return true;
    }
    if (outOfImageForVoxelSampling(width,
                                   height,
                                   depth,
                                   std::array<double, 3>{anchor[0] - rxy, anchor[1], anchor[2]})) {
      return true;
    }
    if (outOfImageForVoxelSampling(width,
                                   height,
                                   depth,
                                   std::array<double, 3>{anchor[0], anchor[1] + rxy, anchor[2]})) {
      return true;
    }
    if (outOfImageForVoxelSampling(width,
                                   height,
                                   depth,
                                   std::array<double, 3>{anchor[0], anchor[1] - rxy, anchor[2]})) {
      return true;
    }
  }

  if (rz > 0.0) {
    if (outOfImageForVoxelSampling(width, height, depth, std::array<double, 3>{anchor[0], anchor[1], anchor[2] + rz})) {
      return true;
    }
    if (outOfImageForVoxelSampling(width, height, depth, std::array<double, 3>{anchor[0], anchor[1], anchor[2] - rz})) {
      return true;
    }
  }

  if (!(step > 0.0)) {
    return false;
  }

  const double len2 = axisDir[0] * axisDir[0] + axisDir[1] * axisDir[1] + axisDir[2] * axisDir[2];
  if (!(len2 > 0.0)) {
    return false;
  }
  const double invLen = 1.0 / std::sqrt(len2);
  const std::array<double, 3> probe = {
    anchor[0] + axisDir[0] * invLen * step,
    anchor[1] + axisDir[1] * invLen * step,
    anchor[2] + axisDir[2] * invLen * step,
  };

  return outOfImageForVoxelSampling(width, height, depth, probe);
}

} // namespace nim
