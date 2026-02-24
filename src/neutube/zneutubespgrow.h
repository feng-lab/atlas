#pragma once

#include "zimg.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nim::neutube {

enum : uint8_t
{
  SP_GROW_TARGET = 1,
  SP_GROW_SOURCE = 2,
  SP_GROW_BARRIER = 3,
  SP_GROW_CONDUCTOR = 4
};

struct SpGrowWorkspace
{
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
void stackSpGrow(const ZImg& stack, SpGrowWorkspace* sgw);

} // namespace nim::neutube
