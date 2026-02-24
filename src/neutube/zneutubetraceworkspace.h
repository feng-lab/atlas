#pragma once

#include <array>
#include <memory>

namespace nim {

class ZImg;

}

namespace nim::neutube {

// C++ port of the legacy `Trace_Workspace` fields used by the tracer.
// This is intentionally minimal at first and expanded incrementally as the trace
// algorithm is migrated off neurolabi C code.
struct TraceWorkspace
{
  std::array<double, 6> traceRange = {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
  std::unique_ptr<ZImg> traceMask;
};

// Port of legacy tz_trace_utils.c::Trace_Workspace_Mask_Value().
//
// Notes:
// - Preserves legacy rounding semantics via `std::lround`.
// - Preserves the legacy bounds check bug (`z < mask.width()` instead of `z < mask.depth()`),
//   but adds a hard CHECK to prevent undefined memory reads if the bug is ever triggered.
[[nodiscard]] int traceWorkspaceMaskValueLegacyLike(const TraceWorkspace& tw, const std::array<double, 3>& pos);

// Port of legacy tz_trace_utils.c::Trace_Workspace_Point_In_Bound().
[[nodiscard]] bool traceWorkspacePointInBoundLegacyLike(const TraceWorkspace& tw, const std::array<double, 3>& pos);

} // namespace nim::neutube
