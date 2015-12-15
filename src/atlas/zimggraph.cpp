#include "zimggraph.h"
#include <boost/graph/dijkstra_shortest_paths.hpp>
#include <boost/graph/astar_search.hpp>

namespace nim {

// euclidean distance heuristic
template <class Graph>
class distance_heuristic : public boost::astar_heuristic<Graph, double>
{
public:
  typedef typename boost::graph_traits<Graph>::vertex_descriptor Vertex;
  distance_heuristic(const ZImgRegion& region, const ZImgInfo& regionInfo,
                     const std::vector<Vertex>& goals, double weight, bool useVoxelSize, const ZNeighborhood &nb)
    : m_region(region), m_regionInfo(regionInfo), m_goals(goals), m_weight(weight), m_useVoxelSize(useVoxelSize)
  {
    if (nb.size() == size_t(2) || nb.size() == size_t(3)) {
      // no diagonal connection
      m_wxyz = m_useVoxelSize ? (m_regionInfo.voxelSizeX + m_regionInfo.voxelSizeY + m_regionInfo.voxelSizeZ) : 3.0;
      m_wxy = m_useVoxelSize ? (m_regionInfo.voxelSizeX + m_regionInfo.voxelSizeY) : 2.0;
      m_wyz = m_useVoxelSize ? (m_regionInfo.voxelSizeZ + m_regionInfo.voxelSizeY) : 2.0;
      m_wxz = m_useVoxelSize ? (m_regionInfo.voxelSizeZ + m_regionInfo.voxelSizeX) : 2.0;
      m_wx = m_useVoxelSize ? m_regionInfo.voxelSizeX : 1.0;
      m_wy = m_useVoxelSize ? m_regionInfo.voxelSizeY : 1.0;
      m_wz = m_useVoxelSize ? m_regionInfo.voxelSizeZ : 1.0;
    } else { // there are many other cases but I am too lazy to write
      m_wxyz = m_useVoxelSize ? std::sqrt(m_regionInfo.voxelSizeX * m_regionInfo.voxelSizeX +
                                          m_regionInfo.voxelSizeY * m_regionInfo.voxelSizeY +
                                          m_regionInfo.voxelSizeZ * m_regionInfo.voxelSizeZ) : std::sqrt(3.0);
      m_wxy = m_useVoxelSize ? std::sqrt(m_regionInfo.voxelSizeX * m_regionInfo.voxelSizeX +
                                         m_regionInfo.voxelSizeY * m_regionInfo.voxelSizeY) : std::sqrt(2.0);
      m_wyz = m_useVoxelSize ? std::sqrt(m_regionInfo.voxelSizeZ * m_regionInfo.voxelSizeZ +
                                         m_regionInfo.voxelSizeY * m_regionInfo.voxelSizeY) : std::sqrt(2.0);
      m_wxz = m_useVoxelSize ? std::sqrt(m_regionInfo.voxelSizeZ * m_regionInfo.voxelSizeZ +
                                         m_regionInfo.voxelSizeX * m_regionInfo.voxelSizeX) : std::sqrt(2.0);
      m_wx = m_useVoxelSize ? m_regionInfo.voxelSizeX : 1.0;
      m_wy = m_useVoxelSize ? m_regionInfo.voxelSizeY : 1.0;
      m_wz = m_useVoxelSize ? m_regionInfo.voxelSizeZ : 1.0;
    }
  }

