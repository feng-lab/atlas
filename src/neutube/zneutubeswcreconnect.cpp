#include "zneutubeswcreconnect.h"

#include "zneutubegraph.h"

#include "zneutubeswcnodeops.h"

#include "zneutubeswcops.h"

#include "zneutubeswcgeom.h"

#include "zlog.h"

#include <cmath>
#include <limits>
#include <unordered_map>
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

  auto packOrderedPair = [](int a, int b) -> std::uint64_t {
    CHECK(a >= 0);
    CHECK(b >= 0);
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a)) << 32) |
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(b));
  };

  auto packUnorderedPair = [&](int a, int b) -> std::uint64_t {
    return (a <= b) ? packOrderedPair(a, b) : packOrderedPair(b, a);
  };

  std::vector<ZSwc::SwcTreeNode> nodeArray;
  nodeArray.reserve(tree->size());
  for (auto it = tree->begin(); it != tree->end(); ++it) {
    nodeArray.push_back(it);
  }

  const int nodeNumber = static_cast<int>(nodeArray.size());

  std::vector<std::vector<int>> nodesByLabel(static_cast<size_t>(treeNumber + 1));
  for (int i = 0; i < nodeNumber; ++i) {
    const int label = static_cast<int>(nodeArray[static_cast<size_t>(i)]->label);
    if (label >= 1 && label <= treeNumber) {
      nodesByLabel[static_cast<size_t>(label)].push_back(i);
    }
  }

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

  // Build the candidate edge list, matching tz_swc_tree.c::Swc_Tree_Reconnect.
  GraphLegacyLike graph;
  graph.nvertex = nodeNumber;
  graph.edges.reserve(static_cast<size_t>(nodeNumber));
  graph.weights.reserve(static_cast<size_t>(nodeNumber));

  for (int i = 0; i < nodeNumber; ++i) {
    const auto tn = nodeArray[static_cast<size_t>(i)];
    if (tn->id < 0) { // virtual node (legacy Swc_Tree_Node_Is_Regular check)
      continue;
    }
    if (isContinuation(tn)) {
      continue;
    }

    const int label1 = static_cast<int>(tn->label);
    CHECK(label1 >= 1 && label1 <= treeNumber);
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

      graphAddEdgeLegacyLike(&graph, i, closestIndex, w);
    }
  }

  // Port of tz_graph.c::Graph_Remove_Duplicated_Edge (orientation-sensitive, keeps first occurrence).
  {
    std::unordered_map<std::uint64_t, int> seen;
    seen.reserve(graph.edges.size());

    GraphLegacyLike deduped;
    deduped.nvertex = graph.nvertex;
    deduped.edges.reserve(graph.edges.size());
    deduped.weights.reserve(graph.weights.size());

    for (size_t i = 0; i < graph.edges.size(); ++i) {
      const int v1 = graph.edges[i][0];
      const int v2 = graph.edges[i][1];
      const std::uint64_t key = packOrderedPair(v1, v2);
      if (seen.find(key) != seen.end()) {
        continue;
      }
      seen.emplace(key, static_cast<int>(i));
      deduped.edges.push_back(graph.edges[i]);
      deduped.weights.push_back(graph.weights[i]);
    }

    graph = std::move(deduped);
  }

  // Build a label graph: for each pair of labels, keep the minimum-weight edge between the two trees.
  GraphLegacyLike subgraph;
  subgraph.nvertex = nodeNumber;

  GraphLegacyLike labelGraph;
  labelGraph.nvertex = treeNumber + 1;

  std::unordered_map<std::uint64_t, int> labelEdgeIndex;
  labelEdgeIndex.reserve(graph.edges.size());

  for (size_t i = 0; i < graph.edges.size(); ++i) {
    const int v1 = graph.edges[i][0];
    const int v2 = graph.edges[i][1];
    const double w = graph.weights[i];

    const int label1 = static_cast<int>(nodeArray[static_cast<size_t>(v1)]->label);
    const int label2 = static_cast<int>(nodeArray[static_cast<size_t>(v2)]->label);
    if (label1 == label2) {
      continue;
    }

    CHECK(label1 >= 1 && label1 <= treeNumber);
    CHECK(label2 >= 1 && label2 <= treeNumber);

    const std::uint64_t key = packUnorderedPair(label1, label2);
    auto it = labelEdgeIndex.find(key);
    if (it == labelEdgeIndex.end()) {
      graphAddEdgeLegacyLike(&labelGraph, label1, label2, w);
      graphAddEdgeLegacyLike(&subgraph, v1, v2, w);
      labelEdgeIndex.emplace(key, static_cast<int>(labelGraph.edges.size()) - 1);
    } else {
      const int edgeIndex = it->second;
      CHECK(edgeIndex >= 0 && edgeIndex < static_cast<int>(labelGraph.weights.size()));
      if (labelGraph.weights[static_cast<size_t>(edgeIndex)] > w) {
        labelGraph.weights[static_cast<size_t>(edgeIndex)] = w;
        subgraph.edges[static_cast<size_t>(edgeIndex)] = {v1, v2};
        subgraph.weights[static_cast<size_t>(edgeIndex)] = w;
      }
    }
  }

  // Kruskal MST over labels with legacy `darray_qsort` tie behavior.
  const GraphMst2ResultLegacyLike mst = graphToMst2LegacyLike(&labelGraph);
  CHECK(mst.edgeIn.size() == subgraph.edges.size());

  // Compact subgraph edges/weights in original order.
  int j = 0;
  const int nedge = static_cast<int>(subgraph.edges.size());
  for (int i = 0; i < nedge; ++i) {
    if (mst.edgeIn[static_cast<size_t>(i)] == 1u) {
      if (i != j) {
        subgraph.edges[static_cast<size_t>(j)] = subgraph.edges[static_cast<size_t>(i)];
        subgraph.weights[static_cast<size_t>(j)] = subgraph.weights[static_cast<size_t>(i)];
      }
      ++j;
    }
  }

  subgraph.edges.resize(static_cast<size_t>(j));
  subgraph.weights.resize(static_cast<size_t>(j));

  // Apply MST edges to connect trees (order matters for root selection).
  for (size_t i = 0; i < subgraph.edges.size(); ++i) {
    if (distThre >= 0.0 && subgraph.weights[i] > distThre) {
      continue;
    }

    const int v1 = subgraph.edges[i][0];
    const int v2 = subgraph.edges[i][1];
    CHECK(v1 >= 0 && v1 < nodeNumber);
    CHECK(v2 >= 0 && v2 < nodeNumber);

    ZSwc::SwcTreeNode tn1 = nodeArray[static_cast<size_t>(v1)];
    ZSwc::SwcTreeNode tn2 = nodeArray[static_cast<size_t>(v2)];

    tree->setAsRoot(tn1);
    tree->appendChild(tn2, tn1);
  }

  resortId(tree);
}

} // namespace nim::neutube
