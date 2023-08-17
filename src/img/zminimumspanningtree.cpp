#include "zminimumspanningtree.h"

#include "zexception.h"
#include "zlog.h"
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/connected_components.hpp>
#include <boost/graph/kruskal_min_spanning_tree.hpp>
#include <boost/graph/prim_minimum_spanning_tree.hpp>
#include <boost/graph/filtered_graph.hpp>
#include <boost/graph/undirected_dfs.hpp>

namespace {

template<typename Graph>
struct edge_in_MST
{
  typedef typename boost::graph_traits<Graph>::edge_descriptor Edge;
  typedef typename boost::graph_traits<Graph>::vertex_descriptor Vertex;

  edge_in_MST() = default;

  explicit edge_in_MST(const std::vector<Edge>& mstEdges)
    : m_MSTEdges(&mstEdges)
  {}

  explicit edge_in_MST(const std::vector<Vertex>& predecessorMap, const Graph& g)
    : m_predecessorMap(&predecessorMap)
    , m_graph(&g)
  {}

  bool operator()(const Edge& e) const
  {
    if (m_MSTEdges) {
      return std::find(m_MSTEdges->begin(), m_MSTEdges->end(), e) != m_MSTEdges->end();
    } else if (m_predecessorMap) {
      return m_predecessorMap->at(boost::source(e, *m_graph)) == boost::target(e, *m_graph) ||
             m_predecessorMap->at(boost::target(e, *m_graph)) == boost::source(e, *m_graph);
    } else {
      return false;
    }
  }

private:
  const std::vector<Edge>* m_MSTEdges = nullptr;
  const std::vector<Vertex>* m_predecessorMap = nullptr;
  const Graph* m_graph = nullptr;
};

template<typename Edge>
class dfs_edge_visitor : public boost::default_dfs_visitor
{
public:
  explicit dfs_edge_visitor(std::vector<Edge>& dfsSortedEdges)
    : m_dfsSortedEdges(dfsSortedEdges)
  {
    m_dfsSortedEdges.clear();
  }

  template<typename Graph>
  void tree_edge(Edge e, const Graph&) const
  {
    m_dfsSortedEdges.push_back(e);
  }

private:
  std::vector<Edge>& m_dfsSortedEdges;
};

} // namespace

namespace nim {

void ZMinimumSpanningTree::addEdge(size_t vs, size_t vt, double weight)
{
  CHECK(vs != vt) << vs << " " << vt << " " << weight;
  m_edges.emplace_back(vs, vt);
  m_weights.push_back(weight);
  m_numVertices = std::max(m_numVertices, std::max(vs + 1, vt + 1));
}

std::vector<std::pair<size_t, size_t>> ZMinimumSpanningTree::runMST(index_t startVertexOfPreOrderResult,
                                                                    bool allowDisconnectedGraph)
{
  std::vector<std::pair<size_t, size_t>> res;
  if (m_numVertices < 2) {
    return res;
  } else if (m_numVertices == 2 || (m_numVertices == 3 && m_edges.size() < 3)) {
    return m_edges;
  }
  CHECK(m_numVertices >= 3) << m_numVertices;

  enum MSTMethod
  {
    Kruskal,
    Prim
  };
  MSTMethod mstMethod;

  double graphDensity = 2.0 * m_edges.size() / (m_numVertices * (m_numVertices - 1.0));
  bool graphIsSparse = graphDensity < 0.4;
  bool startVertexIsValid =
    startVertexOfPreOrderResult >= 0 && startVertexOfPreOrderResult < static_cast<index_t>(m_numVertices);

  if (startVertexIsValid) {
    allowDisconnectedGraph = false;
  }

  if (allowDisconnectedGraph) {
    mstMethod = Kruskal;
  } else {
    mstMethod = graphIsSparse ? Kruskal : Prim;
  }

  if (mstMethod == Prim) {
    auto minCost = *std::min_element(m_weights.begin(), m_weights.end()) - 10.0;
    for (auto& cost : m_weights) {
      cost -= minCost;
    }
  }

  // LOG(INFO) << "running " << (mstMethod == Kruskal ? "Kruskal" : "Prim") << " minimum spanning tree...";

  typedef boost::adjacency_list<
    boost::setS,
    boost::vecS,
    boost::undirectedS,
    boost::property<boost::vertex_distance_t, double>,
    boost::property<boost::edge_weight_t, double, boost::property<boost::edge_color_t, boost::default_color_type>>>
    Graph;
  typedef boost::graph_traits<Graph>::edge_descriptor Edge;
  typedef boost::graph_traits<Graph>::vertex_descriptor Vertex;

  Graph g(m_edges.data(), m_edges.data() + m_edges.size(), m_weights.data(), m_numVertices);

  if (!allowDisconnectedGraph) {
    std::vector<size_t> component(m_numVertices);
    auto num = boost::connected_components(g, component.data());

    // std::vector<int>::size_type i;
    // cout << "Total number of components: " << num << endl;
    // for (i = 0; i != component.size(); ++i)
    //   cout << "Vertex " << i << " is in component " << component[i] << endl;
    // cout << endl;

    if (num != 1) {
      throw ZException("MST error: vertices are not fully connected");
    }
  }

  if (mstMethod == Kruskal) {
    std::vector<Edge> spanning_tree;
    boost::kruskal_minimum_spanning_tree(g, std::back_inserter(spanning_tree));

    if (startVertexIsValid) {
      boost::filtered_graph<Graph, edge_in_MST<Graph>> fg(g, edge_in_MST<Graph>(spanning_tree));

      std::vector<Edge> sortedEdges;
      dfs_edge_visitor<Edge> vis(sortedEdges);
      boost::undirected_dfs(fg,
                            boost::visitor(vis)
                              .root_vertex(static_cast<size_t>(startVertexOfPreOrderResult))
                              .edge_color_map(boost::get(boost::edge_color, g)));

      spanning_tree.swap(sortedEdges);
    }
    for (auto& ei : spanning_tree) {
      res.emplace_back(boost::source(ei, g), boost::target(ei, g));
    }
  } else if (mstMethod == Prim) {
    std::vector<Vertex> p(m_numVertices);
    boost::prim_minimum_spanning_tree(g, p.data());

    if (startVertexIsValid) {
      boost::filtered_graph<Graph, edge_in_MST<Graph>> fg(g, edge_in_MST<Graph>(p, g));

      std::vector<Edge> sortedEdges;
      dfs_edge_visitor<Edge> vis(sortedEdges);
      boost::undirected_dfs(fg,
                            boost::visitor(vis)
                              .root_vertex(static_cast<size_t>(startVertexOfPreOrderResult))
                              .edge_color_map(boost::get(boost::edge_color, g)));

      for (auto& sortedEdge : sortedEdges) {
        res.emplace_back(boost::source(sortedEdge, g), boost::target(sortedEdge, g));
      }
    } else {
      for (size_t i = 0; i != p.size(); ++i) {
        if (p[i] != i) {
          res.emplace_back(p[i], i);
        }
      }
    }
  } else {
    CHECK(false);
  }

  // LOG(INFO) << "end running " << (mstMethod == Kruskal ? "Kruskal" : "Prim") << " minimum spanning tree";
  return res;
}

} // namespace nim
