#include "zneutubetraceworkspace.h"

#include "zneutubemathutils.h"
#include "zvoxelvolume.h"

#include "zlog.h"

#include <cmath>

namespace nim {

void defaultTraceWorkspaceLegacyLike(TraceWorkspace& tw)
{
  // Port of tz_trace_utils.c::Default_Trace_Workspace().
  tw.length = 5000;
  tw.fitFirst = false;
  tw.refit = true;
  tw.breakRefit = false;
  tw.tuneEnd = false;
  tw.tscoreOption = static_cast<int>(StackFitOption::Corrcoef);
  tw.traceStep = 0.5;
  tw.segLength = NeurosegDefaultHLegacyLike;
  tw.minScore = 0.3; // LOCAL_NEUROSEG_MIN_CORRCOEF
  tw.traceStatus = {TraceStatus::Normal, TraceStatus::Normal};
  tw.minChainLength = NeurosegDefaultHLegacyLike * 2.5;

  tw.traceRange = {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
  tw.dyvar = {-1.0, -1.0, -1.0, -1.0, -1.0};

  tw.supStack = nullptr;
  tw.traceMask.reset();
  tw.traceMaskVolume.reset();
  tw.swcMask = nullptr;
  tw.traceMaskUpdating = true;

  tw.resolution = {-1.0, -1.0, -1.0};

  tw.addHit = true;

  defaultLocsegFitWorkspaceLegacyLike(tw.fitWorkspace);
}

void locsegChainDefaultTraceWorkspaceLegacyLike(TraceWorkspace& tw)
{
  // Port of tz_locseg_chain.c::Locseg_Chain_Default_Trace_Workspace().
  defaultTraceWorkspaceLegacyLike(tw);

  tw.dyvar[0] = 25.0; // max radius
  tw.dyvar[2] = -1.0; // height-fit threshold
}

void locsegChainDefaultTraceWorkspaceLegacyLike(TraceWorkspace& tw, const ZImg& stack)
{
  locsegChainDefaultTraceWorkspaceLegacyLike(tw);

  tw.traceRange[0] = 0.0;
  tw.traceRange[1] = 0.0;
  tw.traceRange[2] = 0.0;
  tw.traceRange[3] = static_cast<double>(stack.width()) - 1.0;
  tw.traceRange[4] = static_cast<double>(stack.height()) - 1.0;
  tw.traceRange[5] = static_cast<double>(stack.depth()) - 1.0;
}

void locsegChainDefaultTraceWorkspaceLegacyLike(TraceWorkspace& tw, const ZVoxelVolume& stack)
{
  locsegChainDefaultTraceWorkspaceLegacyLike(tw);

  tw.traceRange[0] = 0.0;
  tw.traceRange[1] = 0.0;
  tw.traceRange[2] = 0.0;
  tw.traceRange[3] = static_cast<double>(stack.width()) - 1.0;
  tw.traceRange[4] = static_cast<double>(stack.height()) - 1.0;
  tw.traceRange[5] = static_cast<double>(stack.depth()) - 1.0;
}

void traceWorkspaceSetTraceStatusLegacyLike(TraceWorkspace& tw, TraceStatus headStatus, TraceStatus tailStatus)
{
  // Port of tz_trace_utils.c::Trace_Workspace_Set_Trace_Status().
  tw.traceStatus[0] = headStatus;
  tw.traceStatus[1] = tailStatus;
}

void traceWorkspaceInitTraceMaskLegacyLike(TraceWorkspace& tw, const ZImg& stack, bool clearing)
{
  // Port of `ZNeuronTracer::initTraceMask(bool clearing)`.
  if (stack.isEmpty()) {
    return;
  }

  if (!tw.traceMask) {
    // Allocate an "already traced" mask for the tracing workspace.
    //
    // Legacy NeuTu stores per-chain region IDs in a GREY16 label image. Atlas does not
    // support chain-ID semantics, and the migrated algorithm only queries mask voxels
    // as a boolean (>0). Use uint8 to save memory on large datasets.
    ZImgInfo info(stack.width(), stack.height(), stack.depth(), 1, 1, 1, VoxelFormat::Unsigned);
    info.setVoxelFormat<uint8_t>();
    info.createDefaultDescriptions();
    tw.traceMask = std::make_unique<ZImg>(info);
    clearing = true;
  }

  if (clearing) {
    tw.traceMask->fill(0);
  }
}

int traceWorkspaceMaskValueLegacyLike(const TraceWorkspace& tw, const std::array<double, 3>& pos)
{
  const int x = iroundLegacyLike(pos[0]);
  const int y = iroundLegacyLike(pos[1]);
  const int z = iroundLegacyLike(pos[2]);

  if (x < 0 || y < 0 || z < 0) {
    return 0;
  }

  if (tw.traceMaskVolume) {
    const ZVoxelVolume& mask = *tw.traceMaskVolume;

    const size_t width = mask.width();
    const size_t height = mask.height();
    const size_t depth = mask.depth();

    if (static_cast<size_t>(x) >= width || static_cast<size_t>(y) >= height || static_cast<size_t>(z) >= depth) {
      return 0;
    }

    const double v = mask.valueAsDouble(x, y, z);
    return static_cast<int>(v);
  }

  if (!tw.traceMask) {
    return 0;
  }

  const ZImg& mask = *tw.traceMask;

  const size_t width = mask.width();
  const size_t height = mask.height();
  const size_t depth = mask.depth();

  if (static_cast<size_t>(x) >= width || static_cast<size_t>(y) >= height || static_cast<size_t>(z) >= depth) {
    return 0;
  }

  return static_cast<int>(*mask.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)));
}

int traceWorkspaceMaskValueZLegacyLike(const TraceWorkspace& tw, std::array<double, 3> pos, double zScale)
{
  // Port of tz_trace_utils.c::Trace_Workspace_Mask_Value_Z().
  pos[2] *= zScale;
  return traceWorkspaceMaskValueLegacyLike(tw, pos);
}

bool traceWorkspacePointInBoundLegacyLike(const TraceWorkspace& tw, const std::array<double, 3>& pos)
{
  if (pos[0] >= 0.0 && pos[1] >= 0.0 && pos[2] >= 0.0) {
    for (size_t i = 0; i < 3; ++i) {
      if (tw.traceRange[i] >= 0.0 && pos[i] < tw.traceRange[i]) {
        return false;
      }
      if (tw.traceRange[i + 3] >= 0.0 && pos[i] > tw.traceRange[i + 3]) {
        return false;
      }
    }
  }

  return true;
}

bool traceWorkspacePointInBoundZLegacyLike(const TraceWorkspace& tw, std::array<double, 3> pos, double zScale)
{
  // Port of tz_trace_utils.c::Trace_Workspace_Point_In_Bound_Z().
  pos[2] *= zScale;
  return traceWorkspacePointInBoundLegacyLike(tw, pos);
}

} // namespace nim
