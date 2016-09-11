#include "zimgmerge.h"

#include "zimgtile.h"
#include "zvoxelregion.h"
#include "zstatisticsutils.h"
#include "zlog.h"
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/connected_components.hpp>
#include <boost/graph/kruskal_min_spanning_tree.hpp>
#include <boost/graph/filtered_graph.hpp>

namespace {

using namespace nim;

template<typename TVoxel>
void mergeMin_Impl(const ZVoxelRegion& region, const ZVoxelCoordinate& minCoord,
                   ZImg& res, const std::vector<ZImgTile>& tiles)
{
  for (const auto& coord : region) {
    TVoxel v = std::numeric_limits<TVoxel>::max();
    for (size_t i = 0; i < tiles.size(); ++i) {
      if (tiles[i].contains(coord)) {
        TVoxel tmp = tiles[i].value<TVoxel>(coord);
        if (tmp < v)
          v = tmp;
      }
    }
    ZVoxelCoordinate locInRes = coord - minCoord;
    *res.data<TVoxel>(locInRes) = v;
  }
}

template<typename TVoxel>
void mergeMax_Impl(const ZVoxelRegion& region, const ZVoxelCoordinate& minCoord,
                   ZImg& res, const std::vector<ZImgTile>& tiles)
{
  for (const auto& coord : region) {
    TVoxel v = std::numeric_limits<TVoxel>::min();
    for (size_t i = 0; i < tiles.size(); ++i) {
      if (tiles[i].contains(coord)) {
        TVoxel tmp = tiles[i].value<TVoxel>(coord);
        if (tmp > v)
          v = tmp;
      }
    }
    ZVoxelCoordinate locInRes = coord - minCoord;
    *res.data<TVoxel>(locInRes) = v;
  }
}

template<typename TVoxel>
void mergeMean_Impl(const ZVoxelRegion& region, const ZVoxelCoordinate& minCoord,
                    ZImg& res, const std::vector<ZImgTile>& tiles)
{
  for (const auto& coord : region) {
    std::vector<TVoxel> buf;
    for (size_t i = 0; i < tiles.size(); ++i) {
      if (tiles[i].contains(coord))
        buf.push_back(tiles[i].value<TVoxel>(coord));
    }
    CHECK(buf.size() > 1);

    ZVoxelCoordinate locInRes = coord - minCoord;
    *res.data<TVoxel>(locInRes) = static_cast<TVoxel>(mean(buf.begin(), buf.end()));;
  }
}

template<typename TVoxel>
void mergeMedian_Impl(const ZVoxelRegion& region, const ZVoxelCoordinate& minCoord,
                      ZImg& res, const std::vector<ZImgTile>& tiles)
{
  for (const auto& coord : region) {
    std::vector<TVoxel> buf;
    for (size_t i = 0; i < tiles.size(); ++i) {
      if (tiles[i].contains(coord))
        buf.push_back(tiles[i].value<TVoxel>(coord));
    }
    CHECK(buf.size() > 1);

    ZVoxelCoordinate locInRes = coord - minCoord;
    *res.data<TVoxel>(locInRes) = static_cast<TVoxel>(medianInPlace(buf.begin(), buf.end()));;
  }
}

template<typename TVoxel>
void merge_Impl(const ZVoxelRegion& region, const ZVoxelCoordinate& minCoord, ZImgMerge::Mode mode,
                ZImg& res, const std::vector<ZImgTile>& tiles)
{
  switch (mode) {
    case ZImgMerge::Mode::Min:
      mergeMin_Impl<TVoxel>(region, minCoord, res, tiles);
      break;
    case ZImgMerge::Mode::Max:
      mergeMax_Impl<TVoxel>(region, minCoord, res, tiles);
      break;
    case ZImgMerge::Mode::Mean:
      mergeMean_Impl<TVoxel>(region, minCoord, res, tiles);
      break;
    case ZImgMerge::Mode::Median:
      mergeMedian_Impl<TVoxel>(region, minCoord, res, tiles);
      break;
    case ZImgMerge::Mode::First:
    default:
      break;
  }
}

struct EdgeInfo
{
  explicit EdgeInfo(double c)
    : cost(c)
  {}

  double cost;
};

struct VertexInfo
{
  VertexInfo(const ZImg* im, size_t idx_)
    : img(im), idx(idx_)
  {}

  const ZImg* img;
  size_t idx;
  boost::default_color_type m_algo_color;
};

template<typename Edge>
struct edge_in_MST
{
  edge_in_MST() = default;

