#include "zimggraph.h"
#include <boost/graph/dijkstra_shortest_paths.hpp>

namespace nim {

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

std::vector<double> ZImgGraph::shortestPaths(const ZVoxelCoordinate &coord, std::vector<size_t> *predecessor)
{
  if (!m_region.containsCoord(coord, m_img.info())) {
    throw ZImgException(QString("Invalid start coord %1 for shortest path in img region <%2>")
                        .arg(coord.toQString()).arg(m_region.toQString()));
  }
  size_t startIdx = ZImg::coordToIndex(coord - m_region.start, m_regionInfo);
  return shortestPaths(startIdx, predecessor);
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