  double operator()(Vertex u)
  {
    double res = std::numeric_limits<double>::max();
    ZVoxelCoordinate vertexCoord = ZImg::indexToCoord(u, m_regionInfo) + m_region.start;
    for (Vertex v : m_goals) {
      ZVoxelCoordinate goalCoord = ZImg::indexToCoord(v, m_regionInfo) + m_region.start;
      // octile distance
      double dx = vertexCoord.x - goalCoord.x;
      double dy = vertexCoord.y - goalCoord.y;
      double dz = vertexCoord.z - goalCoord.z;
      double dxyz = std::min(dx, std::min(dy, dz));
      double dxy = std::min(dx, dy) - dxyz;
      double dyz = std::min(dy, dz) - dxyz;
      double dxz = std::min(dx, dz) - dxyz;
      dx = dx - dxyz - dxy - dxz;
      dy = dy - dxyz - dxy - dyz;
      dz = dz - dxyz - dyz - dxz;

      res = std::min(res, dxyz * m_wxyz + dxy * m_wxy + dyz * m_wyz + dxz * m_wxz + dx * m_wx + dy * m_wy + dz * m_wz);
      //      if (m_useVoxelSize) {
      //        double x = (vertexCoord.x - goalCoord.x) * m_regionInfo.voxelSizeX;
      //        double y = (vertexCoord.y - goalCoord.y) * m_regionInfo.voxelSizeY;
      //        double z = (vertexCoord.z - goalCoord.z) * m_regionInfo.voxelSizeZ;
      //        res = std::min(res, m_weight * std::sqrt(x*x + y*y + z*z));
      //      } else {
      //        double x = (vertexCoord.x - goalCoord.x);
      //        double y = (vertexCoord.y - goalCoord.y);
      //        double z = (vertexCoord.z - goalCoord.z);
      //        res = std::min(res, m_weight * std::sqrt(x*x + y*y + z*z));
      //      }
    }

    return res;
  }
private:
  const ZImgRegion& m_region;
  const ZImgInfo& m_regionInfo;
  const std::vector<Vertex>& m_goals;
  double m_weight;
  bool m_useVoxelSize;

