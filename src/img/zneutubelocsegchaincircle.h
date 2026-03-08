#pragma once

#include "zneutubegeo3dcircle.h"
#include "zneutubelocsegchain.h"

#include <vector>

namespace nim {

// Port of tz_locseg_chain.c::Locseg_Chain_To_Geo3d_Circle_Array_Z().
[[nodiscard]] std::vector<Geo3dCircle> locsegChainToGeo3dCircleArrayZLegacyLike(const LocsegChain& chain,
                                                                                double zToXYRatio);

// Legacy trace-mask equivalent: swells each local neuroseg before converting the chain into circles.
[[nodiscard]] std::vector<Geo3dCircle> locsegChainToGeo3dCircleArraySwelledZLegacyLike(const LocsegChain& chain,
                                                                                       double zToXYRatio,
                                                                                       double swellRatio,
                                                                                       double swellDiff,
                                                                                       double swellLimit);

// Port of tz_locseg_chain.c::Locseg_Chain_To_Geo3d_Circle_Array().
[[nodiscard]] std::vector<Geo3dCircle> locsegChainToGeo3dCircleArrayLegacyLike(const LocsegChain& chain);

} // namespace nim
