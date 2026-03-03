#pragma once

#include "zneutubetraceconfig.h"
#include "zneutubeimgbinarizer.h"

#include "zimg.h"

#include <optional>

namespace nim {

struct MakeMaskDiagnosticsLegacyLike
{
  int binarizeThreshold = 0;

  // Optional timings/counters to help profile mask generation in tests.
  // Keep this disabled by default to avoid overhead in production use.
  bool collectTiming = false;
  std::int64_t ms_binarize_locmax = 0;
  std::int64_t ms_bwsolid_majority_filter_r = 0;

  BinarizeLocmaxDiagnosticsLegacyLike binarizeDiag;
};

// Port of `ZNeuronTracer::makeMask` (excluding optional thin-branch enhancement).
//
// - Input `stack` must be single-channel, single-time, already preprocessed (background subtraction, etc).
// - Returns a GREY (uint8) binary mask with values {0,1} on success.
// - Returns nullopt on legacy "thresholding failed" conditions.
// - `diag` is optional; when provided, receives legacy diagnostic values such as
//   the chosen binarization threshold (used by trace CLI logging/tests).
[[nodiscard]] std::optional<ZImg>
makeMaskLegacyLike(const ZImg& stack, const TraceConfig& cfg, /*nullable*/ MakeMaskDiagnosticsLegacyLike* diag);

} // namespace nim
