#pragma once

#include "zimg.h"

namespace nim::neutube {

// Port of legacy `Stack_Bwdist_L_U16P` (planar/2D-per-slice squared Euclidean distance transform).
//
// Input:
// - `binaryMask`: GREY (uint8) with foreground > 0 and background == 0.
//
// Output:
// - GREY16 (uint16) where each voxel is the squared distance (in pixel units) to the nearest background voxel,
//   computed independently per Z slice.
// - Background voxels have distance 0.
//
// Notes:
// - This matches legacy `dt3d_binary_mu16_p(..., pad=1)` behavior: out-of-bounds are treated as background.
[[nodiscard]] ZImg planarBwdistSquaredU16P(const ZImg& binaryMask);

} // namespace nim::neutube
