#pragma once

#include "zneutubelocsegchain.h"
#include "zneutubeneurocompconn.h"
#include "zneutubetraceconnectiontestworkspace.h"

#include "zimg.h"

namespace nim::neutube {

// Port of `tz_locseg_chain.c::Locseg_Chain_Connection_Test()`.
//
// Notes:
// - `chain1` is the hook chain (its end will connect).
// - `chain2` is the loop chain (may be modified by interpolation).
// - `signal` may be null; shortest-path test is skipped when null.
// - `conn` is always written (mode set to None on failure).
[[nodiscard]] bool locsegChainConnectionTestLegacyLike(const LocsegChain& chain1,
                                                       LocsegChain& chain2,
                                                       /*nullable*/ const ZImg* signal,
                                                       double zScale,
                                                       NeurocompConnLegacyLike& conn,
                                                       const ConnectionTestWorkspaceLegacyLike& ctw);

} // namespace nim::neutube
