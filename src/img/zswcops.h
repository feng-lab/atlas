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

} // namespace nim
