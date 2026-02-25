#include "zneutubeswcpostprocess.h"

#include "zneutubeswcops.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

namespace nim::neutube {

namespace {

template<typename Iter>
[[nodiscard]] bool isVirtualLegacyLike(const Iter& tn)
{
  return !ZSwc::isNull(tn) && tn->id < 0;
}

template<typename Iter>
[[nodiscard]] bool isRegularLegacyLike(const Iter& tn)
{
  return !ZSwc::isNull(tn) && tn->id >= 0;
}

template<typename Iter>
[[nodiscard]] bool isRootLegacyLike(Iter tn)
{
  if (ZSwc::isNull(tn)) {
    return false;
  }

  while (true) {
    const auto parent = ZSwc::parent(tn);
    if (ZSwc::isNull(parent)) {
      return true;
    }
    if (isRegularLegacyLike(parent)) {
      return false;
    }
    tn = parent;
  }
}

template<typename Iter>
[[nodiscard]] Iter regularParentLegacyLike(Iter tn)
{
  auto parent = ZSwc::parent(tn);
  while (!ZSwc::isNull(parent) && !isRegularLegacyLike(parent)) {
    parent = ZSwc::parent(parent);
  }
  return parent;
}

template<typename Iter>
[[nodiscard]] bool isBranchPointLegacyLike(const Iter& tn, ZSwc& tree)
{
  if (!isRegularLegacyLike(tn)) {
    return false;
  }

  // Legacy check: (first_child != NULL) && (first_child->next_sibling != NULL).
  const auto child = ZSwc::firstChild(tn);
  if (ZSwc::isNull(child)) {
    return false;
  }

  auto it = tree.beginChild(tn);
  if (it == tree.endChild(tn)) {
    return false;
  }
  ++it;
  return it != tree.endChild(tn);
}

template<typename Iter>
[[nodiscard]] bool isLeafLegacyLike(const Iter& tn)
{
  if (!isRegularLegacyLike(tn)) {
    return false;
  }
  if (!ZSwc::isNull(ZSwc::firstChild(tn))) {
    return false;
  }
  if (isRootLegacyLike(tn)) {
    return false;
  }
  return true;
}

template<typename Iter>
[[nodiscard]] bool isContinuationLegacyLike(const Iter& tn, ZSwc& tree)
{
  if (!isRegularLegacyLike(tn)) {
    return false;
  }
  if (isRootLegacyLike(tn)) {
    return false;
  }
  if (isLeafLegacyLike(tn)) {
    return false;
  }
  if (isBranchPointLegacyLike(tn, tree)) {
    return false;
  }
  return true;
}

template<typename IterA, typename IterB>
[[nodiscard]] double nodeDistLegacyLike(const IterA& a, const IterB& b)
{
  if (!isRegularLegacyLike(a) || !isRegularLegacyLike(b)) {
    return 0.0;
  }

  const double dx = a->x - b->x;
  const double dy = a->y - b->y;
  const double dz = a->z - b->z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

template<typename Iter>
[[nodiscard]] double nodeLengthLegacyLike(const Iter& tn)
{
  if (!isRegularLegacyLike(tn) || isRootLegacyLike(tn)) {
    return 0.0;
  }

  auto parent = regularParentLegacyLike(tn);
  if (ZSwc::isNull(parent)) {
    return 0.0;
  }

  const double dx = tn->x - parent->x;
  const double dy = tn->y - parent->y;
  const double dz = tn->z - parent->z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline void unitizeLegacyLike(double vec[3])
{
  const double norm = std::sqrt(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2]);
  if (norm > 0.0) {
    vec[0] /= norm;
    vec[1] /= norm;
    vec[2] /= norm;
  }
}

template<typename Iter1, typename Iter2, typename Iter3>
[[nodiscard]] bool formingTurnLegacyLike(const Iter1& tn1, const Iter2& tn2, const Iter3& tn3)
{
  // Port of `Swc_Tree_Node_Forming_Turn`.
  if (!isRegularLegacyLike(tn1) || !isRegularLegacyLike(tn2) || !isRegularLegacyLike(tn3)) {
    return false;
  }

  const auto p2 = ZSwc::parent(tn2);
  const auto p3 = ZSwc::parent(tn3);
  const auto p1 = ZSwc::parent(tn1);
  const bool chainOk = !ZSwc::isNull(p2) && ((!ZSwc::isNull(p3) && p2.node == tn1.node && p3.node == tn2.node) ||
                                             (!ZSwc::isNull(p1) && p2.node == tn3.node && p1.node == tn2.node));
  if (!chainOk) {
    return false;
  }

  double vec1[3] = {tn1->x - tn2->x, tn1->y - tn2->y, tn1->z - tn2->z};
  double vec2[3] = {tn2->x - tn3->x, tn2->y - tn3->y, tn2->z - tn3->z};

  unitizeLegacyLike(vec1);
  unitizeLegacyLike(vec2);

  const double d = vec1[0] * vec2[0] + vec1[1] * vec2[1] + vec1[2] * vec2[2];
  if (d > 0.0) {
    return false;
  }
  return true;
}

template<typename Iter>
[[nodiscard]] bool isTurnLegacyLike(const Iter& tn, ZSwc& tree)
{
  // Port of `Swc_Tree_Node_Is_Turn`.
  if (!isContinuationLegacyLike(tn, tree)) {
    return false;
  }

  const auto parent = ZSwc::parent(tn);
  const auto child = ZSwc::firstChild(tn);
  if (ZSwc::isNull(parent) || ZSwc::isNull(child)) {
    return false;
  }

  return formingTurnLegacyLike(parent, tn, child);
}

template<typename Iter>
[[nodiscard]] bool isOvershootLegacyLike(const Iter& tn, ZSwc& tree)
{
  // Port of `Swc_Tree_Node_Is_Overshoot`.
  if (!isTurnLegacyLike(tn, tree)) {
    return false;
  }

  const auto parent = ZSwc::parent(tn);
  const auto child = ZSwc::firstChild(tn);
  if (ZSwc::isNull(parent) || ZSwc::isNull(child)) {
    return false;
  }

  if (isBranchPointLegacyLike(parent, tree)) {
    if (!isBranchPointLegacyLike(child, tree)) {
      return true;
    }
  } else {
    if (isBranchPointLegacyLike(child, tree)) {
      return true;
    }
  }

  return false;
}

template<typename Iter>
[[nodiscard]] bool isSpurLegacyLike(const Iter& tn, ZSwc& tree)
{
  (void)tree;
  if (!isLeafLegacyLike(tn)) {
    return false;
  }

  const auto parent = ZSwc::parent(tn);
  if (ZSwc::isNull(parent)) {
    return false;
  }

  return isBranchPointLegacyLike(parent, tree);
}

template<typename Iter>
[[nodiscard]] bool isLastChildLegacyLike(const Iter& tn)
{
  if (ZSwc::isNull(tn)) {
    return false;
  }
  if (ZSwc::isNull(ZSwc::parent(tn))) {
    return false;
  }
  return tn.node->nextSibling == nullptr;
}

template<typename Iter>
void setParentLegacyLike(ZSwc& tree, Iter tn, Iter parent)
{
  if (ZSwc::isNull(tn)) {
    return;
  }

  if (ZSwc::isNull(parent)) {
    tree.appendRoot(tn);
    return;
  }

  tree.appendChild(parent, tn);
}

template<typename Iter>
void detachParentRawLegacyLike(Iter pos)
{
  CHECK(!ZSwc::isNull(pos));
  auto* node = pos.node;
  CHECK(node != nullptr);

  if (node->prevSibling != nullptr) {
    node->prevSibling->nextSibling = node->nextSibling;
  }
  if (node->nextSibling != nullptr) {
    node->nextSibling->prevSibling = node->prevSibling;
  }
  if (node->parent != nullptr && node->parent->firstChild == node) {
    node->parent->firstChild = node->nextSibling;
  }
  if (node->parent != nullptr && node->parent->lastChild == node) {
    node->parent->lastChild = node->prevSibling;
  }

  node->parent = nullptr;
  node->prevSibling = nullptr;
  node->nextSibling = nullptr;
}

template<typename Iter>
void replaceChildLegacyLike(ZSwc& tree, Iter oldChild, Iter newChild)
{
  // Port of `Swc_Tree_Node_Replace_Child`.
  CHECK(!ZSwc::isNull(newChild));

  const auto parent = ZSwc::parent(oldChild);
  if (ZSwc::isNull(parent) || oldChild == newChild) {
    return;
  }

  // Detach newChild from its parent/siblings (may be in a different part of the subtree).
  detachParentRawLegacyLike(newChild);

  auto* parentNode = parent.node;
  CHECK(parentNode != nullptr);

  auto* oldNode = oldChild.node;
  auto* newNode = newChild.node;
  CHECK(oldNode != nullptr);
  CHECK(newNode != nullptr);

  auto* prev = oldNode->prevSibling;
  auto* next = oldNode->nextSibling;

  // Splice new child into old child's position.
  newNode->parent = parentNode;
  newNode->prevSibling = prev;
  newNode->nextSibling = next;

  if (prev != nullptr) {
    prev->nextSibling = newNode;
  } else {
    parentNode->firstChild = newNode;
  }
  if (next != nullptr) {
    next->prevSibling = newNode;
  } else {
    parentNode->lastChild = newNode;
  }

  // Detach old child.
  oldNode->parent = nullptr;
  oldNode->prevSibling = nullptr;
  oldNode->nextSibling = nullptr;
}

template<typename Iter>
void tuneBranchNodeLegacyLike(ZSwc& tree, Iter tn)
{
  // Port of `Swc_Tree_Node_Tune_Branch`.
  double dist = 0.0;

  if (isBranchPointLegacyLike(tn, tree)) {
    return;
  }

  const auto parent = ZSwc::parent(tn);
  if (isBranchPointLegacyLike(parent, tree)) {
    if (tn->weight > 0.0) {
      double minDist = -1.0;
      auto newTn = parent;

      const auto firstChild = ZSwc::firstChild(tn);
      if (!formingTurnLegacyLike(firstChild, tn, parent)) {
        minDist = nodeDistLegacyLike(tn, parent);
      }

      const auto parentParent = ZSwc::parent(parent);
      if (!formingTurnLegacyLike(firstChild, tn, parentParent)) {
        dist = nodeDistLegacyLike(tn, parentParent);
        if (minDist < 0.0 || dist < minDist) {
          minDist = dist;
          newTn = parentParent;
        }
      }

      for (auto it = tree.beginChild(parent); it != tree.endChild(parent); ++it) {
        if (it == tn) {
          continue;
        }
        if (!formingTurnLegacyLike(firstChild, tn, it)) {
          dist = nodeDistLegacyLike(tn, it);
          if (minDist < 0.0 || dist < minDist) {
            minDist = dist;
            newTn = it;
          }
        }
      }

      if (newTn != parent) {
        setParentLegacyLike(tree, tn, newTn);
      }
    }
  }

  auto child = ZSwc::firstChild(tn);
  while (!ZSwc::isNull(child)) {
    if (isBranchPointLegacyLike(child, tree)) {
      if (child->weight > 0.0) {
        double minDist = -1.0;
        auto newTn = child;

        if (!formingTurnLegacyLike(parent, tn, child)) {
          minDist = nodeDistLegacyLike(tn, child);
        }

        auto grandchild = ZSwc::firstChild(child);
        while (!ZSwc::isNull(grandchild)) {
          if (!formingTurnLegacyLike(parent, tn, grandchild)) {
            dist = nodeDistLegacyLike(tn, grandchild);
            if (minDist < 0.0 || dist < minDist) {
              minDist = dist;
              newTn = grandchild;
            }
          }
          grandchild = ZSwc::SwcTreeNode(grandchild.node->nextSibling);
        }

        if (newTn != child) {
          replaceChildLegacyLike(tree, child, newTn);
          setParentLegacyLike(tree, child, newTn);
          child = newTn;
        }
      }
    }

    child = ZSwc::SwcTreeNode(child.node->nextSibling);
  }
}

[[nodiscard]] double mainTrunkLengthLegacyLike(const ZSwc& subtree)
{
  // Port of:
  // - ZSwcDistTrunkAnalyzer::extractMainTrunk with distance weights (0, 1)
  // - ZSwcPath::getLength on the returned endpoints/path.
  //
  // This selects endpoints by maximizing Euclidean distance, then measures geodesic
  // length along the parent-chain path between the endpoints.
  if (subtree.empty()) {
    return 0.0;
  }

  // Identify the first regular root within this subtree.
  ZSwc::ConstSwcTreeNode regularRoot;
  for (auto it = subtree.cbegin(); it != subtree.cend(); ++it) {
    if (isRegularLegacyLike(it) && isRootLegacyLike(it)) {
      regularRoot = it;
      break;
    }
  }
  if (ZSwc::isNull(regularRoot)) {
    return 0.0;
  }

  std::vector<ZSwc::ConstSwcTreeNode> leafArray;
  leafArray.push_back(regularRoot);

  for (auto it = subtree.cbeginLeaf(regularRoot); it != subtree.cendLeaf(regularRoot); ++it) {
    if (isRegularLegacyLike(it) && isLeafLegacyLike(it)) {
      leafArray.push_back(it);
    }
  }

  ZSwc::ConstSwcTreeNode leaf1;
  ZSwc::ConstSwcTreeNode leaf2;
  double maxLength = -1.0;

  auto euclideanDistance = [](const ZSwc::ConstSwcTreeNode& a, const ZSwc::ConstSwcTreeNode& b) -> double {
    const double dx = a->x - b->x;
    const double dy = a->y - b->y;
    const double dz = a->z - b->z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
  };

  for (size_t i = 0; i < leafArray.size(); ++i) {
    for (size_t j = 0; j < leafArray.size(); ++j) {
      if (leafArray[i] != leafArray[j]) {
        const double length = euclideanDistance(leafArray[i], leafArray[j]);
        if (length > maxLength) {
          maxLength = length;
          leaf1 = leafArray[i];
          leaf2 = leafArray[j];
        }
      }
    }
  }

  if (ZSwc::isNull(leaf1) || ZSwc::isNull(leaf2)) {
    return 0.0;
  }

  std::unordered_set<const void*> ancestors;
  ancestors.reserve(128);
  for (auto it = leaf1; !ZSwc::isNull(it); it = ZSwc::parent(it)) {
    ancestors.insert(static_cast<const void*>(it.node));
  }

  ZSwc::ConstSwcTreeNode ancestor;
  for (auto it = leaf2; !ZSwc::isNull(it); it = ZSwc::parent(it)) {
    if (ancestors.find(static_cast<const void*>(it.node)) != ancestors.end()) {
      ancestor = it;
      break;
    }
  }
  if (ZSwc::isNull(ancestor)) {
    return 0.0;
  }
  if (isVirtualLegacyLike(ancestor)) {
    return 0.0;
  }

  auto accumulateUp = [&](ZSwc::ConstSwcTreeNode tn) -> double {
    double length = 0.0;
    while (tn != ancestor) {
      const auto p = ZSwc::parent(tn);
      if (ZSwc::isNull(p)) {
        break;
      }
      length +=
        std::sqrt((tn->x - p->x) * (tn->x - p->x) + (tn->y - p->y) * (tn->y - p->y) + (tn->z - p->z) * (tn->z - p->z));
      tn = p;
    }
    return length;
  };

  return accumulateUp(leaf1) + accumulateUp(leaf2);
}

} // namespace

void swcTreeRemoveZigzagLegacyLike(ZSwc& tree)
{
  bool zigzagFound = true;
  while (zigzagFound) {
    zigzagFound = false;

    for (auto it = tree.begin(); it != tree.end();) {
      const auto tn = it;
      auto next = it;
      ++next;

      const auto child = ZSwc::firstChild(tn);
      if (!ZSwc::isNull(child) && isTurnLegacyLike(tn, tree) && isTurnLegacyLike(child, tree)) {
        auto afterChild = child;
        ++afterChild;
        mergeToParent(tree, child);
        zigzagFound = true;
        it = afterChild;
        continue;
      }

      it = next;
    }
  }
}

void swcTreeRemoveOvershootLegacyLike(ZSwc& tree)
{
  for (auto it = tree.begin(); it != tree.end();) {
    const auto tn = it;
    ++it;
    if (isOvershootLegacyLike(tn, tree)) {
      mergeToParent(tree, tn);
    }
  }
}

void swcTreeRemoveSpurLegacyLike(ZSwc& tree)
{
  std::vector<ZSwc::SwcTreeNode> order;
  order.reserve(tree.size());
  for (auto it = tree.beginBreadthFirst(); it != tree.endBreadthFirst(); ++it) {
    order.push_back(it);
  }

  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const auto tn = *it;
    if (isSpurLegacyLike(tn, tree)) {
      mergeToParent(tree, tn);
    }
  }
}

void swcTreeMergeCloseNodeLegacyLike(ZSwc& tree, double threshold)
{
  std::vector<ZSwc::SwcTreeNode> order;
  order.reserve(tree.size());
  for (auto it = tree.beginBreadthFirst(); it != tree.endBreadthFirst(); ++it) {
    order.push_back(it);
  }

  for (const auto& tn : order) {
    if (ZSwc::isNull(tn)) {
      continue;
    }
    if (isRootLegacyLike(tn)) {
      continue;
    }

    if (nodeLengthLegacyLike(tn) < threshold) {
      mergeToParent(tree, tn);
      continue;
    }

    if (isLastChildLegacyLike(tn)) {
      const auto parent = ZSwc::parent(tn);
      if (!ZSwc::isNull(parent) && ZSwc::firstChild(parent) != tn) {
        auto nextChild = ZSwc::firstChild(parent);
        while (!ZSwc::isNull(nextChild)) {
          const auto sibling = ZSwc::SwcTreeNode(nextChild.node->nextSibling);
          if (ZSwc::isNull(sibling)) {
            break;
          }

          if (nodeDistLegacyLike(nextChild, sibling) < threshold) {
            tree.appendChild(nextChild, sibling);
            mergeToParent(tree, sibling);
          } else {
            nextChild = sibling;
          }
        }
      }
    }
  }
}

[[nodiscard]] ZSwc copySubtreeLegacyLike(const ZSwc& tree, const ZSwc::SwcTreeNode& root)
{
  ZSwc out;
  if (ZSwc::isNull(root)) {
    return out;
  }

  const auto newRoot = out.appendRoot(*root);
  out.copy(newRoot, tree, root);
  return out;
}

void swcTreeTuneBranchLegacyLike(ZSwc& tree)
{
  std::vector<ZSwc::SwcTreeNode> order;
  order.reserve(tree.size());
  for (auto it = tree.beginBreadthFirst(); it != tree.endBreadthFirst(); ++it) {
    order.push_back(it);
  }

  for (const auto& tn : order) {
    if (ZSwc::isNull(tn)) {
      continue;
    }
    tuneBranchNodeLegacyLike(tree, tn);
  }
}

void swcTreeRemoveOrphanBlobLegacyLike(ZSwc& tree, double minLength, int minOrphanCount)
{
  std::vector<ZSwc::SwcTreeNode> roots;
  for (auto it = tree.beginRoot(); it != tree.endRoot(); ++it) {
    roots.push_back(it);
  }

  if (roots.empty()) {
    return;
  }

  if (minLength == 0.0 && static_cast<int>(roots.size()) >= minOrphanCount) {
    double sum = 0.0;
    for (const auto& root : roots) {
      const ZSwc subtree = copySubtreeLegacyLike(tree, root);
      sum += mainTrunkLengthLegacyLike(subtree);
    }
    minLength = sum / static_cast<double>(roots.size());
  }

  std::vector<ZSwc::SwcTreeNode> keep;
  keep.reserve(roots.size());
  for (const auto& root : roots) {
    const ZSwc subtree = copySubtreeLegacyLike(tree, root);
    if (mainTrunkLengthLegacyLike(subtree) >= minLength) {
      keep.push_back(root);
    }
  }

  for (const auto& root : roots) {
    if (std::find(keep.begin(), keep.end(), root) == keep.end()) {
      tree.eraseSubtree(root);
    }
  }
}

} // namespace nim::neutube
