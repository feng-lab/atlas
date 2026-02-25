#pragma once

#include "zneutubetraceconfig.h"

#include "zimg.h"

#include <optional>

namespace nim::neutube {

struct MakeMaskDiagnosticsLegacyLike
{
  int binarizeThreshold = 0;
};

// Port of `ZNeuronTracer::makeMask` (excluding optional thin-branch enhancement).
//
// - Input `stack` must be single-channel, single-time, already preprocessed (background subtraction, etc).
// - Returns a GREY (uint8) binary mask with values {0,1} on success.
// - Returns nullopt on legacy "thresholding failed" conditions.
[[nodiscard]] std::optional<ZImg>
makeMaskLegacyLike(const ZImg& stack, const TraceConfig& cfg, MakeMaskDiagnosticsLegacyLike* diag);

} // namespace nim::neutube
