#pragma once

#include "zneutubelocalneuroseg.h"
#include "zneutubestackfitoptions.h"
#include "zneutubetracedefs.h"

#include "zimg.h"
#include "zvoxelvolume.h"

#include <array>
#include <folly/CancellationToken.h>
#include <memory>

namespace nim {

// C++ port of the legacy `Trace_Workspace` fields used by the tracer.
// This is intentionally minimal at first and expanded incrementally as the trace
// algorithm is migrated off neurolabi C code.
struct TraceWorkspace
{
  int length = 5000;
  bool fitFirst = false;
  bool refit = true;
  bool breakRefit = false;
  bool tuneEnd = false;

  int tscoreOption = static_cast<int>(StackFitOption::Corrcoef);
  double minScore = 0.3;
  double minChainLength = NeurosegDefaultHLegacyLike * 2.5;

  double traceStep = 0.5;
  double segLength = NeurosegDefaultHLegacyLike;

  folly::CancellationToken cancellationToken{};

  // End statuses: [0]=head/backward, [1]=tail/forward (matches Trace_Locseg usage).
  std::array<TraceStatus, 2> traceStatus = {TraceStatus::Normal, TraceStatus::Normal};

  std::array<double, 6> traceRange = {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0};

  const ZImg* supStack = nullptr;
  const ZImg* swcMask = nullptr;

  std::unique_ptr<ZImg> traceMask;
  std::unique_ptr<ZVoxelMaskMutable> traceMaskVolume;
  bool traceMaskUpdating = true;

  // Reserved/dynamic variables (legacy behavior depends on routine).
  std::array<double, 5> dyvar = {-1.0, -1.0, -1.0, -1.0, -1.0};

  // Optional trace-space resolution for size-based checks.
  //
  // Current tracing code only uses the XY entries here; tracing entry points should still set the full
  // `{1, 1, zToXYRatio}` tuple explicitly so the workspace stays aligned with the chosen anisotropy contract.
  std::array<double, 3> resolution = {-1.0, -1.0, -1.0};

  bool addHit = true;

  // Default local-neuroseg fitting workspace (mirrors legacy `fit_workspace`).
  LocsegFitWorkspace fitWorkspace{};
};

// Port of tz_trace_utils.c::Default_Trace_Workspace().
void defaultTraceWorkspaceLegacyLike(TraceWorkspace& tw);

// Port of tz_locseg_chain.c::Locseg_Chain_Default_Trace_Workspace().
void locsegChainDefaultTraceWorkspaceLegacyLike(TraceWorkspace& tw);
void locsegChainDefaultTraceWorkspaceLegacyLike(TraceWorkspace& tw, const ZImg& stack);
void locsegChainDefaultTraceWorkspaceLegacyLike(TraceWorkspace& tw, const ZVoxelVolume& stack);

// Port of legacy tz_trace_utils.c::Trace_Workspace_Set_Trace_Status().
//
// Notes:
// - In the legacy tracer implementation, the array slot semantics are effectively:
//   - index 0: head/backward end
//   - index 1: tail/forward end
//   (despite historical comments that describe the opposite).
void traceWorkspaceSetTraceStatusLegacyLike(TraceWorkspace& tw, TraceStatus headStatus, TraceStatus tailStatus);

// Port of `ZNeuronTracer::initTraceMask(bool clearing)`:
// - Ensures `tw->traceMask` exists (allocates a uint8 binary mask on demand).
// - Zeros the mask if `clearing` is true, or if the mask had to be allocated.
//
// Notes:
// - Atlas treats this as a binary "already traced" mask (0/1). The current migrated algorithm only queries
//   the mask as a boolean (>0), so we do not carry the legacy per-chain region IDs.
void traceWorkspaceInitTraceMaskLegacyLike(TraceWorkspace& tw, const ZImg& stack, bool clearing);

// Port of legacy tz_trace_utils.c::Trace_Workspace_Mask_Value().
//
// Notes:
// - Preserves legacy rounding semantics via the same `iround()` rule used by neurolabi.
[[nodiscard]] int traceWorkspaceMaskValueLegacyLike(const TraceWorkspace& tw, const std::array<double, 3>& pos);

// Port of legacy tz_trace_utils.c::Trace_Workspace_Mask_Value_Z().
[[nodiscard]] int
traceWorkspaceMaskValueZLegacyLike(const TraceWorkspace& tw, std::array<double, 3> pos, double zToXYRatio);

// Port of legacy tz_trace_utils.c::Trace_Workspace_Point_In_Bound().
[[nodiscard]] bool traceWorkspacePointInBoundLegacyLike(const TraceWorkspace& tw, const std::array<double, 3>& pos);

// Port of legacy tz_trace_utils.c::Trace_Workspace_Point_In_Bound_Z().
[[nodiscard]] bool
traceWorkspacePointInBoundZLegacyLike(const TraceWorkspace& tw, std::array<double, 3> pos, double zToXYRatio);

} // namespace nim
