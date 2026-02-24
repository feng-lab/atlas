#pragma once

#include "zimg.h"

namespace nim::neutube {

// Logical NOT for a GREY (uint8) binary image, matching legacy Stack_Not():
// out[x] = (in[x] == 0) ? 1 : 0.
[[nodiscard]] ZImg stackNotBinaryU8(const ZImg& in);

// Majority filter for a GREY (uint8) binary image, matching legacy Stack_Majority_Filter().
// For 3D inputs and 4/8 connectivity, it operates slice-by-slice (Z ignored), matching legacy.
[[nodiscard]] ZImg majorityFilterBinaryU8(const ZImg& in, int connectivity);

// Fill holes in a GREY (uint8) binary image, matching legacy Stack_Fill_Hole_N():
// - Labels background connected to the boundary with value 1 (same as foreground),
// - Inverts and ORs with the original to fill enclosed holes.
[[nodiscard]] ZImg fillHolesBinaryU8(const ZImg& in, int connectivity);

} // namespace nim::neutube
