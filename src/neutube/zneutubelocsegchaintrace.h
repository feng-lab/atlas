#pragma once

#include "zneutubelocsegchain.h"
#include "zneutubetraceworkspace.h"

namespace nim::neutube {

// Port of tz_locseg_chain.c::Locseg_Chain_Trace_Test().
[[nodiscard]] TraceStatus locsegChainTraceTestLegacyLike(const LocalNeuroseg* locseg,
                                                         const LocsegChain* chain,
                                                         const TraceWorkspace* tw,
                                                         TraceRecord* tr,
                                                         double zScale,
                                                         double maxR,
                                                         TraceDirection traceDirection,
                                                         double minR,
                                                         const LocalNeuroseg* prevLocseg,
                                                         const ZImg& stack);

// Port of tz_locseg_chain.c::Trace_Locseg().
void traceLocsegLegacyLike(const ZImg& stack, double zScale, LocsegChain* chain, TraceWorkspace* tw);

} // namespace nim::neutube
