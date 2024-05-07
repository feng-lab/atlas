#include "zimagetransformresolve.h"

#include "zlog.h"
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/connected_components.hpp>
#include <boost/graph/kruskal_min_spanning_tree.hpp>
#include <boost/graph/filtered_graph.hpp>

namespace {

struct EdgeInfo
{
  explicit EdgeInfo(double c)
    : cost(c)
  {}

  double cost;
};

struct VertexInfo
{
  VertexInfo(size_t imgidx, size_t idx_)
    : img(imgidx)
    , idx(idx_)
  {}

  size_t img;
  size_t idx;
  boost::default_color_type m_algo_color;
};

template<typename Edge>
struct edge_in_MST
{
  edge_in_MST() = default;

  explicit edge_in_MST(const std::vector<Edge>& mstEdges)
    : m_MSTEdges(&mstEdges)
  {}

  bool operator()(const Edge& e) const
  {
    for (size_t i = 0; i < m_MSTEdges->size(); ++i) {
      if (e == (*m_MSTEdges)[i]) {
        return true;
      }
    }
    return false;
  }

private:
  const std::vector<Edge>* m_MSTEdges;
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

void ZImageTransformResolve::addFixedImage(size_t idx, const ZImageTransform* tfm)
{
  m_idxTransforms[idx] = tfm;
}

void ZImageTransformResolve::addImagePair(size_t fixedIdx,
                                          size_t movingIdx,
                                          const ZImageTransform* tfm,
                                          double transformCost)
{
  m_idxPairs.erase(std::make_pair(fixedIdx, movingIdx));
  m_idxPairs.erase(std::make_pair(movingIdx, fixedIdx));
  m_idxPairs[std::make_pair(fixedIdx, movingIdx)] = std::make_pair(tfm, transformCost);
}

std::map<size_t, std::unique_ptr<ZImageCompositeTransform>> ZImageTransformResolve::resolve() const
{
  CHECK(!m_idxTransforms.empty());
  std::map<size_t, std::unique_ptr<ZImageCompositeTransform>> res;
  for (const auto& [idx, tfm] : m_idxTransforms) {
    res[idx] = std::make_unique<ZImageCompositeTransform>();
    res[idx]->addTransform(*tfm);
  }
  if (m_idxPairs.empty()) {
    return res;
  }

  size_t refIdx = m_idxTransforms.cbegin()->first;
  double minCost = std::numeric_limits<double>::max();
  for (const auto& [imgImg, tfmCost] : m_idxPairs) {
    minCost = std::min(minCost, tfmCost.second);
  }

  using GraphT = boost::adjacency_list<boost::listS, boost::listS, boost::undirectedS, VertexInfo, EdgeInfo>;
  using Vertex = boost::graph_traits<GraphT>::vertex_descriptor;
  using Edge = boost::graph_traits<GraphT>::edge_descriptor;
  std::map<size_t, Vertex> idxToVertexMapper;
  GraphT graph;

  size_t vIdx = 0;
  for (const auto& [idx, tfm] : m_idxTransforms) {
    if (!idxToVertexMapper.contains(idx)) {
      Vertex v = boost::add_vertex(VertexInfo(idx, vIdx++), graph);
      idxToVertexMapper[idx] = v;
    }
  }
  {
    // use low cost to connect imgs with absolute location
    if (auto it = m_idxTransforms.cbegin(); it != m_idxTransforms.cend()) {
      auto nextIt = it;
      ++nextIt;
      for (; nextIt != m_idxTransforms.cend(); ++nextIt, ++it) {
        boost::add_edge(idxToVertexMapper[it->first], idxToVertexMapper[nextIt->first], EdgeInfo(minCost - 1e3), graph);
      }
    }
  }

  for (const auto& [imgImg, tfmCost] : m_idxPairs) {
    if (!idxToVertexMapper.contains(imgImg.first)) {
      Vertex v = boost::add_vertex(VertexInfo(imgImg.first, vIdx++), graph);
      idxToVertexMapper[imgImg.first] = v;
    }
    if (!idxToVertexMapper.contains(imgImg.second)) {
      Vertex v = boost::add_vertex(VertexInfo(imgImg.second, vIdx++), graph);
      idxToVertexMapper[imgImg.second] = v;
    }
    boost::add_edge(idxToVertexMapper[imgImg.first], idxToVertexMapper[imgImg.second], EdgeInfo(tfmCost.second), graph);
  }

  std::vector<int> c(boost::num_vertices(graph));
  auto num =
    boost::connected_components(graph,
                                boost::make_iterator_property_map(c.begin(), boost::get(&VertexInfo::idx, graph)),
                                boost::color_map(boost::get(&VertexInfo::m_algo_color, graph)));
  if (num != 1) {
    throw ZException("Transform resolve error: images are not fully connected");
  }

  std::vector<Edge> tree;
  boost::kruskal_minimum_spanning_tree(
    graph,
    std::back_inserter(tree),
    boost::weight_map(boost::get(&EdgeInfo::cost, graph)).vertex_index_map(boost::get(&VertexInfo::idx, graph)));

  edge_in_MST<Edge> filter(tree);
  boost::filtered_graph<GraphT, edge_in_MST<Edge>> fg(graph, filter);

  std::vector<Edge> sortedEdges;
  dfs_edge_visitor<Edge> vis(sortedEdges);
  boost::depth_first_search(fg,
                            boost::visitor(vis)
                              .root_vertex(idxToVertexMapper[refIdx])
                              .vertex_index_map(boost::get(&VertexInfo::idx, fg))
                              .color_map(boost::get(&VertexInfo::m_algo_color, fg)));

  LOG(INFO) << "transform resolve summary:";
  for (const auto& sortedEdge : sortedEdges) {
    size_t img1 = graph[boost::source(sortedEdge, graph)].img;
    size_t img2 = graph[boost::target(sortedEdge, graph)].img;
    bool img1HasLocation = res.contains(img1);
    bool img2HasLocation = res.contains(img2);

    if (img1HasLocation && !img2HasLocation) {
      std::map<std::pair<size_t, size_t>, std::pair<const ZImageTransform*, double>>::const_iterator pairIt;
      pairIt = m_idxPairs.find(std::make_pair(img1, img2));
      res[img2] = std::unique_ptr<ZImageCompositeTransform>(static_cast<ZImageCompositeTransform*>(res[img1]->clone()));
      if (pairIt != m_idxPairs.end()) {
        res[img2]->addTransform(*pairIt->second.first);
      } else {
        pairIt = m_idxPairs.find(std::make_pair(img2, img1)); // must exist
        res[img2]->addTransform(pairIt->second.first->makeInverseTransform());
      }
      LOG(INFO) << fmt::format("{} connects to {} with cost {}, transform: {}",
                               img2,
                               img1,
                               pairIt->second.second,
                               res[img2]->toString());
    } else if (!img1HasLocation && img2HasLocation) {
      std::map<std::pair<size_t, size_t>, std::pair<const ZImageTransform*, double>>::const_iterator pairIt;
      pairIt = m_idxPairs.find(std::make_pair(img1, img2));
      res[img1] = std::unique_ptr<ZImageCompositeTransform>(static_cast<ZImageCompositeTransform*>(res[img2]->clone()));
      if (pairIt != m_idxPairs.end()) {
        res[img1]->addTransform(pairIt->second.first->makeInverseTransform());
      } else {
        pairIt = m_idxPairs.find(std::make_pair(img2, img1)); // must exist
        res[img1]->addTransform(*pairIt->second.first);
      }
      LOG(INFO) << fmt::format("{} connects to {} with cost {}, transform: {}",
                               img1,
                               img2,
                               pairIt->second.second,
                               res[img1]->toString());
    } else {
      CHECK(img1HasLocation && img2HasLocation);
    }
  }

  return res;
}

} // namespace nim
