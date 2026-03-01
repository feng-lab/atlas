#pragma once

#include "zimg.h"

namespace nim {

// Port of `Stack_Bwthin()` (tz_stack_bwmorph.c) for GREY (u8) stacks.
//
// Expected input:
// - Unsigned 8-bit image
// - Foreground is any non-zero value (legacy treats >0 as ON)
//
// Output:
// - Unsigned 8-bit image with the same dimensions
// - Foreground pixels remain 1, background 0
[[nodiscard]] ZImg bwthinLegacyLike(const ZImg& binary);

} // namespace nim
