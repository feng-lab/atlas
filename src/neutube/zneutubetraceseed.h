#pragma once

#include "zneutubegeo3dscalarfield.h"
#include "zneutubetraceworkspace.h"
#include "zimg.h"

namespace nim::neutube {

// Port of `ZNeuronTracer::extractSeedOriginal`.
//
// - Input `mask` must be a GREY (uint8) binary mask with values {0,1}.
// - Returns seeds in the same order as legacy `Stack_To_Voxel_List` (reverse stack-array order).
[[nodiscard]] Geo3dScalarField extractSeedOriginalLegacyLike(const ZImg& mask);

// Port of `ZNeuronTracer::removeTracedSeed`.
//
// Filters out any seed whose integer (x,y,z) voxel is already marked in `tw.traceMask`.
[[nodiscard]] Geo3dScalarField removeTracedSeedLegacyLike(const Geo3dScalarField& seeds, const TraceWorkspace& tw);

struct RemoveNoisySeedDiagnosticsLegacyLike
{
  double seedDensity = 0.0;
  int minSeedSize = 0;
};

// Port of `ZNeuronTracer::removeNoisySeed` (seed screening based on density).
//
// - May mutate `mask` by removing objects smaller than a derived size threshold.
// - If mutation occurs, seeds are re-extracted from the updated mask using the given seed method.
[[nodiscard]] Geo3dScalarField removeNoisySeedLegacyLike(Geo3dScalarField seeds,
                                                         ZImg* mask,
                                                         int seedMethod,
                                                         bool screeningSeed,
                                                         RemoveNoisySeedDiagnosticsLegacyLike* diag);

} // namespace nim::neutube
