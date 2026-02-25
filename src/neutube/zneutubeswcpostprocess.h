#pragma once

#include "zswc.h"

namespace nim::neutube {

// Ports of the SWC postprocess passes applied by legacy `ZNeuronTracer::trace(Stack*, ...)`:
// - Swc_Tree_Remove_Zigzag
// - Swc_Tree_Tune_Branch
// - Swc_Tree_Remove_Spur
// - Swc_Tree_Merge_Close_Node
// - Swc_Tree_Remove_Overshoot
//
// These operate directly on `ZSwc` using legacy node-type semantics:
// - virtual node: node.id < 0
// - root: a node with no regular ancestor (it may have a virtual parent).
void swcTreeRemoveZigzagLegacyLike(ZSwc* tree);
void swcTreeTuneBranchLegacyLike(ZSwc* tree);
void swcTreeRemoveSpurLegacyLike(ZSwc* tree);
void swcTreeMergeCloseNodeLegacyLike(ZSwc* tree, double threshold);
void swcTreeRemoveOvershootLegacyLike(ZSwc* tree);

// Port of `ZSwcPruner::removeOrphanBlob` for the v2 tracer output.
// For parity with the legacy tracer, callers should pass `minLength=0.0`.
void swcTreeRemoveOrphanBlobLegacyLike(ZSwc* tree, double minLength, int minOrphanCount);

} // namespace nim::neutube
