#pragma once

#include <utility>
#include <vector>
#include <set>

namespace nim {

// a simple class to avoid hide boost into cpp file
class ZMinimumSpanningTree
{
public:
  ZMinimumSpanningTree() = default;

  void addEdge(size_t s, size_t t, double weight);

  std::vector<std::pair<size_t, size_t>> runMST();

private:
  using E = std::pair<size_t, size_t>;
  std::vector<E> m_edges;
  std::vector<double> m_weights;
  std::set<size_t> m_vertices;
};

} // namespace nim