  double m_wxyz;
  double m_wxy;
  double m_wyz;
  double m_wxz;
  double m_wx;
  double m_wy;
  double m_wz;
};

template <class Vertex>
struct found_goal
{
  found_goal(Vertex v) : v(v) {}
  Vertex v;
}; // exception for termination

// visitor that terminates when we find the goal
template <class Vertex>
class astar_goal_visitor : public boost::default_astar_visitor
{
public:
  astar_goal_visitor(const std::vector<Vertex>& goals) : m_goals(goals) {}
  template <class Graph>
  void examine_vertex(Vertex u, Graph&)
  {
    for (Vertex gv : m_goals) {
      if (gv == u)
        throw found_goal<Vertex>(gv);
    }
  }
private:
  const std::vector<Vertex>& m_goals;
};

ZImgGraph::ZImgGraph(const ZImg& img, const ZImgRegion& rgn)
  : m_img(img), m_region(rgn), m_useVoxelSize(true), m_graphIsValid(false)
{
  if (!rgn.isValid(m_img.info())) {
    throw ZImgException(QString("Can not build graph with invalid region <%1> of img <%2>")
                        .arg(m_region.toQString()).arg(m_img.info().toQString()));
  }

  m_region.resolveRegionEnd(m_img.info());
  m_regionInfo = m_region.clip(m_img.info());
  if (m_regionInfo.numChannels > 1 || m_regionInfo.numTimes > 1) {
    throw ZImgException(QString("Can only build graph with 3D or 2D single channel img region, current region: <%1>")
                        .arg(m_region.toQString()));
  }

  if (m_regionInfo.depth == 1) {
    setConnectivity(8);
  } else {
    setConnectivity(26);
  }
}

void ZImgGraph::setConnectivity(size_t n)
{
  m_neighborhood.set(n);
  m_neighborhood.removeSymmetricalOffsets();
  m_neighborhood.removeCenter();
  m_graphIsValid = false;
}

std::vector<double> ZImgGraph::shortestPaths(size_t startIdx, std::vector<size_t> *predecessor)
{
  if (!m_graphIsValid) {
    throw ZImgException("Img graph is not built yet");
  }
  if (startIdx >= boost::num_vertices(m_graph)) {
    throw ZImgException(QString("Invalid start idx %1 for shortest path in img region <%2>")
                        .arg(startIdx).arg(m_region.toQString()));
  }

  std::vector<double> distance(boost::num_vertices(m_graph));

  if (predecessor) {
    predecessor->resize(boost::num_vertices(m_graph));
    dijkstra_shortest_paths(m_graph, startIdx,
                            boost::weight_map(boost::get(&EdgeInfo::weight, m_graph)).
                            predecessor_map(boost::make_iterator_property_map(predecessor->begin(),
                                                                              boost::get(boost::vertex_index, m_graph))).
                            distance_map(boost::make_iterator_property_map(distance.begin(),
                                                                           boost::get(boost::vertex_index, m_graph))));
  } else {
    dijkstra_shortest_paths(m_graph, startIdx,
                            boost::weight_map(boost::get(&EdgeInfo::weight, m_graph)).
                            distance_map(boost::make_iterator_property_map(distance.begin(),
                                                                           boost::get(boost::vertex_index, m_graph))));
  }

  return distance;
}

std::vector<double> ZImgGraph::shortestPaths(const ZVoxelCoordinate &startCoord, std::vector<size_t> *predecessor)
{
  if (!m_region.containsCoord(startCoord, m_img.info())) {
    throw ZImgException(QString("Invalid start coord %1 for shortest path in img region <%2>")
                        .arg(startCoord.toQString()).arg(m_region.toQString()));
  }
  size_t startIdx = ZImg::coordToIndex(startCoord - m_region.start, m_regionInfo);
  return shortestPaths(startIdx, predecessor);
}

std::tuple<double, size_t> ZImgGraph::shortestPath(size_t startIdx, const std::vector<size_t>& targetIdxs,
                                                   std::vector<size_t> *resPath)
{
  if (!m_graphIsValid) {
    throw ZImgException("Img graph is not built yet");
  }
  if (startIdx >= boost::num_vertices(m_graph)) {
    throw ZImgException(QString("Invalid start idx %1 for shortest path in img region <%2>")
                        .arg(startIdx).arg(m_region.toQString()));
  }
  if (targetIdxs.empty()) {
    throw ZImgException("No target idxs");
  }
  for (size_t idx : targetIdxs) {
    if (idx >= boost::num_vertices(m_graph)) {
      throw ZImgException(QString("Invalid target idx %1 for shortest path in img region <%2>")
                          .arg(idx).arg(m_region.toQString()));
    }
  }

  std::vector<double> distance(boost::num_vertices(m_graph));
  std::vector<size_t> predecessor;
  try {
    if (resPath) {
      predecessor.resize(boost::num_vertices(m_graph));
      boost::astar_search(m_graph, startIdx,
                          distance_heuristic<GraphT>(m_region, m_regionInfo, targetIdxs, m_lowestWeight, m_useVoxelSize, m_neighborhood),
                          boost::weight_map(boost::get(&EdgeInfo::weight, m_graph)).
                          predecessor_map(boost::make_iterator_property_map(predecessor.begin(),
                                                                            boost::get(boost::vertex_index, m_graph))).
                          distance_map(boost::make_iterator_property_map(distance.begin(),
                                                                         boost::get(boost::vertex_index, m_graph))).
                          visitor(astar_goal_visitor<size_t>(targetIdxs)));
    } else {
      predecessor.resize(boost::num_vertices(m_graph));
      boost::astar_search(m_graph, startIdx,
                          distance_heuristic<GraphT>(m_region, m_regionInfo, targetIdxs, m_lowestWeight, m_useVoxelSize, m_neighborhood),
                          boost::weight_map(boost::get(&EdgeInfo::weight, m_graph)).
                          distance_map(boost::make_iterator_property_map(distance.begin(),
                                                                         boost::get(boost::vertex_index, m_graph))).
                          visitor(astar_goal_visitor<size_t>(targetIdxs)));
    }
  } catch (found_goal<size_t> fg) {
    if (resPath) {
      resPath->clear();
      for(size_t v = fg.v; ; v = predecessor[v]) {
        resPath->push_back(v);
        if (predecessor[v] == v) {
          break;
        }
      }
    }

    return std::make_tuple(distance[fg.v], fg.v);
  }

  throw ZImgException(QString("Didn't find a path from %1 to target points").arg(startIdx));
  return std::make_tuple(-1, size_t(-1));
}

void ZImgGraph::updateNeighborDistances()
{
  m_dists.resize(m_neighborhood.size());
  if (m_useVoxelSize) {
    for (size_t i=0; i<m_dists.size(); ++i) {
      double x = m_neighborhood.offset(i).x * m_img.info().voxelSizeX;
      double y = m_neighborhood.offset(i).y * m_img.info().voxelSizeY;
      double z = m_neighborhood.offset(i).z * m_img.info().voxelSizeZ;
      m_dists[i] = std::sqrt(x*x + y*y + z*z);
    }
  } else {
    for (size_t i=0; i<m_dists.size(); ++i) {
      m_dists[i] = std::sqrt(m_neighborhood.offset(i).x * m_neighborhood.offset(i).x +
                             m_neighborhood.offset(i).y * m_neighborhood.offset(i).y +
                             m_neighborhood.offset(i).z * m_neighborhood.offset(i).z);
    }
  }
}

} // namespace nim
