#include "zswceditlegacy.h"

#include "zlinearassignment.h"
#include "zswctreenodegeomlegacy.h"
#include "zlog.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <queue>
#include <set>
#include <tuple>
#include <vector>

namespace nim {

void deleteNodesLegacyLike(ZSwc& swc, const std::vector<ZSwc::SwcTreeNode>& toDelete)
{
  for (const auto& node : toDelete) {
    if (ZSwc::isNull(node)) {
      continue;
    }

    std::vector<ZSwc::SwcTreeNode> children;
    for (auto it = swc.beginChild(node); it != swc.endChild(node); ++it) {
      children.push_back(it);
    }

    for (auto& child : children) {
      swc.appendRoot(child);
    }

    swc.erase(node);
  }
}

namespace {

struct Dsu
{
  explicit Dsu(size_t n)
    : parent(n)
    , rank(n, 0)
  {
    for (size_t i = 0; i < n; ++i) {
      parent[i] = i;
    }
  }

  size_t find(size_t x)
  {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  }

  bool unite(size_t a, size_t b)
  {
    a = find(a);
    b = find(b);
    if (a == b) {
      return false;
    }
    if (rank[a] < rank[b]) {
      std::swap(a, b);
    }
    parent[b] = a;
    if (rank[a] == rank[b]) {
      ++rank[a];
    }
    return true;
  }

