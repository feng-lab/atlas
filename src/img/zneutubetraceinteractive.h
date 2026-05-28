#pragma once

#include "zneutubetraceconfig.h"

#include "zimg.h"
#include "zswc.h"
#include "zfolly.h"

#include <array>
#include <cstddef>
#include <memory>

namespace nim {

class ZVoxelVolume;

struct SeedTraceResult
{
  // For "trace new": a newly created SWC (typically a single-root chain).
  // For "trace into host": the full host SWC after appending/connecting the new branch.
  std::unique_ptr<ZSwc> swc;

  // Number of newly created SWC nodes along the traced branch (0 when no trace result was produced).
  size_t newNodes = 0;
};

// In-memory seeded tracing (GUI-facing).
//
// These are ports of the legacy neuTube interactive seeded-trace behavior, but expressed purely in terms of
// Atlas-native types (`ZImg`, `ZSwc`). The caller is responsible for loading images, selecting a trace target, and
// applying results to the document model/undo stack.
//
// Inputs:
// - `signal`: image volume (may be multi-channel/time; tracing uses the selected `c`/`t`)
// - `position`: seed in image voxel coordinates (x,y,z). Fractional x/y are allowed; z is typically an integer slice.
// - `cfg`: already-resolved trace configuration (including any level overrides)
// - `zToXYRatio`: tracing anisotropy in voxel coordinates (`voxelSizeZ / voxelSizeXY`).
//
// Output:
// - `SeedTraceResult::swc` is null when no branch was produced.
// - For the host-SWC variant, `SeedTraceResult::swc` is always non-null (returns a copy of `hostSwc` when no-op).
[[nodiscard]] SeedTraceResult
traceSeedNewSwcLegacyLike(const ZImg& signal,
                          const std::array<double, 3>& position,
                          const TraceConfig& cfg,
                          double zToXYRatio,
                          size_t c = 0,
                          size_t t = 0,
                          folly::CancellationToken cancellationToken = folly::CancellationToken());

// Overload for read-only voxel volumes (single-channel/time view).
[[nodiscard]] SeedTraceResult
traceSeedNewSwcLegacyLike(const ZVoxelVolume& signal,
                          const std::array<double, 3>& position,
                          const TraceConfig& cfg,
                          double zToXYRatio,
                          folly::CancellationToken cancellationToken = folly::CancellationToken());

[[nodiscard]] SeedTraceResult
traceSeedIntoHostSwcLegacyLike(const ZImg& signal,
                               const ZSwc& hostSwc,
                               const std::array<double, 3>& position,
                               const TraceConfig& cfg,
                               double zToXYRatio,
                               size_t c = 0,
                               size_t t = 0,
                               folly::CancellationToken cancellationToken = folly::CancellationToken());

// Overload for read-only voxel volumes (single-channel/time view).
[[nodiscard]] SeedTraceResult
traceSeedIntoHostSwcLegacyLike(const ZVoxelVolume& signal,
                               const ZSwc& hostSwc,
                               const std::array<double, 3>& position,
                               const TraceConfig& cfg,
                               double zToXYRatio,
                               folly::CancellationToken cancellationToken = folly::CancellationToken());

} // namespace nim
