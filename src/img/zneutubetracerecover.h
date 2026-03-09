#pragma once

#include "zneutubetraceconfig.h"
#include "zneutubetraceworkspace.h"

#include "zneutubelocsegchain.h"

#include "zimg.h"

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace nim {

using RecoverChainEndStatusLegacyLike = std::array<TraceStatus, 2>;

struct RecoverResultLegacyLike
{
  std::vector<std::unique_ptr<LocsegChain>> chains;
  std::vector<RecoverChainEndStatusLegacyLike> chainEndStatuses;
  std::optional<ZImg> baseMask;
};

// Port of `ZNeuronTracer::recover(const Stack*)`.
//
// Inputs:
// - `signal`: preprocessed intensity stack.
// - `mask`: the binary tracing mask produced by `makeMask` (legacy `m_mask`).
// - `baseMask`: output of seed sorting (legacy `m_baseMask`), consumed and replaced.
//
// Effects:
// - Updates `tw->traceMask` by tracing additional chains, like the legacy code.
[[nodiscard]] RecoverResultLegacyLike recoverLegacyLike(const ZImg& signal,
                                                        const TraceConfig& cfg,
                                                        double zToXYRatio,
                                                        const ZImg& mask,
                                                        std::optional<ZImg> baseMask,
                                                        TraceWorkspace& tw);

} // namespace nim
