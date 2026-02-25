#pragma once

#include "zneutubelocsegchain.h"
#include "zneutubestackgraph.h"

#include "zimg.h"

#include <array>
#include <cstdint>
#include <vector>

namespace nim::neutube {

// Port of tz_locseg_chain.c::Locseg_Chain_Point_Dist().
//
// Returns the minimal distance from `pos` to the chain surface (via the knot->ellipse
// representation). If `segIndex` is non-null, it receives the legacy segment id
// (knot->id) corresponding to the closest point. If `skelPos` is non-null, it
// receives the center of the closest ellipse (legacy skeleton position).
[[nodiscard]] double locsegChainPointDistLegacyLike(const LocsegChain& chain,
                                                    const std::array<double, 3>& pos,
                                                    int* segIndex,
                                                    std::array<double, 3>* skelPos);

// Port of tz_locseg_chain.c::Locseg_Chain_Bright_End().
//
// Finds the brightest point between the chain endpoint and its midpoint along the
// endpoint segment axis (legacy point-sampling semantics).
void locsegChainBrightEndLegacyLike(const LocsegChain& chain,
                                    LocsegChainEndLegacyLike end,
                                    const ZImg& signal,
                                    double zScale,
                                    std::array<double, 3>* pos);

// Port of tz_locseg_chain.c::Locseg_Chain_Update_Stack_Graph_Workspace().
//
// Updates the range, group mask, and weight parameters inside `sgw` based on a
// source local-neuroseg and a target chain.
void locsegChainUpdateStackGraphWorkspaceLegacyLike(const LocalNeuroseg& source,
                                                    const LocsegChain& target,
                                                    const ZImg& signal,
                                                    double zScale,
                                                    StackGraphWorkspaceLegacyLike* sgw);

// Port of tz_locseg_chain.c::Locseg_Chain_Shortest_Path_Pt().
//
// Returns a voxel-index path (stack offset indices) between `pos` and the
// (startIndex..endIndex) window around `target`. The returned indices are in the
// coordinate system of `signal` (not range-subvolume indices) and exclude any
// invalid entries.
[[nodiscard]] std::vector<int64_t> locsegChainShortestPathPtLegacyLike(std::array<double, 3> pos,
                                                                       const LocsegChain& target,
                                                                       int startIndex,
                                                                       int endIndex,
                                                                       const ZImg& signal,
                                                                       double zScale,
                                                                       StackGraphWorkspaceLegacyLike* sgw);

// Port of tz_locseg_chain.c::Locseg_Chain_Shortest_Path().
[[nodiscard]] std::vector<int64_t> locsegChainShortestPathLegacyLike(const LocsegChain& source,
                                                                     const LocsegChain& target,
                                                                     const ZImg& signal,
                                                                     double zScale,
                                                                     StackGraphWorkspaceLegacyLike* sgw);

} // namespace nim::neutube
