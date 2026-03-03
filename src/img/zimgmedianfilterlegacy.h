#pragma once

#include "zimg.h"

namespace nim {

// Port of `Stack_Median_Filter_N(stack, conn=8)` for 2D images (depth=1).
//
// Behavior notes (matches tz_stack_lib.c):
// - Output is initialized as a full copy of the input.
// - Only pixels with a complete 3x3 neighborhood (i.e., not on the border) are filtered.
// - The neighborhood is the 8-neighborhood plus the center pixel (9 samples total).
// - Works for any ZImg voxel type (legacy use sites typically use uint8/uint16).
[[nodiscard]] ZImg medianFilterConn8LegacyLike(const ZImg& img);

} // namespace nim