  explicit edge_in_MST(const std::vector<Edge>& mstEdges)
    : m_MSTEdges(&mstEdges)
  {
  }

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
  void tree_edge(Edge e, const Graph& /*unused*/) const
  {
    m_dfsSortedEdges.push_back(e);
  }

private:
  std::vector<Edge>& m_dfsSortedEdges;
};

} // namespace

namespace nim {

void ZImgMerge::addImg(const ZImg& img, const ZVoxelCoordinate& loc, const QString& imgName)
{
  if (!img.isEmpty()) {
    m_imgCoords[&img] = loc;
    m_imgNames[&img] = imgName;
  }
}

void
ZImgMerge::addImgPair(const ZImg& img1, const ZImg& img2, const ZVoxelCoordinate& img2Offset, double connectionCost,
                      const QString& img1Name, const QString& img2Name)
{
  if (img1.isEmpty() || img2.isEmpty())
    return;
  if (!img2.isSameType(img1))
    return;
  if (m_imgPairs.find(std::make_pair(&img2, &img1)) != m_imgPairs.end())
    m_imgPairs[std::make_pair(&img2, &img1)] = std::make_pair(-img2Offset, connectionCost);
  else
    m_imgPairs[std::make_pair(&img1, &img2)] = std::make_pair(img2Offset, connectionCost);
  m_imgNames[&img1] = img1Name;
  m_imgNames[&img2] = img2Name;
}

void ZImgMerge::removeImg(const ZImg& img)
{
  m_imgCoords.erase(&img);
  m_imgNames.erase(&img);
  std::map<std::pair<const ZImg*, const ZImg*>, std::pair<ZVoxelCoordinate, double>>::iterator it = m_imgPairs.begin();
  while (it != m_imgPairs.end()) {
    if (it->first.first == &img || it->first.second == &img) {
      it = m_imgPairs.erase(it);
    } else {
      ++it;
    }
  }
}

void ZImgMerge::removeImgConnection(const ZImg& img1, const ZImg& img2)
{
  std::map<std::pair<const ZImg*, const ZImg*>, std::pair<ZVoxelCoordinate, double>>::iterator it = m_imgPairs.begin();
  while (it != m_imgPairs.end()) {
    if (it->first == std::make_pair(&img1, &img2) || it->first == std::make_pair(&img2, &img1)) {
      it = m_imgPairs.erase(it);
    } else {
      ++it;
    }
  }
}

ZImg ZImgMerge::merge(Mode mode, QString* summary) const
{
  ZImg res;
  if (m_imgCoords.empty() && m_imgPairs.empty()) {
    throw ZImgException("Merge Imgs error: no Input Img");
  }

  const ZImg* refImg = nullptr;
  for (const auto& imgCoord : m_imgCoords) {
    if (!refImg) {
      refImg = imgCoord.first;
    } else if (!imgCoord.first->isSameType(*refImg)) {
      throw ZImgException("Merge Imgs error: imgs are not same type");
    }
  }
  double minCost = std::numeric_limits<double>::max();
  for (const auto& imgimgOffsetCost : m_imgPairs) {
    minCost = std::min(minCost, imgimgOffsetCost.second.second);
    if (!refImg) {
      refImg = imgimgOffsetCost.first.first;
    } else if (!imgimgOffsetCost.first.first->isSameType(*refImg)) {
      throw ZImgException("Merge Imgs error: imgs are not same type");
    }
  }

  std::map<const ZImg*, ZVoxelCoordinate> imgs;
  QString summ;
  resolveLocations(imgs, refImg, minCost, summ);
  mergeImgs(res, imgs, mode, summ);

  LOG(INFO) << "merge summary:";
  LOG(INFO) << summ;
  if (summary)
    summ.swap(*summary);

  return res;
}

void ZImgMerge::resolveLocations(std::map<const ZImg*, ZVoxelCoordinate>& imgCoords,
                                 const ZImg* refImg, double minCost, QString& summary) const
{
  imgCoords = m_imgCoords;

  if (m_imgPairs.empty())
    return;

  using GraphT = boost::adjacency_list<boost::listS, boost::listS, boost::undirectedS, VertexInfo, EdgeInfo>;
  using Vertex = boost::graph_traits<GraphT>::vertex_descriptor;
  using Edge = boost::graph_traits<GraphT>::edge_descriptor;
  std::map<const ZImg*, Vertex> imgToVertexMapper;
  GraphT graph;

  size_t vIdx = 0;
  for (const auto& imgCoord : imgCoords) {
    if (imgToVertexMapper.find(imgCoord.first) == imgToVertexMapper.end()) {
      Vertex v = boost::add_vertex(VertexInfo(imgCoord.first, vIdx++), graph);
      imgToVertexMapper[imgCoord.first] = v;
    }
  }
  {
    // use low cost to connect imgs with absolute location
    std::map<const ZImg*, ZVoxelCoordinate>::iterator it = imgCoords.begin();
    if (it != imgCoords.end()) {
      std::map<const ZImg*, ZVoxelCoordinate>::iterator nextIt = it;
      ++nextIt;
      for (; nextIt != imgCoords.end(); ++nextIt, ++it) {
        boost::add_edge(imgToVertexMapper[it->first],
                        imgToVertexMapper[nextIt->first],
                        EdgeInfo(minCost - 1e3),
                        graph);
      }
    }
  }

  for (const auto& imgimgOffsetCost : m_imgPairs) {
    if (imgToVertexMapper.find(imgimgOffsetCost.first.first) == imgToVertexMapper.end()) {
      Vertex v = boost::add_vertex(VertexInfo(imgimgOffsetCost.first.first, vIdx++), graph);
      imgToVertexMapper[imgimgOffsetCost.first.first] = v;
    }
    if (imgToVertexMapper.find(imgimgOffsetCost.first.second) == imgToVertexMapper.end()) {
      Vertex v = boost::add_vertex(VertexInfo(imgimgOffsetCost.first.second, vIdx++), graph);
      imgToVertexMapper[imgimgOffsetCost.first.second] = v;
    }
    boost::add_edge(imgToVertexMapper[imgimgOffsetCost.first.first],
                    imgToVertexMapper[imgimgOffsetCost.first.second],
                    EdgeInfo(imgimgOffsetCost.second.second),
                    graph);
  }

  std::vector<int> c(boost::num_vertices(graph));
  int num = boost::connected_components(graph, boost::make_iterator_property_map(c.begin(),
                                                                                 boost::get(&VertexInfo::idx, graph)),
                                        boost::color_map(boost::get(&VertexInfo::m_algo_color, graph)));
  if (num != 1) {
    throw ZImgException("Merge Imgs error: imgs are not fully connected");
  }

  std::vector<Edge> tree;
  boost::kruskal_minimum_spanning_tree(graph, std::back_inserter(tree),
                                       boost::weight_map(boost::get(&EdgeInfo::cost, graph)).
                                         vertex_index_map(boost::get(&VertexInfo::idx, graph)));

  if (imgCoords.empty())
    imgCoords[refImg] = ZVoxelCoordinate();

  edge_in_MST<Edge> filter(tree);
  boost::filtered_graph<GraphT, edge_in_MST<Edge>> fg(graph, filter);

  std::vector<Edge> sortedEdges;
  dfs_edge_visitor<Edge> vis(sortedEdges);
  boost::depth_first_search(fg, boost::visitor(vis).root_vertex(imgToVertexMapper[refImg]).
    vertex_index_map(boost::get(&VertexInfo::idx, fg)).
    color_map(boost::get(&VertexInfo::m_algo_color, fg)));

  for (size_t i = 0; i < sortedEdges.size(); ++i) {
    const ZImg* img1 = graph[boost::source(sortedEdges[i], graph)].img;
    const ZImg* img2 = graph[boost::target(sortedEdges[i], graph)].img;
    bool img1HasLocation = imgCoords.find(img1) != imgCoords.end();
    bool img2HasLocation = imgCoords.find(img2) != imgCoords.end();

    if (img1HasLocation && !img2HasLocation) {
      std::map<std::pair<const ZImg*, const ZImg*>, std::pair<ZVoxelCoordinate, double>>::const_iterator pairIt;
      pairIt = m_imgPairs.find(std::make_pair(img1, img2));
      if (pairIt != m_imgPairs.end())
        imgCoords[img2] = imgCoords[img1] + pairIt->second.first;
      else {
        pairIt = m_imgPairs.find(std::make_pair(img2, img1));  // must exist
        imgCoords[img2] = imgCoords[img1] - pairIt->second.first;
      }
      summary += QString("tile %1 connects to tile %2 with cost %3\n")
        .arg(m_imgNames.at(img1))
        .arg(m_imgNames.at(img2))
        .arg(pairIt->second.second);
    } else if (!img1HasLocation && img2HasLocation) {
      std::map<std::pair<const ZImg*, const ZImg*>, std::pair<ZVoxelCoordinate, double>>::const_iterator pairIt;
      pairIt = m_imgPairs.find(std::make_pair(img1, img2));
      if (pairIt != m_imgPairs.end())
        imgCoords[img1] = imgCoords[img2] - pairIt->second.first;
      else {
        pairIt = m_imgPairs.find(std::make_pair(img2, img1));  // must exist
        imgCoords[img1] = imgCoords[img2] + pairIt->second.first;
      }
      summary += QString("tile %1 connects to tile %2 with cost %3\n")
        .arg(m_imgNames.at(img1))
        .arg(m_imgNames.at(img2))
        .arg(pairIt->second.second);
    } else {
      CHECK(img1HasLocation && img2HasLocation);
    }
  }
}

void ZImgMerge::mergeImgs(ZImg& res, const std::map<const ZImg*, ZVoxelCoordinate>& imgs,
                          ZImgMerge::Mode mode, QString& summary) const
{
  std::vector<ZImgTile> tiles;
  for (const auto& imgCoord : imgs)
    tiles.emplace_back(imgCoord.first, imgCoord.second);

  ZVoxelRegion allRegion;
  ZVoxelRegion overlapRegion;

  for (size_t i = 0; i < tiles.size(); ++i) {
    allRegion.addBox(tiles[i].location(), tiles[i].maxCoord());

    ZVoxelRegion r1;
    r1.addBox(tiles[i].location(), tiles[i].maxCoord());
    for (size_t j = i + 1; j < tiles.size(); ++j) {
      ZVoxelRegion r2;
      r2.addBox(tiles[j].location(), tiles[j].maxCoord());
      if (!r2.intersect(r1).isEmpty())
        overlapRegion.unite(r2);
    }
  }

  ZVoxelCoordinate minCoord;
  ZVoxelCoordinate maxCoord;
  allRegion.getBoundBox(minCoord, maxCoord);
  ZVoxelCoordinate resSize = maxCoord - minCoord + 1;
  // create result with correct meta info (channel color, time stamp, voxel size...)
  res.infoRef() = tiles[0].img().info();
  res.infoRef().width = resSize.x;
  res.infoRef().height = resSize.y;
  res.infoRef().depth = resSize.z;
  res.infoRef().numChannels = resSize.c;
  res.infoRef().numTimes = resSize.t;
  res.infoRef().createDefaultDescriptions();
  res.allocate();

  // to get correct meta info from tile img
  std::set<size_t> neededChannelInfo;
  for (size_t i = 0; i < res.numChannels(); ++i) {
    neededChannelInfo.insert(i);
  }
  std::set<size_t> neededTimeStamp;
  for (size_t i = 0; i < res.numTimes(); ++i) {
    neededTimeStamp.insert(i);
  }

  for (size_t i = 0; i < tiles.size(); ++i) {
    ZVoxelCoordinate tileLoc = tiles[i].location() - minCoord;
    if (mode == Mode::Max) {
      res.pasteImgMax(tiles[i].img(), tileLoc);
    } else {
      res.pasteImg(tiles[i].img(), tileLoc);
    }
    summary += QString("tile %1 final offset %2\n").arg(m_imgNames.at(&(tiles[i].img()))).arg(tileLoc.toQString());

    if (!neededChannelInfo.empty()) { // some channnel info need to be filled
      size_t tileStartC = tileLoc.c;
      size_t tileEndC = tileStartC + tiles[i].img().numChannels();
      for (std::set<size_t>::iterator it = neededChannelInfo.begin(); it != neededChannelInfo.end();) {
        if (*it >= tileStartC && *it < tileEndC) {
          res.infoRef().channelColors[*it] = tiles[i].img().info().channelColors[*it - tileStartC];
          res.infoRef().channelNames[*it] = tiles[i].img().info().channelNames[*it - tileStartC];
          it = neededChannelInfo.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (!neededTimeStamp.empty()) { // some time stamp info need to be filled
      size_t tileStartT = tileLoc.t;
      size_t tileEndT = tileStartT + tiles[i].img().numTimes();
      for (std::set<size_t>::iterator it = neededTimeStamp.begin(); it != neededTimeStamp.end();) {
        if (*it >= tileStartT && *it < tileEndT) {
          res.infoRef().timeStamps[*it] = tiles[i].img().info().timeStamps[*it - tileStartT];
          it = neededTimeStamp.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  if (!neededChannelInfo.empty() || !neededTimeStamp.empty()) {
    LOG(FATAL) << "check";
  }

  if (mode != Mode::First && mode != Mode::Max) {
    // now merge overlap region
    IMG_TYPED_CALL(merge_Impl, res, overlapRegion, minCoord, mode, res, tiles);
  }
}

} // namespace nim
