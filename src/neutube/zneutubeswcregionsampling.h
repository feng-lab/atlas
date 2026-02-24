#pragma once

#include "zneutubevoxelarray.h"

#include "zswc.h"

#include <memory>

namespace nim::neutube {

// Port of `ZSwcGenerator::createSwcByRegionSampling(const ZVoxelArray&, ...)`.
//
// Creates a single-root SWC chain by sampling voxels along the input path, suppressing
// voxels covered by earlier (larger-radius) samples.
[[nodiscard]] std::unique_ptr<ZSwc> createSwcByRegionSampling(const ZNeutubeVoxelArray& voxelArray,
                                                              double radiusAdjustment = 0.0);

} // namespace nim::neutube
