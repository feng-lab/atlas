#pragma once

#include "zswc.h"

#include <array>

namespace nim {

struct SwcPointDistResult
{
  double dist = -1.0;
  std::array<double, 3> closestPoint = {0.0, 0.0, 0.0};
  // Mutable handle into the queried tree (used by callers to reconnect/insert nodes).
  ZSwc::SwcTreeNode closestNode;
};

// Port of `Swc_Tree_Hit_Test`:
// - Hits when the point is inside any node sphere (radius) OR any segment frustum volume.
// - Ignores virtual nodes (legacy semantics: node.id < 0).
[[nodiscard]] bool swcTreeHitTest(const ZSwc& tree, double x, double y, double z);

// Port of `Swc_Tree_Point_Dist` using a straight frustum segment model per edge.
//
// Returns:
// - dist == -1 and closestNode == null when the tree has no segments.
// - dist == 0 when the point is inside any segment volume.
//
// Note: This function does not mutate `tree`, but it takes `ZSwc&` because the legacy algorithm returns a
// mutable node handle (`closestNode`) which is typically used immediately for tree mutations (e.g. addBreak/connect).
[[nodiscard]] SwcPointDistResult swcTreePointDist(ZSwc& tree, double x, double y, double z);

// Variant that ignores segments belonging to `excludeRoot` (used to match legacy connect-branch behavior,
// where the new branch is not part of the destination tree during distance queries).
[[nodiscard]] SwcPointDistResult
swcTreePointDist(ZSwc& tree, double x, double y, double z, const ZSwc::SwcTreeNode& excludeRoot);

} // namespace nim
