#pragma once

#include "zimgneighborhooditerator.h"
#include "zimgneighborhoodwithptriterator.h"
#include <boost/graph/adjacency_list.hpp>
#include <functional>

namespace nim {

class ZImgGraph
{
public:
  explicit ZImgGraph(const ZImg& img, const ZImgRegion& rgn = ZImgRegion());

  // default is 8-conn for 2d and 26-conn for 3d
  void setConnectivity(size_t n);

  // whether to use voxel size of img to calcuate voxel distance, default is true
  void setUseVoxelSize(bool v)
  {
    m_useVoxelSize = v;
    m_graphIsValid = false;
  }

  // EdgeWeightFunctor take voxel distance and voxel intensities as parameter
  // return edge weight as double value
  template<typename EdgeWeightFunctor>
  void build(const EdgeWeightFunctor& edgeWeightFunc)
  {
    m_graph.clear();
    m_lowestWeight = std::numeric_limits<double>::max();

    updateNeighborDistances();

    for (size_t i = 0; i < m_regionInfo.voxelNumber(); ++i) {
      boost::add_vertex(m_graph);
    }

    if (m_region.containsWholePlane(m_img.info())) {
      IMG_TYPED_CALL(addEdges_Opt, m_img.info(), edgeWeightFunc)
    } else {
      IMG_TYPED_CALL(addEdges, m_img.info(), edgeWeightFunc)
    }

    m_graphIsValid = true;
  }

  // return shortest distance from startIdx (idx of region) to other voxels
  // optionally return predecessor of each voxel if predecessor is not nullptr
  std::vector<double> shortestPaths(size_t startIdx, std::vector<size_t>* predecessor = nullptr);

  // overload, accept img coord rather than region idx
  std::vector<double> shortestPaths(const ZVoxelCoordinate& startCoord, std::vector<size_t>* predecessor = nullptr);

  // astar version, return cost and first reached target idx
  std::tuple<double, size_t>
  shortestPath(size_t startIdx, const std::vector<size_t>& targetIdxs, std::vector<size_t>* resPath = nullptr);

protected:
  void updateNeighborDistances();

  template<typename TVoxel, typename EdgeWeightFunctor>
  void addEdges_Opt(const EdgeWeightFunctor& edgeWeightFunc)
  {
    ZImgNeighborhoodConstIterator<TVoxel> nit = ZImgNeighborhoodConstIterator<TVoxel>(m_neighborhood, m_img, m_region);
    const TVoxel* data = m_img.planeData<TVoxel>(m_region.zStart(), m_region.cStart(), m_region.tStart());
    for (; !nit.isAtEnd(); ++nit) {
      for (size_t n = 0; n < nit.numNeighbors(); ++n) {
        if (nit.isInBound(n)) {
          size_t nidx = nit.index(n);
          double weight = edgeWeightFunc(m_dists[n], data[nit.index()], data[nidx]);
          boost::add_edge(nit.index(), nidx, EdgeInfo(weight), m_graph);
          m_lowestWeight = std::min(m_lowestWeight, weight / m_dists[n]);
        }
      }
    }
  }

  template<typename TVoxel, typename EdgeWeightFunctor>
  void addEdges(const EdgeWeightFunctor& edgeWeightFunc)
  {
    ZImgNeighborhoodWithPtrConstIterator<TVoxel> nit =
      ZImgNeighborhoodWithPtrConstIterator<TVoxel>(m_neighborhood, m_img, m_region);
    for (; !nit.isAtEnd(); ++nit) {
      for (size_t n = 0; n < nit.numNeighbors(); ++n) {
        if (nit.isInBound(n)) {
          double weight = edgeWeightFunc(m_dists[n], *nit, nit.valueRef(n));
          boost::add_edge(nit.index(), nit.index(n), EdgeInfo(weight), m_graph);
          m_lowestWeight = std::min(m_lowestWeight, weight / m_dists[n]);
        }
      }
    }
  }

private:
  const ZImg& m_img;
  ZImgRegion m_region;
  ZImgInfo m_regionInfo;
  ZNeighborhood m_neighborhood;
  bool m_useVoxelSize = true;
  std::vector<double> m_dists; // dists for each neighbor
  bool m_graphIsValid = false;

  struct EdgeInfo
  {
    explicit EdgeInfo(double w)
      : weight(w)
    {}

    double weight;
  };

  using GraphT = boost::adjacency_list<boost::vecS,
                                       boost::vecS,
                                       boost::undirectedS,
                                       boost::no_property,
                                       EdgeInfo,
                                       boost::no_property,
                                       boost::vecS>;
  using Vertex = boost::graph_traits<GraphT>::vertex_descriptor;
  using Edge = boost::graph_traits<GraphT>::edge_descriptor;

  GraphT m_graph;
  double m_lowestWeight;
  std::function<double(double, double, double)> m_edgeWeightFunction;

  // some predefined edge weight functor

public:
  struct EdgeWeight1
  {
    double operator()(double dist, double v1, double v2) const
    {
      return dist * (1.0 / (v1 + 1.0) + 1.0 / (v2 + 1.0));
    }
  };

  struct EdgeWeight2
  {
    EdgeWeight2(double thre, double scale)
      : m_thre(thre)
      , m_scale(scale)
    {}

    double operator()(double dist, double v1, double v2) const
    {
      return dist * (1.0 / (1.0 + std::exp((v1 - m_thre) / m_scale)) + 1.0 / (1.0 + std::exp((v2 - m_thre) / m_scale)) +
                     0.00001);
    }

  private:
    double m_thre;
    double m_scale;
  };

  struct EdgeWeight3
  {
    EdgeWeight3(double thre, double scale)
      : m_thre(thre)
      , m_scale(scale)
    {}

    double operator()(double dist, double v1, double v2) const
    {
      if (v1 < m_thre || v2 < m_thre) {
        return 1000;
      }
      return dist * (1.0 / (1.0 + std::exp((v1 - m_thre) / m_scale)) + 1.0 / (1.0 + std::exp((v2 - m_thre) / m_scale)) +
                     0.00001);
    }

  private:
    double m_thre;
    double m_scale;
  };
};

} // namespace nim
