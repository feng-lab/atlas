#pragma once

#include "zneutubelocalneuroseg.h"
#include "zneutubelocsegchain.h"
#include "zneutubetraceworkspace.h"

#include "zimg.h"

#include <array>
#include <memory>
#include <vector>

namespace nim {

// End statuses per kept chain returned by `traceAllSeedsLegacyLike`.
// - `end[0]` is the head/backward end status
// - `end[1]` is the tail/forward end status
using TraceAllSeedsEndStatusLegacyLike = std::array<TraceStatus, 2>;

// Port of `tz_locseg_chain.c::Trace_Locseg_S(...)` (multi-seed tracing).
//
// - `locsegArray` and `scores` must have the same length; `scores` is sorted in-place (ascending),
//   matching legacy behavior (caller should not rely on original order after calling).
// - Returns the traced chains (each owned by a unique_ptr).
[[nodiscard]] std::vector<std::unique_ptr<LocsegChain>>
traceAllSeedsLegacyLike(const ZImg& signal,
                        double zToXYRatio,
                        std::vector<LocalNeuroseg>& locsegArray,
                        std::vector<double>& scores,
                        TraceWorkspace& tw,
                        /*nullable*/ std::vector<TraceAllSeedsEndStatusLegacyLike>* outEndStatuses = nullptr);

} // namespace nim
