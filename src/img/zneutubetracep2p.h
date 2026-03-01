#pragma once

#include "zimg.h"
#include "zswc.h"
#include "zneutubetraceconfig.h"
#include "zneutubeswcsignalfitter.h"

#include <array>
#include <cstddef>
#include <memory>

namespace nim {

// Port of legacy `ZNeuronTracer::trace(x1,y1,z1,r1, x2,y2,z2,r2)` used by
// SWC smart-extend (path computation).
//
// Returns a single-chain SWC where:
// - root is at (rounded) start position
// - leaf is forced back to the original (unrounded) target position
[[nodiscard]] std::unique_ptr<ZSwc> tracePointToPointLegacyLike(const ZImg& signal,
                                                                size_t c,
                                                                size_t t,
                                                                const std::array<double, 3>& start,
                                                                double startRadius,
                                                                const std::array<double, 3>& target,
                                                                double targetRadius,
                                                                const TraceConfig& cfg,
                                                                ZNeutubeImageBackgroundLegacyLike background);

} // namespace nim
