#include "zminimumspanningtree.h"

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/kruskal_min_spanning_tree.hpp>

namespace nim {

void ZMinimumSpanningTree::addEdge(size_t s, size_t t, double weight)
{
  m_edges.emplace_back(s, t);
  m_weights.push_back(weight);
  m_vertices.insert(s);
  m_vertices.insert(t);
}

std::vector<std::pair<size_t, size_t>> ZMinimumSpanningTree::runMST()
{
  typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::undirectedS,
  boost::no_property, boost::property<boost::edge_weight_t, double>> Graph;
  typedef boost::graph_traits<Graph>::edge_descriptor Edge;
  // typedef boost::graph_traits<Graph>::vertex_descriptor Vertex;
  Graph g(m_edges.data(), m_edges.data() + m_edges.size(), m_weights.data(), m_vertices.size());
  // boost::property_map<Graph, boost::edge_weight_t>::type weight = get(boost::edge_weight, g);
  std::vector<Edge> spanning_tree;
  boost::kruskal_minimum_spanning_tree(g, std::back_inserter(spanning_tree));

  std::vector<std::pair<size_t, size_t>> res;
  for (auto ei = spanning_tree.begin(); ei != spanning_tree.end(); ++ei) {
    res.emplace_back(boost::source(*ei, g), boost::target(*ei, g));
  }
  return res;
}

} // namespace nim

