#pragma once

#include "zswc.h"

namespace nim {

enum class SwcMergeOption
{
  MergeWithParent,
  MergeWithChild,
  MergeAverage,
  MergeWeightedAverage
};

// Port of `Swc_Tree_Node_Merge_To_Parent` semantics for the regular-node case.
// Removes `node` from the tree and splices its children into its parent.
void mergeToParent(ZSwc& swc, ZSwc::SwcTreeNode node);

// Same as mergeToParent, but updates the parent's geometric properties before merging,
// matching `SwcTreeNode::mergeToParent(..., option)` used by legacy `ZSwcResampler`.
void mergeToParent(ZSwc& swc, ZSwc::SwcTreeNode node, SwcMergeOption option);

// Port of `Swc_Tree_Node_Add_Break(tn, lambda)` for regular nodes.
// Inserts a new node between `node` and its parent at position `lambda` along the segment.
// Returns:
// - parent(node) if lambda <= 0
// - node if lambda >= 1 or node is root
// - the inserted break node otherwise
[[nodiscard]] ZSwc::SwcTreeNode addBreak(ZSwc& swc, ZSwc::SwcTreeNode node, double lambda);

// Port of `Swc_Tree_Connect_Node(tree, tn)`.
// Connects `node` to the closest point on `tree`, possibly inserting a break node.
// `node` is expected to be a root (in the same forest as `tree`).
void connectNode(ZSwc& tree, ZSwc::SwcTreeNode node);

// Port of `Swc_Tree_Connect_Branch(tree, start_tn)`.
// Connects the branch (a single chain) into `tree` by attaching the closer endpoint.
// Returns the endpoint node that got connected (matching the legacy API).
[[nodiscard]] ZSwc::SwcTreeNode connectBranch(ZSwc& tree, ZSwc::SwcTreeNode startNode);

// Port of `Swc_Tree_Resort_Id`.
// Assigns sequential IDs in depth-first order and updates each node's parentID field.
// Returns the number of regular nodes assigned.
int resortId(ZSwc& tree);

// Port of `Swc_Tree_Node_Set_Root(tn)`.
//
// Notes:
// - Matches NeuTu's definition of "root": a node with no *regular* parent above it.
//   (Virtual ancestors are allowed.)
// - Preserves legacy weight-shifting semantics while reversing parent links.
void swcTreeNodeSetRootLegacyLike(ZSwc& tree, ZSwc::SwcTreeNode node);

// Port of `Swc_Tree_Regularize(tree)` as used by the neuTube reconstruction path.
//
// Removes all virtual nodes (id < 0) by merging them into their parent. In Atlas' ZSwc
// representation there is no explicit virtual root node; so virtual roots are removed
// by promoting their children to roots.
void swcTreeRegularizeLegacyLike(ZSwc& tree);

// Port of `Swc_Tree_Subtract(tree1, tree2)`:
// Removes any node in `tree1` whose position hits `tree2` (node sphere OR segment volume),
// and detaches the cut node's descendants as new roots (matching `Swc_Tree_Cut_Node` behavior).
void subtractSwcTrees(ZSwc& tree1, const ZSwc& tree2);

} // namespace nim
