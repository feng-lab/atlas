#pragma once

#include "zimg.h"

namespace nim {

// Logical NOT for a GREY (uint8) binary image, matching legacy Stack_Not():
// out[x] = (in[x] == 0) ? 1 : 0.
[[nodiscard]] ZImg stackNotBinaryU8(const ZImg& in);

// Majority filter for a GREY (uint8) binary image, matching legacy Stack_Majority_Filter().
// For 3D inputs and 4/8 connectivity, it operates slice-by-slice (Z ignored), matching legacy.
[[nodiscard]] ZImg majorityFilterBinaryU8(const ZImg& in, int connectivity);

// Majority filter with an absolute neighbor-count threshold, matching legacy Stack_Majority_Filter_R().
//
// - For internal voxels (all neighbors in-bounds): clears voxels with neighbor-foreground count < mnbr.
// - For boundary voxels: clears voxels when n * connectivity < mnbr * nbound.
[[nodiscard]] ZImg majorityFilterBinaryU8RLegacyLike(const ZImg& in, int connectivity, int mnbr);

// Fill holes in a GREY (uint8) binary image, matching legacy Stack_Fill_Hole_N():
// - Labels background connected to the boundary with value 1 (same as foreground),
// - Inverts and ORs with the original to fill enclosed holes.
[[nodiscard]] ZImg fillHolesBinaryU8(const ZImg& in, int connectivity);

} // namespace nim