  std::vector<size_t> parent;
  std::vector<uint8_t> rank;
};

struct WeightedEdge
{
  size_t u = 0;
  size_t v = 0;
  double w = 0.0;
  size_t order = 0;
};

[[nodiscard]] double computeConnectorDistanceLegacyLike(const ZSwc::ConstSwcTreeNode& a,
                                                        const ZSwc::ConstSwcTreeNode& b,
                                                        const SwcConnectSelectedNodesOptionsLegacyLike& opt)
{
  double d = 0.0;
  if (opt.useSurfaceDistance) {
    d = swcNodeSurfaceDistanceLegacyLike(a, b, opt.voxelSizeX, opt.voxelSizeY, opt.voxelSizeZ);
  } else {
    d = swcNodeDistanceLegacyLike(a, b, opt.voxelSizeX, opt.voxelSizeY, opt.voxelSizeZ);
  }

  if (d < 0.0) {
    const double r1 = a->radius;
    const double r2 = b->radius;
    if (r1 == 0.0 || r2 == 0.0) {
      d = 0.0;
    } else {
      d = swcNodeDistanceLegacyLike(a, b, opt.voxelSizeX, opt.voxelSizeY, opt.voxelSizeZ) / r1 / r2;
    }
  }

  return d;
}

[[nodiscard]] double boundBoxDiagonalLengthWithRadiusLegacyLike(const std::vector<ZSwc::SwcTreeNode>& nodes)
{
  CHECK(!nodes.empty());

  double minX = std::numeric_limits<double>::infinity();
  double minY = std::numeric_limits<double>::infinity();
  double minZ = std::numeric_limits<double>::infinity();
  double maxX = -std::numeric_limits<double>::infinity();
  double maxY = -std::numeric_limits<double>::infinity();
  double maxZ = -std::numeric_limits<double>::infinity();

  for (const auto& n : nodes) {
    if (ZSwc::isNull(n)) {
      continue;
    }
    minX = std::min(minX, n->x - n->radius);
    minY = std::min(minY, n->y - n->radius);
    minZ = std::min(minZ, n->z - n->radius);
    maxX = std::max(maxX, n->x + n->radius);
    maxY = std::max(maxY, n->y + n->radius);
    maxZ = std::max(maxZ, n->z + n->radius);
  }

  const double dx = maxX - minX;
  const double dy = maxY - minY;
  const double dz = maxZ - minZ;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

[[nodiscard]] std::vector<WeightedEdge>
buildConnectionEdgesLegacyLike(const std::vector<ZSwc::SwcTreeNode>& nodes,
                               const SwcConnectSelectedNodesOptionsLegacyLike& opt)
{
  std::vector<WeightedEdge> edges;
  if (nodes.size() < 2) {
    return edges;
  }

  const double diag = boundBoxDiagonalLengthWithRadiusLegacyLike(nodes);
  const double minDist = (nodes.empty() || opt.nodeNumberThreshold <= 0)
                           ? 0.0
                           : (diag * static_cast<double>(opt.nodeNumberThreshold) / nodes.size());

  std::vector<ZSwc::SwcTreeNode> roots;
  roots.reserve(nodes.size());
  for (const auto& n : nodes) {
    roots.push_back(ZSwc::root(n));
  }

  std::vector<bool> isOrphan(nodes.size(), true);
  size_t order = 0;
  for (size_t i = 0; i < nodes.size(); ++i) {
    for (size_t j = i + 1; j < nodes.size(); ++j) {
      const auto& n1 = nodes[i];
      const auto& n2 = nodes[j];
      if (ZSwc::isNull(n1) || ZSwc::isNull(n2)) {
        continue;
      }

      if (roots[i] != roots[j]) {
        const double w = computeConnectorDistanceLegacyLike(n1, n2, opt);
        if (w <= minDist) {
          edges.push_back(WeightedEdge{.u = i, .v = j, .w = w + 0.1, .order = order++});
        }
      } else {
        if (isOrphan[j]) {
          edges.push_back(WeightedEdge{.u = i, .v = j, .w = 0.0, .order = order++});
          isOrphan[j] = false;
        }
      }
    }
  }

  return edges;
}

[[nodiscard]] std::vector<WeightedEdge> kruskalMinimumSpanningForest(std::vector<WeightedEdge> edges, size_t nVertices)
{
  std::stable_sort(edges.begin(), edges.end(), [](const WeightedEdge& a, const WeightedEdge& b) {
    if (a.w != b.w) {
      return a.w < b.w;
    }
    return a.order < b.order;
  });

  Dsu dsu(nVertices);
  std::vector<WeightedEdge> mst;
  mst.reserve(nVertices > 0 ? (nVertices - 1) : 0);
  for (const auto& e : edges) {
    if (e.u == e.v) {
      continue;
    }
    if (e.u >= nVertices || e.v >= nVertices) {
      continue;
    }
    if (dsu.unite(e.u, e.v)) {
      mst.push_back(e);
    }
  }
  return mst;
}

[[nodiscard]] std::vector<std::pair<size_t, size_t>>
breadthFirstDirectedEdgesLegacyLike(const std::vector<WeightedEdge>& undirectedEdges, size_t nVertices)
{
  std::vector<std::vector<size_t>> adj(nVertices);
  for (const auto& e : undirectedEdges) {
    if (e.u >= nVertices || e.v >= nVertices) {
      continue;
    }
    adj[e.u].push_back(e.v);
    adj[e.v].push_back(e.u);
  }
  for (auto& nbrs : adj) {
    std::sort(nbrs.begin(), nbrs.end());
  }

  std::vector<bool> visited(nVertices, false);
  std::vector<std::pair<size_t, size_t>> directed;

  for (const auto& e : undirectedEdges) {
    const size_t root = e.u;
    if (root >= nVertices || visited[root]) {
      continue;
    }

    std::queue<size_t> q;
    visited[root] = true;
    q.push(root);

    while (!q.empty()) {
      const size_t v = q.front();
      q.pop();

      for (const size_t n : adj[v]) {
        if (visited[n]) {
          continue;
        }
        visited[n] = true;
        q.push(n);
        directed.emplace_back(v, n);
      }
    }
  }

  return directed;
}

} // namespace

bool connectSelectedNodesLegacyLike(ZSwc& swc,
                                    const std::vector<ZSwc::SwcTreeNode>& selectedNodes,
                                    const SwcConnectSelectedNodesOptionsLegacyLike& opt)
{
  if (selectedNodes.size() < 2) {
    return false;
  }

  // Match neuTube: only connect nodes from different regular roots.
  bool haveDifferentRoots = false;
  ZSwc::SwcTreeNode firstRoot = ZSwc::root(selectedNodes.front());
  for (const auto& n : selectedNodes) {
    if (ZSwc::root(n) != firstRoot) {
      haveDifferentRoots = true;
      break;
    }
  }
  if (!haveDifferentRoots) {
    return false;
  }

  std::vector<WeightedEdge> allEdges = buildConnectionEdgesLegacyLike(selectedNodes, opt);
  if (allEdges.empty()) {
    return false;
  }

  std::vector<WeightedEdge> mst = kruskalMinimumSpanningForest(std::move(allEdges), selectedNodes.size());

  // Remove zero-weight edges (those were only used to ensure connectivity within each original tree).
  std::vector<WeightedEdge> connEdges;
  connEdges.reserve(mst.size());
  for (const auto& e : mst) {
    if (e.w != 0.0) {
      connEdges.push_back(e);
    }
  }
  if (connEdges.empty()) {
    return false;
  }

  const std::vector<std::pair<size_t, size_t>> directedEdges =
    breadthFirstDirectedEdgesLegacyLike(connEdges, selectedNodes.size());
  if (directedEdges.empty()) {
    return false;
  }

  for (const auto& [upIdx, downIdx] : directedEdges) {
    const auto upNode = selectedNodes.at(upIdx);
    const auto downNode = selectedNodes.at(downIdx);
    CHECK(!ZSwc::isNull(upNode));
    CHECK(!ZSwc::isNull(downNode));
    swc.setAsRoot(downNode);
    swc.appendChild(upNode, downNode);
  }

  return true;
}

std::optional<ZSwc::SwcTreeNode> mergeSelectedNodesLegacyLike(ZSwc& swc,
                                                              const std::vector<ZSwc::SwcTreeNode>& selectedNodes)
{
  if (selectedNodes.size() <= 1) {
    return std::nullopt;
  }

  std::set<ZSwc::SwcTreeNode> nodeSet(selectedNodes.begin(), selectedNodes.end());

  glm::dvec3 center(0.0);
  double maxRadius = 0.0;
  for (const auto& n : nodeSet) {
    if (ZSwc::isNull(n)) {
      continue;
    }
    center += glm::dvec3(n->x, n->y, n->z);
    maxRadius = std::max(maxRadius, n->radius);
  }
  center /= static_cast<double>(nodeSet.size());

  // Match neuTube `SwcTreeNode::MakePointer(center, radius)` defaults: id=1, type=0, parent_id=-1.
  SwcNode coreNode;
  coreNode.id = 1;
  coreNode.type = 0;
  coreNode.x = center.x;
  coreNode.y = center.y;
  coreNode.z = center.z;
  coreNode.radius = maxRadius;
  coreNode.parentID = -1;
  coreNode.selected = false;

  std::set<ZSwc::SwcTreeNode> parentSet;
  std::vector<ZSwc::SwcTreeNode> externalChildren;
  externalChildren.reserve(nodeSet.size());

  for (const auto& n : nodeSet) {
    if (ZSwc::isNull(n)) {
      continue;
    }

    const auto parent = ZSwc::parent(n);
    if (!ZSwc::isNull(parent) && !nodeSet.contains(parent)) {
      parentSet.insert(parent);
    }

    for (auto it = swc.beginChild(n); it != swc.endChild(n); ++it) {
      if (!nodeSet.contains(it)) {
        externalChildren.push_back(it);
      }
    }
  }

  ZSwc::SwcTreeNode coreIt = swc.appendRoot(coreNode);

  // Attach external children under the new core node.
  for (auto& child : externalChildren) {
    swc.appendChild(coreIt, child);
  }

  // Attach the core node following neuTube semantics based on the number of external parents.
  if (parentSet.size() == 1) {
    const auto onlyParent = *parentSet.begin();
    CHECK(!ZSwc::isNull(onlyParent));
    swc.appendChild(onlyParent, coreIt);
  } else if (parentSet.size() > 1) {
    for (const auto& p : parentSet) {
      CHECK(!ZSwc::isNull(p));
      swc.appendChild(coreIt, p);
    }
  } else {
    // No external parent: keep core as a root (equivalent to attaching to a virtual root in neuTube).
  }

  // Remove the original selected nodes.
  std::vector<ZSwc::SwcTreeNode> toErase;
  toErase.reserve(nodeSet.size());
  for (auto it = swc.beginPostOrder(); it != swc.endPostOrder(); ++it) {
    if (nodeSet.contains(it)) {
      toErase.push_back(it);
    }
  }
  for (auto& n : toErase) {
    swc.erase(n);
  }

  return coreIt;
}

namespace {

[[nodiscard]] bool isContinuationNodeLegacyLike(const ZSwc& swc, const ZSwc::SwcTreeNode& n)
{
  if (ZSwc::isNull(n) || ZSwc::isRoot(n)) {
    return false;
  }
  if (ZSwc::isBranchNode(n)) {
    return false;
  }
  return swc.numChildren(n) == 1;
}

[[nodiscard]] double swcPathLengthLegacyLike(const ZSwc::ConstSwcTreeNode& a, const ZSwc::ConstSwcTreeNode& b)
{
  // Legacy `SwcTreeNode::pathLength(tn1, tn2)`:
  // for adjacent nodes (the only case we use here), it reduces to Euclidean distance.
  return swcNodeDistanceLegacyLike(a, b);
}

[[nodiscard]] double swcPathLengthRatioLegacyLike(const ZSwc::ConstSwcTreeNode& tn1,
                                                  const ZSwc::ConstSwcTreeNode& tn2,
                                                  const ZSwc::ConstSwcTreeNode& tn)
{
  const double d1 = swcPathLengthLegacyLike(tn1, tn);
  const double d2 = swcPathLengthLegacyLike(tn2, tn);

  if (d1 == 0.0 && d2 == 0.0) {
    return 0.5;
  }
  if (d1 == 0.0) {
    return 0.0;
  }

  if (std::isinf(d1) && std::isinf(d2)) {
    return 0.5;
  }
  if (std::isinf(d1)) {
    return 1.0;
  }
  if (std::isinf(d2)) {
    return 0.0;
  }

  return d1 / (d1 + d2);
}

[[nodiscard]] double swcDotLegacyLike(const ZSwc::ConstSwcTreeNode& tn1,
                                      const ZSwc::ConstSwcTreeNode& tn2,
                                      const ZSwc::ConstSwcTreeNode& tn3)
{
  // Port of `Swc_Tree_Node_Dot(tn1, tn2, tn3)`:
  // - Requires adjacency along parent links (tn2 is between tn1 and tn3).
  // - Computes the normalized dot product of (tn1 - tn2) and (tn2 - tn3).
  if (ZSwc::isNull(tn1) || ZSwc::isNull(tn2) || ZSwc::isNull(tn3)) {
    return 0.0;
  }

  const auto p2 = ZSwc::parent(tn2);
  if (ZSwc::isNull(p2)) {
    return 0.0;
  }

  const bool adjacent = ((p2 == tn1) && (ZSwc::parent(tn3) == tn2)) || ((p2 == tn3) && (ZSwc::parent(tn1) == tn2));
  if (!adjacent) {
    return 0.0;
  }

  double v1x = tn1->x - tn2->x;
  double v1y = tn1->y - tn2->y;
  double v1z = tn1->z - tn2->z;
  double v2x = tn2->x - tn3->x;
  double v2y = tn2->y - tn3->y;
  double v2z = tn2->z - tn3->z;

  const double n1 = std::sqrt(v1x * v1x + v1y * v1y + v1z * v1z);
  if (n1 > 0.0) {
    v1x /= n1;
    v1y /= n1;
    v1z /= n1;
  }

  const double n2 = std::sqrt(v2x * v2x + v2y * v2y + v2z * v2z);
  if (n2 > 0.0) {
    v2x /= n2;
    v2y /= n2;
    v2z /= n2;
  }

  return v1x * v2x + v1y * v2y + v1z * v2z;
}

} // namespace

std::optional<SwcNodeGeometryLegacyLike> computeRemoveTurnGeometryLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& node)
{
  if (ZSwc::isNull(node)) {
    return std::nullopt;
  }

  ZSwc::SwcTreeNode tn1;
  ZSwc::SwcTreeNode tn2;

  if (isContinuationNodeLegacyLike(swc, node)) {
    tn1 = ZSwc::firstChild(node);
    tn2 = ZSwc::parent(node);
  } else {
    const auto parent = ZSwc::parent(node);
    if (!ZSwc::isNull(parent)) {
      double minDot = 0.0;
      for (auto child = swc.beginChild(node); child != swc.endChild(node); ++child) {
        const double dot = swcDotLegacyLike(child, node, parent);
        if (dot < minDot) {
          minDot = dot;
          tn1 = child;
          tn2 = parent;
        }
      }
    }
  }

  if (ZSwc::isNull(tn1) || ZSwc::isNull(tn2)) {
    return std::nullopt;
  }

  if (!swcNodesFormingTurnLegacyLike(tn1, node, tn2)) {
    return std::nullopt;
  }

  const double lambda = swcPathLengthRatioLegacyLike(tn2, tn1, node);

  SwcNodeGeometryLegacyLike out;
  out.x = tn1->x * lambda + tn2->x * (1.0 - lambda);
  out.y = tn1->y * lambda + tn2->y * (1.0 - lambda);
  out.z = tn1->z * lambda + tn2->z * (1.0 - lambda);
  out.radius = std::max(0.0, tn1->radius * lambda + tn2->radius * (1.0 - lambda));
  return out;
}

namespace {

[[nodiscard]] double swcNormalizedDotLegacyLike(const ZSwc::ConstSwcTreeNode& tn1,
                                                const ZSwc::ConstSwcTreeNode& tn2,
                                                const ZSwc::ConstSwcTreeNode& tn3)
{
  if (ZSwc::isNull(tn1) || ZSwc::isNull(tn2) || ZSwc::isNull(tn3)) {
    return 0.0;
  }

  double v1x = tn1->x - tn2->x;
  double v1y = tn1->y - tn2->y;
  double v1z = tn1->z - tn2->z;
  double v2x = tn2->x - tn3->x;
  double v2y = tn2->y - tn3->y;
  double v2z = tn2->z - tn3->z;

  const double n1 = std::sqrt(v1x * v1x + v1y * v1y + v1z * v1z);
  if (n1 > 0.0) {
    v1x /= n1;
    v1y /= n1;
    v1z /= n1;
  }

  const double n2 = std::sqrt(v2x * v2x + v2y * v2y + v2z * v2z);
  if (n2 > 0.0) {
    v2x /= n2;
    v2y /= n2;
    v2z /= n2;
  }

  return v1x * v2x + v1y * v2y + v1z * v2z;
}

} // namespace

bool resetBranchPointLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& loopNode)
{
  if (ZSwc::isNull(loopNode)) {
    return false;
  }

  const std::vector<ZSwc::SwcTreeNode> loopNeighbors = swcNeighborArrayLegacyLike(swc, loopNode);
  for (const auto& tn : loopNeighbors) {
    if (ZSwc::isNull(tn)) {
      continue;
    }
    if (!ZSwc::isBranchNode(tn)) {
      continue;
    }

    const std::vector<ZSwc::SwcTreeNode> candidateHooks = swcNeighborArrayLegacyLike(swc, tn);
    ZSwc::SwcTreeNode hook;
    double minDot = std::numeric_limits<double>::infinity();

    for (const auto& hookCandidate : candidateHooks) {
      if (ZSwc::isNull(hookCandidate) || hookCandidate == loopNode) {
        continue;
      }
      if (hookCandidate->id < 0) { // Match legacy SwcTreeNode::isRegular().
        continue;
      }

      const double dot = swcNormalizedDotLegacyLike(hookCandidate, tn, loopNode);
      if (dot < minDot) {
        minDot = dot;
        hook = hookCandidate;
      }
    }

    if (ZSwc::isNull(hook)) {
      break;
    }

    if (ZSwc::parent(tn) == hook) {
      swc.appendRoot(tn);
      swc.setAsRoot(loopNode);
      swc.appendChild(hook, loopNode);
    } else {
      swc.appendChild(loopNode, hook);
    }

    return true;
  }

  return false;
}

