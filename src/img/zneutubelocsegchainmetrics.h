#pragma once

#include "zneutubelocsegchain.h"
#include "zneutubestackfitoptions.h"

#include "zimg.h"

namespace nim {

// Port of `tz_locseg_chain.c::Locseg_Chain_Geolen()`.
[[nodiscard]] double locsegChainGeolenLegacyLike(const LocsegChain& chain);

// Port of `tz_locseg_chain.c::Locseg_Chain_Average_Score()`.
[[nodiscard]] double locsegChainAverageScoreLegacyLike(const LocsegChain& chain,
                                                       const ZImg& stack,
                                                       double zToXYRatio,
                                                       StackFitOption option);

// Port of `tz_locseg_chain.c::Locseg_Chain_Min_Seg_Score()`.
[[nodiscard]] double
locsegChainMinSegScoreLegacyLike(const LocsegChain& chain, const ZImg& stack, double zToXYRatio, StackFitOption option);

// Port of `tz_locseg_chain.c::Locseg_Chain_Average_Signal()`.
[[nodiscard]] double locsegChainAverageSignalLegacyLike(const LocsegChain& chain, const ZImg& stack, double zToXYRatio);

// Port of tz_locseg_chain.c::locseg_chain_dist_upper_bound().
[[nodiscard]] double
locsegChainDistUpperBoundLegacyLike(const LocsegChain& chain, double zToXYRatio, const LocalNeuroseg& testseg);

} // namespace nim
