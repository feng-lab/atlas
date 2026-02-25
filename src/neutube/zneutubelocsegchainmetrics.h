#pragma once

#include "zneutubelocsegchain.h"
#include "zneutubestackfitoptions.h"

#include "zimg.h"

namespace nim::neutube {

// Port of `tz_locseg_chain.c::Locseg_Chain_Geolen()`.
[[nodiscard]] double locsegChainGeolenLegacyLike(const LocsegChain& chain);

// Port of `tz_locseg_chain.c::Locseg_Chain_Average_Score()`.
[[nodiscard]] double
locsegChainAverageScoreLegacyLike(const LocsegChain& chain, const ZImg& stack, double zScale, StackFitOption option);

// Port of `tz_locseg_chain.c::Locseg_Chain_Min_Seg_Score()`.
[[nodiscard]] double
locsegChainMinSegScoreLegacyLike(const LocsegChain& chain, const ZImg& stack, double zScale, StackFitOption option);

// Port of `tz_locseg_chain.c::Locseg_Chain_Average_Signal()`.
[[nodiscard]] double locsegChainAverageSignalLegacyLike(const LocsegChain& chain, const ZImg& stack, double zScale);

// Port of tz_locseg_chain.c::locseg_chain_dist_upper_bound().
// Computes an upper bound on the minimal distance between a `testseg` center and
// the centers of segments in `chain` after applying `localNeurosegScaleZLegacyLike`
// to the chain segments with `zScale`.
[[nodiscard]] double
locsegChainDistUpperBoundLegacyLike(const LocsegChain& chain, double zScale, const LocalNeuroseg& testseg);

} // namespace nim::neutube