bool joinIsolatedBranchLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& branchPoint)
{
  if (ZSwc::isNull(branchPoint)) {
    return false;
  }

  // Legacy requires multiple trees under the virtual master root.
  if (swc.numRoots() <= 1) {
    return false;
  }

  const ZSwc::SwcTreeNode hostRoot = ZSwc::root(branchPoint);

  ZSwc::SwcTreeNode closest;
  double minDist = std::numeric_limits<double>::infinity();

  for (auto rit = swc.beginRoot(); rit != swc.endRoot(); ++rit) {
    const ZSwc::SwcTreeNode root = ZSwc::SwcTreeNode(rit);
    if (root == hostRoot) {
      continue;
    }

    for (auto it = swc.begin(root); it != swc.end(root); ++it) {
      if (it->id < 0) { // Match legacy SwcTreeNode::isRegular().
        continue;
      }

      const double dist = swcNodeSurfaceDistanceLegacyLike(it, branchPoint);
      if (dist < minDist) {
        minDist = dist;
        closest = it;
      }
    }
  }

  if (ZSwc::isNull(closest)) {
    return false;
  }

  // Legacy:
  // - If the closest node is not a root, make it the root of its subtree (reverse along the upstream chain).
  // - Re-parent the closest node under the selected branch point.
  if (!ZSwc::isRoot(closest)) {
    swc.setAsRoot(closest);
  }
  swc.appendChild(branchPoint, closest);
  return true;
}

