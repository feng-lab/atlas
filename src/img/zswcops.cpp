#include "zswcops.h"

#include "zswcgeom.h"
#include "zswcpointdist.h"

#include "zlog.h"

namespace nim {

namespace {

void cutNodeLikeLegacySubtract(ZSwc& swc, ZSwc::SwcTreeNode node)
{
  CHECK(!ZSwc::isNull(node));

  std::vector<ZSwc::SwcTreeNode> children;
  for (auto it = swc.beginChild(node); it != swc.endChild(node); ++it) {
    children.push_back(it);
  }

  for (auto& child : children) {
    swc.appendRoot(child);
  }

  swc.erase(node);
}

} // namespace

void mergeToParent(ZSwc& swc, ZSwc::SwcTreeNode node)
{
  mergeToParent(swc, node, SwcMergeOption::MergeWithParent);
}

void mergeToParent(ZSwc& swc, ZSwc::SwcTreeNode node, SwcMergeOption option)
{
  CHECK(!ZSwc::isNull(node));
  CHECK(!ZSwc::isRoot(node)) << "mergeToParent expects a non-root node";

  auto parent = ZSwc::parent(node);
  CHECK(!ZSwc::isNull(parent));

  switch (option) {
    case SwcMergeOption::MergeWithChild:
      *parent = *node;
      break;
    case SwcMergeOption::MergeAverage:
      parent->radius = (node->radius + parent->radius) * 0.5;
      parent->x = (node->x + parent->x) * 0.5;
      parent->y = (node->y + parent->y) * 0.5;
      parent->z = (node->z + parent->z) * 0.5;
      break;
    case SwcMergeOption::MergeWeightedAverage: {
      const double w1 = node->weight;
      const double w2 = parent->weight;
      if (w1 == 0.0 && w2 == 0.0) {
        parent->radius = (node->radius + parent->radius) * 0.5;
        parent->x = (node->x + parent->x) * 0.5;
        parent->y = (node->y + parent->y) * 0.5;
        parent->z = (node->z + parent->z) * 0.5;
      } else {
        const double w = w1 + w2;
        parent->radius = (node->radius * w1 + parent->radius * w2) / w;
        parent->x = (node->x * w1 + parent->x * w2) / w;
        parent->y = (node->y * w1 + parent->y * w2) / w;
        parent->z = (node->z * w1 + parent->z * w2) / w;
      }
      parent->weight += 1.0;
      break;
    }
    case SwcMergeOption::MergeWithParent:
    default:
      break;
  }

  swc.erase(node);
}

ZSwc::SwcTreeNode addBreak(ZSwc& swc, ZSwc::SwcTreeNode node, double lambda)
{
  CHECK(!ZSwc::isNull(node));

  if (ZSwc::isRoot(node)) {
    return node;
  }

  ZSwc::SwcTreeNode parent = ZSwc::parent(node);
  CHECK(!ZSwc::isNull(parent));

  if (lambda <= 0.0) {
    return parent;
  }
  if (lambda >= 1.0) {
    return node;
  }

  SwcNode newNode;
  // Legacy `Swc_Tree_Node_Add_Break` creates a new node via `New_Swc_Tree_Node()`,
  // which defaults `Swc_Node.type` to 2 (see tz_swc_cell.c::Default_Swc_Node).
  // Preserve this exact behavior for byte-identical SWC outputs.
  newNode.id = 0;
  newNode.type = 2;
  newNode.x = lambda * node->x + (1.0 - lambda) * parent->x;
  newNode.y = lambda * node->y + (1.0 - lambda) * parent->y;
  newNode.z = lambda * node->z + (1.0 - lambda) * parent->z;
  newNode.radius = lambda * node->radius + (1.0 - lambda) * parent->radius;
  newNode.parentID = -1;

  ZSwc::SwcTreeNode inserted = swc.insertChildBefore(parent, node, newNode);

  // Make `node` the only child of the inserted break node.
  swc.appendChild(inserted, node);

  return inserted;
}

void connectNode(ZSwc& tree, ZSwc::SwcTreeNode node)
{
  CHECK(!ZSwc::isNull(node));

  constexpr double Eps = 0.05;

  const auto excludeRoot = ZSwc::root(node);
  const SwcPointDistResult distRes = swcTreePointDist(tree, node->x, node->y, node->z, excludeRoot);
  if (ZSwc::isNull(distRes.closestNode)) {
    return;
  }

  const auto tmpNode = distRes.closestNode;
  CHECK(!ZSwc::isRoot(tmpNode));
  const auto tmpParent = ZSwc::parent(tmpNode);
  CHECK(!ZSwc::isNull(tmpParent));

  std::array<double, 3> tmpPos = distRes.closestPoint;
  if (distRes.dist <= Eps) {
    tmpPos = {node->x, node->y, node->z};
  }

  const std::array<double, 3> start = {tmpParent->x, tmpParent->y, tmpParent->z};
  const std::array<double, 3> end = {tmpNode->x, tmpNode->y, tmpNode->z};

  double lambda = 0.0;
  (void)geo3dPointLineSegDist(tmpPos, start, end, lambda);

  if (lambda - Eps < 0.0) {
    tree.appendChild(tmpParent, node);
  } else if (lambda + Eps > 1.0) {
    tree.appendChild(tmpNode, node);
  } else {
    const auto breakNode = addBreak(tree, tmpNode, lambda);
    tree.appendChild(breakNode, node);
  }
}

ZSwc::SwcTreeNode connectBranch(ZSwc& tree, ZSwc::SwcTreeNode startNode)
{
  CHECK(!ZSwc::isNull(startNode));

  ZSwc::SwcTreeNode endNode = startNode;
  while (true) {
    const auto child = ZSwc::firstChild(endNode);
    if (ZSwc::isNull(child)) {
      break;
    }
    endNode = child;
  }

  if (startNode == endNode) {
    connectNode(tree, startNode);
    return startNode;
  }

  constexpr double Eps = 0.05;

  const auto excludeRoot = ZSwc::root(startNode);
  SwcPointDistResult d1 = swcTreePointDist(tree, startNode->x, startNode->y, startNode->z, excludeRoot);
  SwcPointDistResult d2 = swcTreePointDist(tree, endNode->x, endNode->y, endNode->z, excludeRoot);

  ZSwc::SwcTreeNode tmpNode = d1.closestNode;
  ZSwc::SwcTreeNode tn = startNode;
  double dist = d1.dist;
  std::array<double, 3> pt = d1.closestPoint;

  if (d2.dist < d1.dist) {
    tmpNode = d2.closestNode;
    tn = endNode;
    tree.setAsRoot(tn);
    dist = d2.dist;
    pt = d2.closestPoint;
  }

  if (ZSwc::isNull(tmpNode)) {
    return tn;
  }

  if (ZSwc::isRoot(tmpNode)) {
    tree.appendChild(tmpNode, tn);
    return tn;
  }

  const auto tmpParent = ZSwc::parent(tmpNode);
  CHECK(!ZSwc::isNull(tmpParent));

  std::array<double, 3> tmpPos = pt;
  if (dist <= Eps) {
    tmpPos = {tn->x, tn->y, tn->z};
  }

  const std::array<double, 3> start = {tmpParent->x, tmpParent->y, tmpParent->z};
  const std::array<double, 3> end = {tmpNode->x, tmpNode->y, tmpNode->z};

  double lambda = 0.0;
  (void)geo3dPointLineSegDist(tmpPos, start, end, lambda);

  if (lambda - Eps < 0.0) {
    tree.appendChild(tmpParent, tn);
  } else if (lambda + Eps > 1.0) {
    tree.appendChild(tmpNode, tn);
  } else {
    const auto breakNode = addBreak(tree, tmpNode, lambda);
    tree.appendChild(breakNode, tn);
  }

  return tn;
}

int resortId(ZSwc& tree)
{
  int id = 1;
  for (auto it = tree.begin(); it != tree.end(); ++it) {
    if (it->id >= 0) {
      it->id = id;
      ++id;
    }

    const auto parent = ZSwc::parent(it);
    if (ZSwc::isNull(parent)) {
      it->parentID = -1;
    } else {
      it->parentID = parent->id;
    }
  }

  return id - 1;
}

void subtractSwcTrees(ZSwc& tree1, const ZSwc& tree2)
{
  for (auto it = tree1.begin(); it != tree1.end();) {
    auto next = it;
    ++next;

    if (it->id >= 0 && swcTreeHitTest(tree2, it->x, it->y, it->z)) {
      cutNodeLikeLegacySubtract(tree1, it);
    }

    it = next;
  }
}

} // namespace nim
