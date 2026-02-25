#pragma once

#include "zneutubegeo3dscalarfield.h"
#include "zneutubelocalneuroseg.h"
#include "zneutubetraceworkspace.h"

#include "zimg.h"

#include <vector>

namespace nim::neutube {

struct SeedSortResultLegacyLike
{
  std::vector<LocalNeuroseg> locsegArray;
  std::vector<double> scoreArray;
  ZImg baseMask; // GREY (uint8) with values 0/1/2
};

// Port of `ZNeuronTraceSeeder::sortSeed(...)`.
//
// - `seeds` are produced by `extractSeed*` methods (x,y,z integer-valued in practice).
// - Mutates `tw->fitWorkspace` (legacy reuses the same workspace for subsequent tracing).
// - Returns `baseMask` which labels each seed segment with:
//   - 2 when its score exceeds `tw->minScore`,
//   - 1 otherwise.
[[nodiscard]] SeedSortResultLegacyLike
sortSeedsLegacyLike(const Geo3dScalarField& seeds, const ZImg& signal, TraceWorkspace* tw);

} // namespace nim::neutube
