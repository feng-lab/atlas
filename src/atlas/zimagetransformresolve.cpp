#include "zimagetransformresolve.h"
#include <cassert>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/connected_components.hpp>
#include <boost/graph/kruskal_min_spanning_tree.hpp>
#include <boost/graph/filtered_graph.hpp>

namespace {

struct EdgeInfo
{
  EdgeInfo(double c)
    : cost(c)
  {}
  double cost;
};

struct VertexInfo
{
  VertexInfo(size_t imgidx, size_t idx)
    : img(imgidx), idx(idx)
  {}
  size_t img;
  size_t idx;
  boost::default_color_type m_algo_color;
};

template<typename Edge>
struct edge_in_MST {
  edge_in_MST() {}
  edge_in_MST(const std::vector<Edge>& mstEdges)
    : m_MSTEdges(&mstEdges)
  {
  }
  bool operator()(const Edge& e) const
  {
    for (size_t i=0; i<m_MSTEdges->size(); ++i) {
      if (e == m_MSTEdges->at(i)) {
        return true;
      }
    }
    return false;
  }
private:
  const std::vector<Edge>* m_MSTEdges;
};

template<typename Edge>
class dfs_edge_visitor : public boost::default_dfs_visitor {
public:
  dfs_edge_visitor(std::vector<Edge> &dfsSortedEdges)
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

}

namespace nim {

ZImageTransformResolve::ZImageTransformResolve()
{
}

void ZImageTransformResolve::addFixedImage(size_t idx, const ZImageTransform *tfm)
{
  m_idxTransforms[idx] = tfm;
}

void ZImageTransformResolve::addImagePair(size_t fixedIdx, size_t movingIdx, const ZImageTransform *tfm, double transformCost)
{
  m_idxPairs.erase(std::make_pair(fixedIdx, movingIdx));
  m_idxPairs.erase(std::make_pair(movingIdx, fixedIdx));
  m_idxPairs[std::make_pair(fixedIdx, movingIdx)] = std::make_pair(tfm, transformCost);
}

std::map<size_t, std::unique_ptr<ZImageCompositeTransform>> ZImageTransformResolve::resolve(QString *summary) const
{
  assert(!m_idxTransforms.empty());
  std::map<size_t, std::unique_ptr<ZImageCompositeTransform>> res;
  for (auto it = m_idxTransforms.cbegin(); it != m_idxTransforms.cend(); ++it) {
    res[it->first] = std::make_unique<ZImageCompositeTransform>();
    res[it->first]->addTransform(*it->second);
  }
  if (m_idxPairs.empty())
    return res;

  size_t refIdx = m_idxTransforms.cbegin()->first;
  double minCost = std::numeric_limits<double>::max();
  for (auto it = m_idxPairs.cbegin(); it != m_idxPairs.cend(); ++it) {
    minCost = std::min(minCost, it->second.second);
  }

  QString summ;

  typedef boost::adjacency_list<boost::listS, boost::listS,
      boost::undirectedS, VertexInfo, EdgeInfo> GraphT;
  typedef boost::graph_traits<GraphT>::vertex_descriptor Vertex;
  typedef boost::graph_traits<GraphT>::edge_descriptor Edge;
  std::map<size_t, Vertex> idxToVertexMapper;
  GraphT graph;

  size_t vIdx = 0;
  for (auto it = m_idxTransforms.cbegin(); it  != m_idxTransforms.cend(); ++it) {
    if (idxToVertexMapper.find(it->first) == idxToVertexMapper.end()) {
      Vertex v = boost::add_vertex(VertexInfo(it->first, vIdx++), graph);
      idxToVertexMapper[it->first] = v;
    }
  }
  {
  // use low cost to connect imgs with absolute location
  auto it = m_idxTransforms.cbegin();
  if (it != m_idxTransforms.cend()) {
    auto nextIt = it;
    ++nextIt;
    for (; nextIt != m_idxTransforms.cend(); ++nextIt, ++it) {
      boost::add_edge(idxToVertexMapper[it->first],
                      idxToVertexMapper[nextIt->first],
                      EdgeInfo(minCost - 1e3),
                      graph);
    }
  }
  }

  for (auto it = m_idxPairs.begin(); it != m_idxPairs.end(); ++it) {
    if (idxToVertexMapper.find(it->first.first) == idxToVertexMapper.end()) {
      Vertex v = boost::add_vertex(VertexInfo(it->first.first, vIdx++), graph);
      idxToVertexMapper[it->first.first] = v;
    }
    if (idxToVertexMapper.find(it->first.second) == idxToVertexMapper.end()) {
      Vertex v = boost::add_vertex(VertexInfo(it->first.second, vIdx++), graph);
      idxToVertexMapper[it->first.second] = v;
    }
    boost::add_edge(idxToVertexMapper[it->first.first],
                    idxToVertexMapper[it->first.second],
                    EdgeInfo(it->second.second),
                    graph);
  }

  std::vector<int> c(boost::num_vertices(graph));
  int num = boost::connected_components(graph, boost::make_iterator_property_map(c.begin(), boost::get(&VertexInfo::idx, graph)),
                                        boost::color_map(boost::get(&VertexInfo::m_algo_color, graph)));
  if (num != 1) {
    throw ZImgException("Transform resolve error: images are not fully connected");
  }

  std::vector<Edge> tree;
  boost::kruskal_minimum_spanning_tree(graph, std::back_inserter(tree),
                                       boost::weight_map(boost::get(&EdgeInfo::cost, graph)).
                                       vertex_index_map(boost::get(&VertexInfo::idx, graph)));

  edge_in_MST<Edge> filter(tree);
  boost::filtered_graph<GraphT, edge_in_MST<Edge>> fg(graph, filter);

  std::vector<Edge> sortedEdges;
  dfs_edge_visitor<Edge> vis(sortedEdges);
  boost::depth_first_search(fg, boost::visitor(vis).root_vertex(idxToVertexMapper[refIdx]).
                            vertex_index_map(boost::get(&VertexInfo::idx, fg)).
                            color_map(boost::get(&VertexInfo::m_algo_color, fg)));

  for (size_t i=0; i<sortedEdges.size(); ++i) {
    size_t img1 = graph[boost::source(sortedEdges[i], graph)].img;
    size_t img2 = graph[boost::target(sortedEdges[i], graph)].img;
    bool img1HasLocation = res.find(img1) != res.end();
    bool img2HasLocation = res.find(img2) != res.end();

    if (img1HasLocation && !img2HasLocation) {
      std::map<std::pair<size_t, size_t>, std::pair<const ZImageTransform*, double>>::const_iterator pairIt;
      pairIt = m_idxPairs.find(std::make_pair(img1, img2));
      res[img2] = std::unique_ptr<ZImageCompositeTransform>(dynamic_cast<ZImageCompositeTransform*>(res[img1]->clone()));
      if (pairIt != m_idxPairs.end()) {
        res[img2]->addTransform(*pairIt->second.first);
      } else {
        pairIt = m_idxPairs.find(std::make_pair(img2, img1));  // must exist
        res[img2]->addTransform(pairIt->second.first->makeInverseTransform());
      }
      summ += QString("%1 connects to %2 with cost %3\n").arg(img1).arg(img2).arg(pairIt->second.second);
    } else if (!img1HasLocation && img2HasLocation) {
      std::map<std::pair<size_t, size_t>, std::pair<const ZImageTransform*, double>>::const_iterator pairIt;
      pairIt = m_idxPairs.find(std::make_pair(img1, img2));
      res[img1] = std::unique_ptr<ZImageCompositeTransform>(dynamic_cast<ZImageCompositeTransform*>(res[img2]->clone()));
      if (pairIt != m_idxPairs.end())
        res[img1]->addTransform(pairIt->second.first->makeInverseTransform());
      else {
        pairIt = m_idxPairs.find(std::make_pair(img2, img1));  // must exist
        res[img1]->addTransform(*pairIt->second.first);
      }
      summ += QString("%1 connects to %2 with cost %3\n").arg(img1).arg(img2).arg(pairIt->second.second);
    } else {
      assert(img1HasLocation && img2HasLocation);
    }
  }

  LINFO() << "transform resolve summary:\n" << summ;
  if (summary)
    summ.swap(*summary);

  return res;
}

} // namespace nims
