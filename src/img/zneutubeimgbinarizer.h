#pragma once

#include "zimg.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace nim {

struct BinarizeLocmaxDiagnosticsLegacyLike
{
  bool collectTiming = false;

  // Coarse timings (milliseconds).
  std::int64_t ms_hist_full = 0;
  std::int64_t ms_locmax_hist = 0;
  std::int64_t ms_threshold = 0;
  std::int64_t ms_threshold_binarize = 0;

  // Fine-grained timings inside `computeLocmaxHistLegacyLike()` (milliseconds).
  std::int64_t ms_locmax_region_mask = 0;
  std::int64_t ms_locmax_seed_select = 0;
  std::int64_t ms_locmax_hist_masked = 0;

  // Basic counters for sanity/debug.
  size_t locmax_region_nonzero = 0;
  size_t locmax_seed_nonzero = 0;
};

struct BinarizeResultLegacyLike
{
  ZImg binary; // GREY (uint8) with values 0/1
  bool success = false;
  int actualThreshold = 0;
};

// Port of ZStackProcessor::SubtractBackground(Stack*, double, int).
//
// Returns the subtracted background intensity (0 means no-op), matching legacy.
//
// Notes on voxel types:
// - For uint8/uint16, this follows legacy exactly by building exact-intensity histograms (256/65536 bins) and applying
//   the same mode/foreground-ratio iteration.
// - For other voxel types, it builds a binned histogram over the current data range, runs the same logic on bin
// indices,
//   then converts the chosen bin back into an intensity value (via `ZImg::binRange`) before subtracting. This avoids
//   UB for out-of-range float->int casts and avoids allocating (max-min) bins for wide-range integer types.
int subtractBackgroundLegacyLike(ZImg& stack, double minFr, int maxIter);

// Port of ZNeuronTracer::binarize(const Stack*, Stack*).
//
// Returns a GREY (uint8) binary stack (values 0/1) on success.
BinarizeResultLegacyLike
binarizeLocmaxLegacyLike(const ZImg& stack, int retryCount, /*nullable*/ BinarizeLocmaxDiagnosticsLegacyLike* diag);

} // namespace nim
