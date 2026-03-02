#pragma once

#include <cstddef>

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

} // namespace nim
