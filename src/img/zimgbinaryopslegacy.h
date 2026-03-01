#pragma once

#include "zimg.h"

namespace nim {

// Port of `Stack_Invert_Value()`:
// for each voxel: v <- min + max - v, where min/max are computed over the whole image
// (per-channel/per-time for ZImg).
void invertValueInPlaceLegacyLike(ZImg& img);

// Port of `Stack_Threshold_Binarize(stack, thre)` for GREY/GREY16:
// out(x) = (in(x) > thre) ? 1 : 0, with an unsigned 8-bit output image.
[[nodiscard]] ZImg binarizeGreaterThanLegacyLike(const ZImg& img, int threshold);

} // namespace nim