namespace {

constexpr double kHungarianLargeWeightLegacyLike = 10000.0;

[[nodiscard]] std::vector<int> linearAssignmentLegacyLike(const std::vector<std::vector<double>>& weight)
{
  const int m = static_cast<int>(weight.size());
  if (m == 0) {
    return {};
  }
  const int n = static_cast<int>(weight[0].size());
  for (int i = 0; i < m; ++i) {
    CHECK(static_cast<int>(weight[i].size()) == n);
  }

  std::vector<double> rowMajor;
  rowMajor.reserve(static_cast<size_t>(m) * static_cast<size_t>(n));
  for (const std::vector<double>& row : weight) {
    rowMajor.insert(rowMajor.end(), row.begin(), row.end());
  }

  const ZLinearAssignmentResult assignment =
    solveLinearAssignment(rowMajor, static_cast<size_t>(m), static_cast<size_t>(n));
  CHECK(static_cast<int>(assignment.rowToCol.size()) == m);

  std::vector<int> colMate;
  colMate.reserve(assignment.rowToCol.size());
  for (const int32_t col : assignment.rowToCol) {
    colMate.push_back(static_cast<int>(col));
  }
  return colMate;
}

[[nodiscard]] int idxFromSubLegacyLike(int nvertex, int i, int j)
{
  return nvertex * i + j;
}

[[nodiscard]] double matrixValueFromIndLegacyLike(const std::vector<std::vector<double>>& wm, int idx, int nvertex)
{
  const int r = idx / nvertex;
  const int c = idx % nvertex;
  return wm[r][c];
}

void setConnValueFromIndLegacyLike(std::vector<std::vector<bool>>& conn, int idx, int nvertex, bool value)
{
  const int r = idx / nvertex;
  const int c = idx % nvertex;
  conn[r][c] = value;
}

[[nodiscard]] std::vector<int>
constructEdgeLoopLegacyLike(const std::vector<std::vector<bool>>& conn, int nvertex, int i, int j)
{
  std::vector<int> loop;
  loop.reserve(static_cast<size_t>(nvertex));
  loop.push_back(idxFromSubLegacyLike(nvertex, i, j));

  bool opening = true;
  while (opening) {
    opening = false;
    for (int k = 0; k < nvertex; ++k) {
      if (conn[j][k]) {
        opening = true;
        loop.push_back(idxFromSubLegacyLike(nvertex, j, k));
        if (k == i) {
          opening = false;
        } else {
          j = k;
        }
        break;
      }
    }
  }

  return loop;
}

[[nodiscard]] double loopMatchScoreLegacyLike(const std::vector<int>& loop,
                                              int loopLength,
                                              int start,
                                              const std::vector<std::vector<double>>& wm,
                                              int nvertex)
{
  double w = 0.0;
  for (int i = start; i < loopLength; i += 2) {
    if ((start != 0) || (i != loopLength - 1)) {
      w += matrixValueFromIndLegacyLike(wm, loop[i], nvertex);
    }
  }
  return w;
}

void correctMatchLegacyLike(std::vector<std::vector<bool>>& conn,
                            int nvertex,
                            const std::vector<int>& loop,
                            int loopLength,
                            int start)
{
  for (int i = 0; i < start; ++i) {
    setConnValueFromIndLegacyLike(conn, loop[i], nvertex, false);
  }
  for (int i = start + 1; i < loopLength; i += 2) {
    setConnValueFromIndLegacyLike(conn, loop[i], nvertex, false);
  }
  if (start == 0) {
    if (loopLength % 2 == 1) {
      setConnValueFromIndLegacyLike(conn, loop[loopLength - 1], nvertex, false);
    }
  }
}

[[nodiscard]] std::vector<std::vector<bool>>
minWeightSumMatchConnLegacyLike(const std::vector<std::vector<double>>& weight)
{
  const int nvertex = static_cast<int>(weight.size());
  CHECK(nvertex >= 0);
  if (nvertex == 0) {
    return {};
  }
  for (int i = 0; i < nvertex; ++i) {
    CHECK(static_cast<int>(weight[i].size()) == nvertex);
  }

  std::vector<std::vector<double>> wm = weight;
  for (int i = 0; i < nvertex; ++i) {
    wm[i][i] = kHungarianLargeWeightLegacyLike;
    for (int j = 0; j < nvertex; ++j) {
      if (std::isinf(wm[i][j])) {
        wm[i][j] = kHungarianLargeWeightLegacyLike;
      }
    }
  }

  const std::vector<int> colMate = linearAssignmentLegacyLike(wm);
  CHECK(static_cast<int>(colMate.size()) == nvertex);

  std::vector<std::vector<bool>> conn(static_cast<size_t>(nvertex),
                                      std::vector<bool>(static_cast<size_t>(nvertex), false));
  for (int i = 0; i < nvertex; ++i) {
    const int j = colMate[i];
    if (j >= 0 && j < nvertex && i != j) {
      conn[i][j] = true;
    }
  }

  // Keep a copy of the original weight matrix for scoring and filtering.
  std::vector<std::vector<double>> wmScore = weight;
  for (int i = 0; i < nvertex; ++i) {
    wmScore[i][i] = std::numeric_limits<double>::infinity();
  }

  // Legacy loop correction: convert the assignment into an undirected matching by correcting cycles.
  for (int i = 0; i < nvertex; ++i) {
    for (int j = 0; j < nvertex; ++j) {
      if (!conn[i][j]) {
        continue;
      }

      if (conn[j][i]) {
        conn[j][i] = false;
        continue;
      }

      const std::vector<int> loop = constructEdgeLoopLegacyLike(conn, nvertex, i, j);
      const int loopLength = static_cast<int>(loop.size());
      if (loopLength > 1) {
        int bestStart = 0;
        double minW = loopMatchScoreLegacyLike(loop, loopLength, 0, wmScore, nvertex);
        double tmpW = loopMatchScoreLegacyLike(loop, loopLength, 1, wmScore, nvertex);
        if (tmpW < minW) {
          minW = tmpW;
          bestStart = 1;
        }
        if (loopLength % 2 == 1) {
          tmpW = loopMatchScoreLegacyLike(loop, loopLength, 2, wmScore, nvertex);
          if (tmpW < minW) {
            bestStart = 2;
          }
        }
        correctMatchLegacyLike(conn, nvertex, loop, loopLength, bestStart);
      }
    }
  }

  // Remove any matches on missing edges (Infinity weight in the original matrix).
  for (int i = 0; i < nvertex; ++i) {
    for (int j = 0; j < nvertex; ++j) {
      if (std::isinf(wmScore[i][j])) {
        conn[i][j] = false;
      }
    }
  }

  return conn;
}

[[nodiscard]] std::map<int, int> minWeightSumMatchLegacyLike(const std::vector<std::vector<double>>& weight)
{
  std::map<int, int> matchMap;
  const int n = static_cast<int>(weight.size());
  if (n == 0) {
    return matchMap;
  }

  const std::vector<std::vector<bool>> conn = minWeightSumMatchConnLegacyLike(weight);
  for (int i = 0; i < n; ++i) {
    for (int j = i + 1; j < n; ++j) {
      if (conn[i][j] || conn[j][i]) {
        matchMap[i] = j;
      }
    }
  }

  return matchMap;
}

[[nodiscard]] double coordinateCos3LegacyLike(const std::array<double, 3>& coord1,
                                              const std::array<double, 3>& coord2,
                                              const std::array<double, 3>& coord3)
{
  const double v1x = coord1[0] - coord2[0];
  const double v1y = coord1[1] - coord2[1];
  const double v1z = coord1[2] - coord2[2];

  const double v2x = coord2[0] - coord3[0];
  const double v2y = coord2[1] - coord3[1];
  const double v2z = coord2[2] - coord3[2];

  const double d1 = v1x * v1x + v1y * v1y + v1z * v1z;
  if (d1 == 0.0) {
    return 0.0;
  }
  const double d2 = v2x * v2x + v2y * v2y + v2z * v2z;
  if (d2 == 0.0) {
    return 0.0;
  }

  return (v1x * v2x + v1y * v2y + v1z * v2z) / std::sqrt(d1 * d2);
}

[[nodiscard]] std::vector<std::pair<ZSwc::SwcTreeNode, ZSwc::SwcTreeNode>>
crossoverMatchPairsLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& center, double maxAngleRadians)
{
  std::vector<std::pair<ZSwc::SwcTreeNode, ZSwc::SwcTreeNode>> pairs;
  if (ZSwc::isNull(center) || center->id < 0) { // Match legacy SwcTreeNode::isRegular().
    return pairs;
  }

  const std::vector<ZSwc::SwcTreeNode> nbrArray = swcNeighborArrayLegacyLike(swc, center);
  if (nbrArray.size() < 4) {
    return pairs;
  }

  const double minDot = std::cos(maxAngleRadians);
  const std::array<double, 3> centerPos{center->x, center->y, center->z};

  const int n = static_cast<int>(nbrArray.size());
  std::vector<std::vector<double>> weight(static_cast<size_t>(n), std::vector<double>(static_cast<size_t>(n), 0.0));

  int goodNodeNumber = 0;
  for (int i = 0; i < n; ++i) {
    const std::array<double, 3> pos1{nbrArray[static_cast<size_t>(i)]->x,
                                     nbrArray[static_cast<size_t>(i)]->y,
                                     nbrArray[static_cast<size_t>(i)]->z};
    bool firstMatch = true;
    for (int j = i + 1; j < n; ++j) {
      if (firstMatch) {
        goodNodeNumber++;
        firstMatch = false;
      }
      const std::array<double, 3> pos2{nbrArray[static_cast<size_t>(j)]->x,
                                       nbrArray[static_cast<size_t>(j)]->y,
                                       nbrArray[static_cast<size_t>(j)]->z};
      const double dot = coordinateCos3LegacyLike(pos1, centerPos, pos2);
      if (dot >= minDot) {
        goodNodeNumber++;
        weight[static_cast<size_t>(i)][static_cast<size_t>(j)] = std::acos(dot);
        weight[static_cast<size_t>(j)][static_cast<size_t>(i)] = weight[static_cast<size_t>(i)][static_cast<size_t>(j)];
      } else {
        weight[static_cast<size_t>(i)][static_cast<size_t>(j)] = kHungarianLargeWeightLegacyLike;
        weight[static_cast<size_t>(j)][static_cast<size_t>(i)] = kHungarianLargeWeightLegacyLike;
      }
    }
  }

  if (goodNodeNumber < 4) {
    return pairs;
  }

  const std::map<int, int> matchIndex = minWeightSumMatchLegacyLike(weight);
  pairs.reserve(matchIndex.size());
  for (const auto& [i, j] : matchIndex) {
    if (i < 0 || j < 0 || i >= n || j >= n) {
      continue;
    }
    pairs.emplace_back(nbrArray[static_cast<size_t>(i)], nbrArray[static_cast<size_t>(j)]);
  }

  std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) {
    return a.first.node < b.first.node;
  });

  return pairs;
}

} // namespace

