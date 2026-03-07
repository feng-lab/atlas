#pragma once

#include "zimg.h"

namespace folly {
class CancellationToken;
} // namespace folly

namespace nim {

// C++ port of `Stack_Bwdist_L_U16()` / `dt3d_binary_mu16()` (3D squared Euclidean distance transform).
//
// Semantics:
// - Input: GREY (uint8) binary image with foreground > 0 and background == 0.
// - Output: GREY16 (uint16) image where each voxel is the squared distance to the nearest background voxel.
// - Background voxels (==0) map to 0.
//
// Legacy note:
// - neuTube calls `dt3d_binary_mu16(..., pad=!pad)`. The `pad` parameter exposed here follows the
//   public `Stack_Bwdist_L_U16()` signature (i.e. it is inverted when passed to the internal dt3d routine).
[[nodiscard]] ZImg bwdistSquaredU16LegacyLike(const ZImg& binaryMask, int pad);

// Cancellation-aware overload. Throws `ZCancellationException` when cancellation is requested.
[[nodiscard]] ZImg
bwdistSquaredU16LegacyLike(const ZImg& binaryMask, int pad, const folly::CancellationToken& cancellationToken);

} // namespace nim
