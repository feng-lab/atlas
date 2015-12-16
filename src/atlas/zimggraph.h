#ifndef ZIMGGRAPH_H
#define ZIMGGRAPH_H

#include "zimgneighborhooditerator.h"
#include "zimgneighborhoodwithptriterator.h"
#include <boost/graph/adjacency_list.hpp>
#include <functional>

namespace nim {

class ZImgGraph
{
public:
  ZImgGraph(const ZImg& img, const ZImgRegion& rgn = ZImgRegion());

  // default is 8-conn for 2d and 26-conn for 3d
  void setConnectivity(size_t n);

  // whether to use voxel size of img to calcuate voxel distance, default is true
  inline void setUseVoxelSize(bool v) { m_useVoxelSize = v; m_graphIsValid = false; }

  // EdgeWeightFunc take voxel distance and voxel intensities as parameter
  // return edge weight as double value
  void build(const std::function<double(double, double, double)>& edgeWeightFunc);

  // return shortest distance from startIdx (idx of region) to other voxels
  // optionally return predecessor of each voxel if predecessor is not nullptr
  std::vector<double> shortestPaths(size_t startIdx, std::vector<size_t> *predecessor = nullptr);

  // overload, accept img coord rather than region idx
  std::vector<double> shortestPaths(const ZVoxelCoordinate& startCoord, std::vector<size_t> *predecessor = nullptr);

  // astar version, return cost and first reached target idx
  std::tuple<double, size_t> shortestPath(size_t startIdx, const std::vector<size_t>& targetIdxs,
                                          std::vector<size_t> *resPath = nullptr);

protected:
  void updateNeighborDistances();

  template <typename TVoxel>
  void addEdges_Opt();

  template <typename TVoxel>
  void addEdges();

private:
  const ZImg& m_img;
  ZImgRegion m_region;
  ZImgInfo m_regionInfo;
  ZNeighborhood m_neighborhood;
  bool m_useVoxelSize;
  std::vector<double> m_dists; // dists for each neighbor
  bool m_graphIsValid;

  struct EdgeInfo
  {
    EdgeInfo(double w)
      : weight(w)
    {}
    double weight;
  };

  typedef boost::adjacency_list<boost::vecS, boost::vecS,
      boost::undirectedS, boost::no_property, EdgeInfo,
      boost::no_property, boost::vecS> GraphT;
  typedef boost::graph_traits<GraphT>::vertex_descriptor Vertex;
  typedef boost::graph_traits<GraphT>::edge_descriptor Edge;

  GraphT m_graph;
  double m_lowestWeight;
  std::function<double(double, double, double)> m_edgeWeightFunction;

// some predefined edge weight functor
public:
  struct EdgeWeight1
  {
    inline double operator()(double dist, double v1, double v2) const
    {
      return dist * (1.0 / (v1 + 1.0) + 1.0 / (v2 + 1.0));
    }
  };

  struct EdgeWeight2
  {
    EdgeWeight2(double thre, double scale)
      : m_thre(thre), m_scale(scale)
    {}
    inline double operator()(double dist, double v1, double v2) const
    {
      return dist *
          (1.0 / (1.0 + std::exp((v1 - m_thre) / m_scale))
           + 1.0 / (1.0 + std::exp((v2 - m_thre) / m_scale))
           + 0.00001);
    }
  private:
    double m_thre;
    double m_scale;
  };
};

} // namespace nim

#endif // ZIMGGRAPH_H
