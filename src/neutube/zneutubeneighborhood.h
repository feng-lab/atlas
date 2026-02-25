#pragma once

#include "zneighborhood.h"

namespace nim {

// Returns a neighborhood definition whose offset ordering matches the legacy neuTube/NeuTu
// connectivity tables (tz_stack_neighborhood.*).
//
// Supported connectivity values: 4, 8, 6, 10, 18, 26.
// Offsets never include the center voxel.
[[nodiscard]] const ZNeighborhood& neighborhoodLegacyOrder(int connectivity);

} // namespace nim
