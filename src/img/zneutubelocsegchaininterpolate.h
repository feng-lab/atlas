#pragma once

#include "zneutubelocsegchain.h"

#include <array>

namespace nim {

// Port of tz_locseg_chain.c::Locseg_Chain_Interpolate_L().
//
// - `pt` is the query point in stack space.
// - `ort` is an optional orientation vector (nullptr means no orientation).
// - `newPos` is an optional output point (may be updated even when no insertion occurs).
//
// Returns:
// - >=0: the index at which a new segment was inserted, matching legacy semantics.
// - -1 : no insertion happened (break fell on an existing knot).
[[nodiscard]] int locsegChainInterpolateLLegacyLike(LocsegChain& chain,
                                                    const std::array<double, 3>& pt,
                                                    /*nullable*/ const std::array<double, 3>* ort,
                                                    /*nullable*/ std::array<double, 3>* newPos);

} // namespace nim
