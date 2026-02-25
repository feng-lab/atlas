#pragma once

#include "zneutubelocsegchain.h"
#include "zneutubetraceworkspace.h"

namespace nim::neutube {

// Port of tz_locseg_chain.c::Locseg_Chain_Trace_Test().
//
// Notes:
// - Nullable pointer parameters preserve legacy call patterns:
//   - `prevLocseg` is passed as nullptr by the trace loop when there is no prior segment yet
//     (see `src/neutube/zneutubelocsegchaintrace.cpp`).
//   - `tr` may be null to skip trace-record score gating (legacy supports this).
[[nodiscard]] TraceStatus locsegChainTraceTestLegacyLike(/*nullable*/ const LocalNeuroseg* locseg,
                                                         /*nullable*/ const LocsegChain* chain,
                                                         const TraceWorkspace& tw,
                                                         /*nullable*/ TraceRecord* tr,
                                                         double zScale,
                                                         double maxR,
                                                         TraceDirection traceDirection,
                                                         double minR,
                                                         /*nullable*/ const LocalNeuroseg* prevLocseg,
                                                         const ZImg& stack);

// Port of tz_locseg_chain.c::Trace_Locseg().
void traceLocsegLegacyLike(const ZImg& stack, double zScale, LocsegChain& chain, TraceWorkspace& tw);

} // namespace nim::neutube
