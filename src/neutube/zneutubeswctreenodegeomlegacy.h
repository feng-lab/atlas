#pragma once

#include "zswc.h"

#include <vector>

namespace nim::neutube {

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

[[nodiscard]] std::vector<ZSwc::SwcTreeNode> swcNeighborArrayLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& node);

void swcNodeAverageLegacyLike(const ZSwc::ConstSwcTreeNode& a, const ZSwc::ConstSwcTreeNode& b, ZSwc::SwcTreeNode out);

void swcNodeInterpolateLegacyLike(const ZSwc::ConstSwcTreeNode& a,
                                  const ZSwc::ConstSwcTreeNode& b,
                                  double lambda,
                                  ZSwc::SwcTreeNode out);

} // namespace nim::neutube
