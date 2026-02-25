#pragma once

#include "zimg.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace nim {

struct SpGrowWorkspace;

enum : uint8_t
{
  SP_GROW_TARGET = 1,
  SP_GROW_SOURCE = 2,
  SP_GROW_BARRIER = 3,
  SP_GROW_CONDUCTOR = 4
};

// Port of legacy `Stack_Voxel_Weight_I` as a `WeightFunc` callback.
// argv layout: [0]=d, [1]=v1, [2]=v2.
// `argv` must be non-null and point to at least 3 doubles (see layout above).
[[nodiscard]] double stackVoxelWeightILegacyLike(double* argv);

// Port of legacy `Stack_Sp_Grow_Infer_Parameter`.
// Populates `sgw->argv[3]` and `sgw->argv[4]` when `sgw->weightFunc` is `stackVoxelWeightSLegacyLike`.
void stackSpGrowInferParameterLegacyLike(SpGrowWorkspace& sgw, const ZImg& stack);

struct SpGrowWorkspace
{
  // Legacy callback signature: `argv` points to a small fixed-layout array.
  // It is treated as read-only by our ports, but must remain a pointer to match
  // the legacy function-pointer type.
  using WeightFunc = double (*)(double* argv);

  size_t size = 0;

  // Buffers (size = number of voxels).
  std::vector<double> dist;
  std::vector<double> length; // optional
  std::vector<int> path;
  std::vector<int> checked;
  std::vector<uint8_t> flag;

  // Mask provided by caller: stored into `flag` before running.
  // Values follow SP_GROW_* constants above.
  std::vector<uint8_t> mask;

  int conn = 26;
  std::array<double, 3> resolution = {1.0, 1.0, 1.0};

  WeightFunc weightFunc = &stackVoxelWeightILegacyLike;
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

  double value = 0.0;
  double fgratio = 0.0;

  // 0 = normal (sum weights), 1 = maxmin (not used by skeletonize).
  int spOption = 0;

  bool lengthBufferEnabled = false;

  int width = 0;
  int height = 0;
  int depth = 0;
};

// Port of legacy `Stack_Sp_Grow(..., sgw)` for the skeletonize use-case:
// - Sources/targets/barriers are provided via `sgw->mask`.
// - No explicit seed/target arrays are supported (matching skeletonize usage).
//
// `stack` may be uint8 or uint16; values are interpreted as doubles for weight calculations.
void stackSpGrow(const ZImg& stack, SpGrowWorkspace& sgw);

// Port of legacy `Stack_Sp_Grow(..., sgw)` for mask-driven usage.
//
// Returns the voxel-index path from source to the first reached target (inclusive).
// If no target is reachable, returns an empty vector.
[[nodiscard]] std::vector<int64_t> stackSpGrowPathLegacyLike(const ZImg& stack, SpGrowWorkspace& sgw);

} // namespace nim
