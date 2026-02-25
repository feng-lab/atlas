#pragma once

#include "zswc.h"

#include <array>

namespace nim::neutube {

struct SwcPointDistResult
{
  double dist = -1.0;
  std::array<double, 3> closestPoint = {0.0, 0.0, 0.0};
  ZSwc::SwcTreeNode closestNode;
};

// Port of `Swc_Tree_Point_Dist` using a straight frustum segment model per edge.
//
// Returns:
// - dist == -1 and closestNode == null when the tree has no segments.
// - dist == 0 when the point is inside any segment volume.
[[nodiscard]] SwcPointDistResult swcTreePointDist(ZSwc& tree, double x, double y, double z);

// Variant that ignores segments belonging to `excludeRoot` (used to match legacy connect-branch behavior,
// where the new branch is not part of the destination tree during distance queries).
[[nodiscard]] SwcPointDistResult
swcTreePointDist(ZSwc& tree, double x, double y, double z, const ZSwc::SwcTreeNode& excludeRoot);

} // namespace nim::neutube
