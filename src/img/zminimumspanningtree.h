#pragma once

#include "zglobal.h"
#include <utility>
#include <vector>
#include <set>

namespace nim {

// a simple class to avoid hide boost into cpp file
class ZMinimumSpanningTree
{
public:
  ZMinimumSpanningTree() = default;

  // vs and vt are index of the vertices, therefore numVertices = std::max(vs + 1, std::max(vt + 1, numVertices))
  void addEdge(size_t vs, size_t vt, double weight);

  // might through exception
  // if startVertexOfPreOrderResult < 0 (not a valid vertex), then returned pairs have no order
  // if startVertexOfPreOrderResult is valid, returned pairs are in pre-order (by dfs) but
  // exception will be thrown if graph is not connected
  [[nodiscard]] std::vector<std::pair<size_t, size_t>> runMST(index_t startVertexOfPreOrderResult = -1,
                                                              bool allowDisconnectedGraph = false);

private:
  std::vector<std::pair<size_t, size_t>> m_edges;
  std::vector<double> m_weights;
  size_t m_numVertices = 0;
};

} // namespace nim




