#pragma once

#include "zneutubestackgraph.h"

#include <array>
#include <cstdint>
#include <vector>

namespace nim::neutube {

// Port of tz_stack_graph.c::Stack_Route().
//
// Returns a list of voxel indices in the original `stack` (start..end).
// For parity with the legacy implementation, the returned list may contain `-1`
// entries when the shortest-path traverses "virtual" group vertices (see
// `Stack_Graph_W`'s `group_mask` behavior). Callers that require only valid stack
// voxel indices should filter to `[0, stack.voxelNumber())`.
// When no path exists, returns an empty vector and sets `sgw->value` to +inf.
[[nodiscard]] std::vector<int64_t> stackRouteLegacyLike(const ZImg& stack,
                                                        const std::array<int, 3>& startPos,
                                                        const std::array<int, 3>& endPos,
                                                        StackGraphWorkspaceLegacyLike& sgw);

} // namespace nim::neutube
