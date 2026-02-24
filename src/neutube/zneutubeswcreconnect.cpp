#include "zneutubeswcreconnect.h"

#include "zneutubeswcnodeops.h"

#include "zneutubeswcops.h"

#include "zneutubeswcgeom.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nim::neutube {

int labelForest(ZSwc* tree)
{
  CHECK(tree != nullptr);

  int treeNumber = 0;
  for (auto root = tree->beginRoot(); root != tree->endRoot(); ++root) {
    ++treeNumber;
    for (auto it = tree->beginBreadthFirst(root); it != tree->endBreadthFirst(root); ++it) {
      it->label = treeNumber;
    }
  }

  return treeNumber;
}

void reconnectSwc(ZSwc* tree, double zScale, double distThre)
{
  CHECK(tree != nullptr);

  if (tree->numRoots() < 2) {
    return;
  }

  const int treeNumber = labelForest(tree);
  if (treeNumber < 2) {
    return;
  }

  std::vector<ZSwc::SwcTreeNode> nodeArray;
  nodeArray.reserve(tree->size());
  for (auto it = tree->begin(); it != tree->end(); ++it) {
    nodeArray.push_back(it);
  }

  const int nodeNumber = static_cast<int>(nodeArray.size());

  std::vector<std::vector<int>> nodesByLabel(static_cast<size_t>(treeNumber + 1));
  nodesByLabel.shrink_to_fit();
  for (int i = 0; i < nodeNumber; ++i) {
    const int label = static_cast<int>(nodeArray[static_cast<size_t>(i)]->label);
    CHECK(label >= 1 && label <= treeNumber);
    nodesByLabel[static_cast<size_t>(label)].push_back(i);
  }

  struct Edge
  {
    int v1 = -1;
    int v2 = -1;
    double w = 0.0;
  };

  auto pointDistNodeZ = [&](const std::vector<int>& candidates,
                            const std::array<double, 3>& pos) -> std::pair<double, int> {
    double mindist = std::numeric_limits<double>::infinity();
    int best = -1;

    for (const int idx : candidates) {
      const auto n = nodeArray[static_cast<size_t>(idx)];
      const double d = geo3dDist(pos[0], pos[1], pos[2] * zScale, n->x, n->y, n->z * zScale) - n->radius;

      if (mindist >= 0.0) {
        if (mindist > d) {
          mindist = d;
          best = idx;
        }
      } else if (d < 0.0) {
        CHECK(best >= 0);
        const double rBest = nodeArray[static_cast<size_t>(best)]->radius;
        if (mindist / rBest > d / n->radius) {
          mindist = d;
          best = idx;
        }
      }
    }

    return {mindist, best};
  };

  std::vector<Edge> edges;
  edges.reserve(static_cast<size_t>(nodeNumber));

  for (int i = 0; i < nodeNumber; ++i) {
    const auto tn = nodeArray[static_cast<size_t>(i)];
    if (isContinuation(tn)) {
      continue;
    }

    const int label1 = static_cast<int>(tn->label);
    const std::array<double, 3> pos = {tn->x, tn->y, tn->z};

    for (int label2 = 1; label2 <= treeNumber; ++label2) {
      if (label2 == label1) {
        continue;
      }

      const auto [dist, closestIndex] = pointDistNodeZ(nodesByLabel[static_cast<size_t>(label2)], pos);
      if (closestIndex < 0) {
        continue;
      }

      if (distThre >= 0.0 && dist > distThre) {
        continue;
      }

      double w = dist;
      if (w < 0.0) {
        w = 0.0;
      }

      edges.push_back({i, closestIndex, w});
    }
  }

  struct PairHash
  {
    size_t operator()(const std::pair<int, int>& p) const noexcept
    {
      return (static_cast<size_t>(p.first) << 32) ^ static_cast<size_t>(p.second);
    }
  };

  std::unordered_map<std::pair<int, int>, Edge, PairHash> uniqueEdges;
  uniqueEdges.reserve(edges.size());

  for (const auto& e : edges) {
    const int a = std::min(e.v1, e.v2);
    const int b = std::max(e.v1, e.v2);
    const std::pair<int, int> key = {a, b};
    auto it = uniqueEdges.find(key);
    if (it == uniqueEdges.end() || it->second.w > e.w) {
      uniqueEdges[key] = {a, b, e.w};
    }
  }

  edges.clear();
  edges.reserve(uniqueEdges.size());
  for (const auto& kv : uniqueEdges) {
    edges.push_back(kv.second);
  }

  // Build a label graph: for each pair of labels, keep the minimum-weight edge between the two trees.
  std::vector<Edge> labelEdges;
  std::vector<Edge> subgraphEdges;

  std::unordered_map<std::pair<int, int>, size_t, PairHash> labelEdgeIndex;
  labelEdgeIndex.reserve(edges.size());

  for (const auto& e : edges) {
    const int label1 = static_cast<int>(nodeArray[static_cast<size_t>(e.v1)]->label);
    const int label2 = static_cast<int>(nodeArray[static_cast<size_t>(e.v2)]->label);
    if (label1 == label2) {
      continue;
    }

    const int a = std::min(label1, label2);
    const int b = std::max(label1, label2);
    const std::pair<int, int> key = {a, b};

    auto it = labelEdgeIndex.find(key);
    if (it == labelEdgeIndex.end()) {
      const size_t idx = labelEdges.size();
      labelEdgeIndex[key] = idx;
      labelEdges.push_back({a, b, e.w});
      subgraphEdges.push_back(e);
    } else {
      const size_t idx = it->second;
      if (labelEdges[idx].w > e.w) {
        labelEdges[idx].w = e.w;
        subgraphEdges[idx] = e;
      }
    }
  }

  // Kruskal MST over labels.
  struct DSU
  {
    std::vector<int> parent;
    explicit DSU(int n)
      : parent(static_cast<size_t>(n))
    {
      for (int i = 0; i < n; ++i) {
        parent[static_cast<size_t>(i)] = i;
      }
    }

    int find(int x)
    {
      int r = x;
      while (parent[static_cast<size_t>(r)] != r) {
        r = parent[static_cast<size_t>(r)];
      }
      while (parent[static_cast<size_t>(x)] != x) {
        const int p = parent[static_cast<size_t>(x)];
        parent[static_cast<size_t>(x)] = r;
        x = p;
      }
      return r;
    }

    bool unite(int a, int b)
    {
      a = find(a);
      b = find(b);
      if (a == b) {
        return false;
      }
      parent[static_cast<size_t>(b)] = a;
      return true;
    }
  };

  std::vector<size_t> edgeOrder(labelEdges.size());
  for (size_t i = 0; i < edgeOrder.size(); ++i) {
    edgeOrder[i] = i;
  }
  std::sort(edgeOrder.begin(), edgeOrder.end(), [&](size_t a, size_t b) {
    return labelEdges[a].w < labelEdges[b].w;
  });

  DSU dsu(treeNumber + 1);
  std::vector<bool> inMst(labelEdges.size(), false);

  for (const size_t ei : edgeOrder) {
    const int a = labelEdges[ei].v1;
    const int b = labelEdges[ei].v2;
    if (dsu.unite(a, b)) {
      inMst[ei] = true;
    }
  }

  // Apply MST edges to connect trees.
  for (size_t i = 0; i < subgraphEdges.size(); ++i) {
    if (!inMst[i]) {
      continue;
    }

    const Edge& e = subgraphEdges[i];
    if (distThre >= 0.0 && e.w > distThre) {
      continue;
    }

    ZSwc::SwcTreeNode tn1 = nodeArray[static_cast<size_t>(e.v1)];
    ZSwc::SwcTreeNode tn2 = nodeArray[static_cast<size_t>(e.v2)];

    tree->setAsRoot(tn1);
    tree->appendChild(tn2, tn1);
  }
}

} // namespace nim::neutube
