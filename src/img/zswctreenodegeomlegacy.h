#pragma once

#include "zswc.h"

#include <vector>

namespace nim {

// Ported helpers matching legacy `SwcTreeNode` geometric predicates used by NeuTu tracing.

[[nodiscard]] double swcNodeDistanceLegacyLike(const ZSwc::ConstSwcTreeNode& a,
                                               const ZSwc::ConstSwcTreeNode& b,
                                               double sx = 1.0,
                                               double sy = 1.0,
                                               double sz = 1.0);

[[nodiscard]] double swcNodeSurfaceDistanceLegacyLike(const ZSwc::ConstSwcTreeNode& a,
                                                      const ZSwc::ConstSwcTreeNode& b,
                                                      double sx = 1.0,
                                                      double sy = 1.0,
                                                      double sz = 1.0);

[[nodiscard]] bool swcNodesHasOverlapLegacyLike(const ZSwc::ConstSwcTreeNode& a, const ZSwc::ConstSwcTreeNode& b);

[[nodiscard]] bool swcNodesHasSignificantOverlapLegacyLike(const ZSwc::ConstSwcTreeNode& a,
                                                           const ZSwc::ConstSwcTreeNode& b);

// Port of `Swc_Tree_Node_Forming_Turn(tn1, tn2, tn3)`.
// Returns true when (tn1, tn2, tn3) are consecutive along parent links and their dot product <= 0.
[[nodiscard]] bool swcNodesFormingTurnLegacyLike(const ZSwc::ConstSwcTreeNode& tn1,
                                                 const ZSwc::ConstSwcTreeNode& tn2,
                                                 const ZSwc::ConstSwcTreeNode& tn3);

// Returns parent + children of `node` as mutable node handles.
// Note: this helper does not modify `swc`, but it takes `ZSwc&` because it returns `ZSwc::SwcTreeNode`
// (a mutable iterator type) which is commonly used by callers that later mutate/reconnect the tree.
[[nodiscard]] std::vector<ZSwc::SwcTreeNode> swcNeighborArrayLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& node);

void swcNodeAverageLegacyLike(const ZSwc::ConstSwcTreeNode& a, const ZSwc::ConstSwcTreeNode& b, ZSwc::SwcTreeNode out);

void swcNodeInterpolateLegacyLike(const ZSwc::ConstSwcTreeNode& a,
                                  const ZSwc::ConstSwcTreeNode& b,
                                  double lambda,
                                  ZSwc::SwcTreeNode out);

// Port of `SwcTreeNode::correctTurn(tn)`.
// If `tn` forms a turn between its parent/child neighbor pair, this moves `tn` onto the
// interpolated segment position (and radius) between the two neighbors.
void swcNodeCorrectTurnLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& tn);

// Port of `SwcTreeNode::maxBendingEnergy(tn)`.
// This measures local curvature around `tn` using up to 5 consecutive points on the chain.
[[nodiscard]] double swcNodeMaxBendingEnergyLegacyLike(const ZSwc& swc, const ZSwc::ConstSwcTreeNode& tn);

} // namespace nim
