#pragma once

#include "zimg.h"

namespace nim {

// Legacy STACK_LOCMAX_* option values (tz_image_lib_defs.h).
enum class StackLocmaxOptionLegacyLike : int
{
  Center = 0,
  Neighbor = 1,
  NonFlat = 2,
  Flat = 3,
  Alter1 = 4,
  Alter2 = 5,
  Single = 6
};

// Port of tz_stack_lib.c::Stack_Local_Max().
//
// Returns a GREY (uint8) mask with values:
// - 1 at local maxima under the legacy scan-mask rules
// - 0 otherwise
[[nodiscard]] ZImg stackLocalMaxMaskLegacyLike(const ZImg& stack, StackLocmaxOptionLegacyLike option);

// Port of tz_stack.c::Stack_Locmax_Region().
//
// Returns a GREY (uint8) mask with values:
// - 1 for voxels belonging to local-maximum plateaus/regions
// - 0 otherwise
[[nodiscard]] ZImg stackLocmaxRegionMaskLegacyLike(const ZImg& stack, int connectivity);

} // namespace nim