bool resolveCrossoverLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& center)
{
  if (ZSwc::isNull(center) || center->id < 0) { // Match legacy SwcTreeNode::isRegular().
    return false;
  }

  const std::vector<ZSwc::SwcTreeNode> nbrArray = swcNeighborArrayLegacyLike(swc, center);
  const size_t centerNeighborCount = nbrArray.size();

  const std::vector<std::pair<ZSwc::SwcTreeNode, ZSwc::SwcTreeNode>> matchedPairs =
    crossoverMatchPairsLegacyLike(swc, center, std::numbers::pi / 2.0);
  if (matchedPairs.empty()) {
    return false;
  }

  bool changed = false;
  for (const auto& [n1, n2] : matchedPairs) {
    if (ZSwc::isNull(n1) || ZSwc::isNull(n2)) {
      continue;
    }

    const bool n1ChildOfCenter = (ZSwc::parent(n1) == center);
    const bool n2ChildOfCenter = (ZSwc::parent(n2) == center);

    if (n1ChildOfCenter && n2ChildOfCenter) {
      swc.appendChild(n2, n1);
      swc.appendRoot(n2);
    } else {
      // Match legacy behavior: make sure the center is attached under the (virtual) master root.
      // Atlas does not model the master root explicitly, so interpret this as "make center a root".
      swc.appendRoot(center);

      if (ZSwc::parent(n1) == center) {
        swc.appendChild(n2, n1);
      } else {
        swc.appendChild(n1, n2);
      }
    }

    changed = true;
  }

  // Legacy detaches and deletes the center when every neighbor participates in the matching.
  if (changed && matchedPairs.size() * 2 == centerNeighborCount) {
    if (swc.beginChild(center) == swc.endChild(center)) {
      swc.erase(center);
    }
  }

  return changed;
}

} // namespace nim
