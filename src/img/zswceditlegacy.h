#pragma once

#include "zswc.h"

#include <optional>
#include <vector>

namespace nim {

// Legacy neuTube semantics for deleting SWC nodes:
// - The deleted node is removed.
// - All of its children become new roots (i.e. detached to the forest root in neuTube).
//
// Note: `toDelete` must contain only nodes that belong to `swc`. Callers typically
// build this list in post-order to avoid iterator invalidation hazards.
void deleteNodesLegacyLike(ZSwc& swc, const std::vector<ZSwc::SwcTreeNode>& toDelete);

struct SwcConnectSelectedNodesOptionsLegacyLike
{
  // Matches `ZSwcConnector::useSurfaceDist(true)` in neuTube ConnectSwcNode.
  bool useSurfaceDistance = true;

  // Matches `ZSwcConnector::setResolution(doc->getResolution())`.
  // Atlas SWC editing currently does not have a single canonical resolution source,
  // so callers can optionally provide voxel sizes when available.
  double voxelSizeX = 1.0;
  double voxelSizeY = 1.0;
  double voxelSizeZ = 1.0;

  // Matches `nodeNumberThreshold` constant in neuTube (`ConnectSwcNode` constructor).
  int nodeNumberThreshold = 500;
};

// Legacy neuTube semantics for "Connect" (context menu, selected nodes):
// - Builds a proximity graph using surface distance (with the resolution scaling above).
// - Computes a minimum spanning forest and keeps only non-zero edges (between different trees).
// - Breadth-first orders the remaining edges and orients them, then attaches each "down" tree to its parent.
//
// Returns true if any connections were made.
[[nodiscard]] bool connectSelectedNodesLegacyLike(ZSwc& swc,
                                                  const std::vector<ZSwc::SwcTreeNode>& selectedNodes,
                                                  const SwcConnectSelectedNodesOptionsLegacyLike& opt = {});

// Legacy neuTube semantics for "Merge" (context menu, selected nodes).
//
// Returns the newly created core node when a merge happens, otherwise nullopt.
[[nodiscard]] std::optional<ZSwc::SwcTreeNode>
mergeSelectedNodesLegacyLike(ZSwc& swc, const std::vector<ZSwc::SwcTreeNode>& selectedNodes);

struct SwcNodeGeometryLegacyLike
{
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double radius = 0.0;
};

// Legacy neuTube semantics for "Advanced Editing → Remove turn".
//
// When a sharp turn is detected at `node` (either as a continuation node, or by choosing
// the most obtuse parent-child neighbor pair), this returns the updated geometry for `node`.
//
// Returns nullopt when no turn is detected / nothing should be changed.
[[nodiscard]] std::optional<SwcNodeGeometryLegacyLike>
computeRemoveTurnGeometryLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& node);

// Legacy neuTube semantics for "Advanced Editing → Reset branch point".
//
// Returns true if any parent links were changed.
[[nodiscard]] bool resetBranchPointLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& loopNode);

// Legacy neuTube semantics for "Advanced Editing → Join isolated branch" (aka "Set branch point").
//
// When `branchPoint` is selected, find the nearest node in a *different* tree (under the same virtual master root)
// and re-parent it under `branchPoint`, creating a new branch point.
//
// Returns true if a connection was made.
[[nodiscard]] bool joinIsolatedBranchLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& branchPoint);

// Legacy neuTube semantics for "Advanced Editing → Resolve crossover".
//
// When `center` is selected, matches its neighbors into pairs using a minimum-weight perfect matching
// (weighted by straight-through angles), then rewires parents to resolve crossover topology.
//
// Returns true if any parent links were changed.
[[nodiscard]] bool resolveCrossoverLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& center);

} // namespace nim
