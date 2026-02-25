#pragma once

#include "zneutubegeo3dcircle.h"
#include "zneutubelocsegchain.h"

#include <vector>

namespace nim {

// Port of tz_locseg_chain.c::Locseg_Chain_To_Geo3d_Circle_Array_Z().
[[nodiscard]] std::vector<Geo3dCircle> locsegChainToGeo3dCircleArrayZLegacyLike(const LocsegChain& chain,
                                                                                double zScale);

// Port of tz_locseg_chain.c::Locseg_Chain_To_Geo3d_Circle_Array().
[[nodiscard]] std::vector<Geo3dCircle> locsegChainToGeo3dCircleArrayLegacyLike(const LocsegChain& chain);

} // namespace nim
