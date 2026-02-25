#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace nim::neutube {

// Minimal C++ graph container matching the legacy `tz_graph.h` usage patterns
// required by neuron reconstruction.
//
// Notes:
// - The graph is treated as undirected, but edge orientation is preserved in
//   `edges[i] = {v1, v2}` because downstream code relies on this ordering.
// - `weights` is optional but must match `edges.size()` when present.
struct GraphLegacyLike
{
  int nvertex = 0;
  std::vector<std::array<int, 2>> edges;
  std::vector<double> weights;
};

// Port of `Graph_Edge_Index(v1, v2)` semantics (orientation-sensitive).
[[nodiscard]] int graphEdgeIndexLegacyLike(const GraphLegacyLike& graph, int v1, int v2);

void graphAddEdgeLegacyLike(GraphLegacyLike& graph, int v1, int v2, double weight);

// Result bundle for the `Graph_To_Mst2` port.
struct GraphMst2ResultLegacyLike
{
  // Marks which original edges were included (size == original nedge).
  std::vector<std::uint8_t> edgeIn;
};

// Port of `tz_graph.c::Graph_To_Mst2()`.
//
// - Uses legacy `darray_qsort` ordering to preserve tie behavior.
// - Compacts `graph.edges` and `graph.weights` in-place to only the selected MST edges.
[[nodiscard]] GraphMst2ResultLegacyLike graphToMst2LegacyLike(GraphLegacyLike& graph);

} // namespace nim::neutube
