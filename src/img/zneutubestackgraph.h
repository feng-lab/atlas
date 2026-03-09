#pragma once

#include "zimg.h"

#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace nim {

// Port of tz_stack_graph.h::Stack_Graph_Workspace, restricted to fields used by tracing.
//
// Notes:
// - `signalMask` is a non-owning pointer (legacy code carefully nulls it out before
//   destroying the workspace when it aliases an external mask).
// - `groupMask` is owned by the workspace and allocated lazily by the shortest-path helpers.
struct StackGraphWorkspaceLegacyLike
{
  // Legacy callback signature: `argv` points to a small fixed-layout array.
  // It is treated as read-only by our ports, but must remain a pointer to match
  // the legacy function-pointer type.
  using WeightFunc = double (*)(double* argv);

  int conn = 26;

  // Range is inclusive (x0..x1, y0..y1, z0..z1) in voxel coordinates.
  // When not set, Stack_Route computes a range based on start/end and margins.
  std::optional<std::array<int, 6>> range;

  // Neighbor step lengths used when converting voxel offsets to edge lengths.
  //
  // This is a normalized routing metric, not raw voxel metadata. In tracing code it is usually
  // `{1, 1, zToXYRatio}`, which preserves the caller-selected relative Z-vs-XY spacing while keeping XY in the routing
  // baseline unit. Other clients may provide different explicit spacings, but callers must not infer a tracing
  // `zToXYRatio` from image metadata inside inner loops.
  std::array<double, 3> resolution = {1.0, 1.0, 1.0};
  WeightFunc weightFunc = nullptr;
  int spOption = 0;

  // Arguments passed to `weightFunc` (legacy uses STACK_GRAPH_WORKSPACE_ARGC=10).
  // - argv[0]=d, argv[1]=v1, argv[2]=v2
  // - argv[3]=thre, argv[4]=scale for Stack_Voxel_Weight_S
  std::array<double, 10> argv = {std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::quiet_NaN()};

  std::optional<ZImg> groupMask;
  const ZImg* signalMask = nullptr;

  double value = 0.0;
  int virtualVertex = -1;
  bool includingSignalBorder = false;

  double greyFactor = 1.0;
  double greyOffset = 0.0;
};

void defaultStackGraphWorkspaceLegacyLike(StackGraphWorkspaceLegacyLike& sgw);

void stackGraphWorkspaceSetRangeLegacyLike(StackGraphWorkspaceLegacyLike& sgw,
                                           int x0,
                                           int x1,
                                           int y0,
                                           int y1,
                                           int z0,
                                           int z1);

void stackGraphWorkspaceUpdateRangeLegacyLike(StackGraphWorkspaceLegacyLike& sgw, int x, int y, int z);

void stackGraphWorkspaceExpandRangeLegacyLike(StackGraphWorkspaceLegacyLike& sgw,
                                              int mx0,
                                              int mx1,
                                              int my0,
                                              int my1,
                                              int mz0,
                                              int mz1);

void stackGraphWorkspaceValidateRangeLegacyLike(StackGraphWorkspaceLegacyLike& sgw, int width, int height, int depth);

// Port of tz_stack_graph.c::Stack_Voxel_Weight_S().
// argv layout: [0]=d, [1]=v1, [2]=v2, [3]=thre, [4]=scale.
// `argv` must be non-null and point to at least 5 doubles (see layout above).
[[nodiscard]] double stackVoxelWeightSLegacyLike(double* argv);

// Port of tz_stack_graph.c::Stack_Voxel_Weight_Sr() (for bright-background images).
// argv layout: [0]=d, [1]=v1, [2]=v2, [3]=thre, [4]=scale.
// `argv` must be non-null and point to at least 5 doubles (see layout above).
[[nodiscard]] double stackVoxelWeightSrLegacyLike(double* argv);

} // namespace nim
