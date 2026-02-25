#include "zneutubegraph.h"

#include "zneutubedarrayqsort.h"

#include "zlog.h"

#include <algorithm>
#include <limits>

namespace nim::neutube {

int graphEdgeIndexLegacyLike(const GraphLegacyLike& graph, int v1, int v2)
{
  // Port of tz_graph.c::Graph_Edge_Index() with a linear scan instead of a hash table.
  const int nedge = static_cast<int>(graph.edges.size());
  for (int i = 0; i < nedge; ++i) {
    if (graph.edges[static_cast<size_t>(i)][0] == v1 && graph.edges[static_cast<size_t>(i)][1] == v2) {
      return i;
    }
  }
  return -1;
}

void graphAddEdgeLegacyLike(GraphLegacyLike* graph, int v1, int v2, double weight)
{
  CHECK(graph != nullptr);
  CHECK(v1 >= 0);
  CHECK(v2 >= 0);
  CHECK(v1 < graph->nvertex);
  CHECK(v2 < graph->nvertex);

  graph->edges.push_back({v1, v2});
  graph->weights.push_back(weight);
}

GraphMst2ResultLegacyLike graphToMst2LegacyLike(GraphLegacyLike* graph)
{
  // Port of tz_graph.c::Graph_To_Mst2().
  CHECK(graph != nullptr);

  const int nedge = static_cast<int>(graph->edges.size());
  GraphMst2ResultLegacyLike res;
  res.edgeIn.assign(static_cast<size_t>(nedge), 0u);

  if (nedge == 0) {
    return res;
  }

  CHECK(static_cast<int>(graph->weights.size()) == nedge) << "Graph_To_Mst2 requires a weighted graph";

  const int nvertex = graph->nvertex;
  CHECK(nvertex >= 0);

  std::vector<int> treeId(static_cast<size_t>(nvertex), 0);
  std::vector<int> connection(static_cast<size_t>(nvertex), -1);
  for (int i = 0; i < nvertex; ++i) {
    treeId[static_cast<size_t>(i)] = i;
  }

  std::vector<double> weights = graph->weights;
  std::vector<int> sortedEdgeIdx;
  darrayQsortLegacy(&weights, &sortedEdgeIdx);
  CHECK(static_cast<int>(sortedEdgeIdx.size()) == nedge);

  for (int i = 0; i < nedge; ++i) {
    const int edgeIndex = sortedEdgeIdx[static_cast<size_t>(i)];
    CHECK(edgeIndex >= 0 && edgeIndex < nedge);

    const int v1 = graph->edges[static_cast<size_t>(edgeIndex)][0];
    const int v2 = graph->edges[static_cast<size_t>(edgeIndex)][1];

    CHECK(v1 >= 0 && v1 < nvertex);
    CHECK(v2 >= 0 && v2 < nvertex);

    if (treeId[static_cast<size_t>(v1)] != treeId[static_cast<size_t>(v2)]) {
      const int tmpId = treeId[static_cast<size_t>(v2)];

      // Change ids of v2-tree to v1.
      int next = treeId[static_cast<size_t>(v2)];
      while (next >= 0) {
        CHECK(connection[static_cast<size_t>(next)] != next) << "self loop";
        treeId[static_cast<size_t>(next)] = treeId[static_cast<size_t>(v1)];
        next = connection[static_cast<size_t>(next)];
      }

      // Connect v1-tree and v2-tree.
      next = treeId[static_cast<size_t>(v1)];
      while (connection[static_cast<size_t>(next)] >= 0) {
        CHECK(connection[static_cast<size_t>(next)] != next) << "self loop";
        next = connection[static_cast<size_t>(next)];
      }
      connection[static_cast<size_t>(next)] = tmpId;

      res.edgeIn[static_cast<size_t>(edgeIndex)] = 1u;
    }
  }

  // Compact edges/weights in original order.
  int j = 0;
  for (int i = 0; i < nedge; ++i) {
    if (res.edgeIn[static_cast<size_t>(i)] == 1u) {
      if (i != j) {
        graph->edges[static_cast<size_t>(j)] = graph->edges[static_cast<size_t>(i)];
        graph->weights[static_cast<size_t>(j)] = graph->weights[static_cast<size_t>(i)];
      }
      ++j;
    }
  }

  graph->edges.resize(static_cast<size_t>(j));
  graph->weights.resize(static_cast<size_t>(j));

  return res;
}

} // namespace nim::neutube
