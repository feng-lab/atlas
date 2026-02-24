#include "zneutubetraceworkspace.h"

#include "zimg.h"
#include "zlog.h"

#include <cmath>

namespace nim::neutube {

namespace {

[[nodiscard]] int iroundLegacyLike(double x)
{
  return static_cast<int>(std::lround(x));
}

} // namespace

int traceWorkspaceMaskValueLegacyLike(const TraceWorkspace& tw, const std::array<double, 3>& pos)
{
  if (!tw.traceMask) {
    return 0;
  }

  const ZImg& mask = *tw.traceMask;
  const int x = iroundLegacyLike(pos[0]);
  const int y = iroundLegacyLike(pos[1]);
  const int z = iroundLegacyLike(pos[2]);

  if (x < 0 || y < 0 || z < 0) {
    return 0;
  }

  const size_t width = mask.width();
  const size_t height = mask.height();
  const size_t depth = mask.depth();

  // Legacy bounds check:
  //   z < trace_mask->width   (bug: should be depth)
  if (static_cast<size_t>(x) >= width || static_cast<size_t>(y) >= height || static_cast<size_t>(z) >= width) {
    return 0;
  }

  // Prevent undefined reads if the legacy bug is ever triggered.
  CHECK(static_cast<size_t>(z) < depth) << "Legacy Trace_Workspace_Mask_Value bounds bug triggered (z < width)."
                                        << " mask=" << mask.info() << " x=" << x << " y=" << y << " z=" << z;

  if (mask.isType<uint16_t>()) {
    return static_cast<int>(
      *mask.data<uint16_t>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)));
  }
  if (mask.isType<uint8_t>()) {
    return static_cast<int>(
      *mask.data<uint8_t>(static_cast<size_t>(x), static_cast<size_t>(y), static_cast<size_t>(z)));
  }

  CHECK(false) << "Unsupported trace mask voxel type (expected uint16/uint8): " << mask.info();
  return 0;
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

} // namespace nim::neutube
