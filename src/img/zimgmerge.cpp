#include "zimgmerge.h"

#include <utility>

#include "zstatisticsutils.h"
#include "zlog.h"
#include "zstringutils.h"
#include "zcpuinfo.h"
#include "zminimumspanningtree.h"

namespace {

using namespace nim;

template<typename TVoxel>
void mergeMin_Impl(const ZVoxelRegion& region,
                   const ZVoxelCoordinate& minCoord,
                   ZImg& res,
                   const std::vector<ZImgTile>& tiles)
{
  for (auto& tile : tiles) {
    tile.createImgCache();
  }
  for (auto coord : region) {
    TVoxel v = std::numeric_limits<TVoxel>::max();
    for (const auto& tile : tiles) {
      if (tile.contains(coord)) {
        auto tmp = tile.value<TVoxel>(coord);
        if (tmp < v) {
          v = tmp;
        }
      }
    }
    ZVoxelCoordinate locInRes = coord - minCoord;
    *res.data<TVoxel>(locInRes) = v;
  }
  for (auto& tile : tiles) {
    tile.clearImgCache();
  }
}

template<typename TVoxel>
void mergeMax_Impl(const ZVoxelRegion& region,
                   const ZVoxelCoordinate& minCoord,
                   ZImg& res,
                   const std::vector<ZImgTile>& tiles)
{
  for (auto& tile : tiles) {
    tile.createImgCache();
  }
  for (auto coord : region) {
    TVoxel v = std::numeric_limits<TVoxel>::lowest();
    for (const auto& tile : tiles) {
      if (tile.contains(coord)) {
        auto tmp = tile.value<TVoxel>(coord);
        if (tmp > v) {
          v = tmp;
        }
      }
    }
    ZVoxelCoordinate locInRes = coord - minCoord;
    *res.data<TVoxel>(locInRes) = v;
  }
  for (auto& tile : tiles) {
    tile.clearImgCache();
  }
}

template<typename TVoxel>
void mergeMean_Impl(const ZVoxelRegion& region,
                    const ZVoxelCoordinate& minCoord,
                    ZImg& res,
                    const std::vector<ZImgTile>& tiles)
{
  for (auto& tile : tiles) {
    tile.createImgCache();
  }
  for (auto coord : region) {
    std::vector<TVoxel> buf;
    for (const auto& tile : tiles) {
      if (tile.contains(coord)) {
        buf.push_back(tile.value<TVoxel>(coord));
      }
    }
    CHECK(buf.size() > 1);

    ZVoxelCoordinate locInRes = coord - minCoord;
    *res.data<TVoxel>(locInRes) = static_cast<TVoxel>(mean(buf.begin(), buf.end()));
  }
  for (auto& tile : tiles) {
    tile.clearImgCache();
  }
}

template<typename TVoxel>
void mergeMedian_Impl(const ZVoxelRegion& region,
                      const ZVoxelCoordinate& minCoord,
                      ZImg& res,
                      const std::vector<ZImgTile>& tiles)
{
  for (auto& tile : tiles) {
    tile.createImgCache();
  }
  for (auto coord : region) {
    std::vector<TVoxel> buf;
    for (const auto& tile : tiles) {
      if (tile.contains(coord)) {
        buf.push_back(tile.value<TVoxel>(coord));
      }
    }
    CHECK(buf.size() > 1);

    ZVoxelCoordinate locInRes = coord - minCoord;
    *res.data<TVoxel>(locInRes) = static_cast<TVoxel>(median(buf.begin(), buf.end()));
  }
  for (auto& tile : tiles) {
    tile.clearImgCache();
  }
}

template<typename TVoxel>
void merge_Impl(const ZVoxelRegion& region,
                const ZVoxelCoordinate& minCoord,
                ImgMergeMode mode,
                ZImg& res,
                const std::vector<ZImgTile>& tiles)
{
  switch (mode) {
    case ImgMergeMode::Min:
      mergeMin_Impl<TVoxel>(region, minCoord, res, tiles);
      break;
    case ImgMergeMode::Max:
      mergeMax_Impl<TVoxel>(region, minCoord, res, tiles);
      break;
    case ImgMergeMode::Mean:
      mergeMean_Impl<TVoxel>(region, minCoord, res, tiles);
      break;
    case ImgMergeMode::Median:
      mergeMedian_Impl<TVoxel>(region, minCoord, res, tiles);
      break;
    case ImgMergeMode::First:
    default:
      break;
  }
}

} // namespace

namespace nim {

void ZImgMerge::addImg(const ZImgSubBlock& img, const ZVoxelCoordinate& loc, const QString& imgName)
{
  m_imgCoords[&img] = loc;
  m_imgNames[&img] = imgName;
  if (!m_imgInfos.contains(&img)) {
    m_imgInfos[&img] = img.readInfo();
  }
}

void ZImgMerge::addImgPair(const ZImgSubBlock& img1,
                           const ZImgSubBlock& img2,
                           const ZVoxelCoordinate& img2Offset,
                           double connectionCost,
                           const QString& img1Name,
                           const QString& img2Name)
{
  if (m_imgPairs.contains(std::make_pair(&img2, &img1))) {
    m_imgPairs[std::make_pair(&img2, &img1)] = std::make_pair(-img2Offset, connectionCost);
  } else {
    m_imgPairs[std::make_pair(&img1, &img2)] = std::make_pair(img2Offset, connectionCost);
  }
  m_imgNames[&img1] = img1Name;
  m_imgNames[&img2] = img2Name;
  if (!m_imgInfos.contains(&img1)) {
    m_imgInfos[&img1] = img1.readInfo();
  }
  if (!m_imgInfos.contains(&img2)) {
    m_imgInfos[&img2] = img2.readInfo();
  }
}

void ZImgMerge::removeImg(const ZImgSubBlock& img)
{
  m_imgCoords.erase(&img);
  m_imgNames.erase(&img);
  m_imgInfos.erase(&img);
  auto it = m_imgPairs.begin();
  while (it != m_imgPairs.end()) {
    if (it->first.first == &img || it->first.second == &img) {
      it = m_imgPairs.erase(it);
    } else {
      ++it;
    }
  }
}

void ZImgMerge::removeImgPair(const ZImgSubBlock& img1, const ZImgSubBlock& img2)
{
  auto it = m_imgPairs.begin();
  while (it != m_imgPairs.end()) {
    if (it->first == std::make_pair(&img1, &img2) || it->first == std::make_pair(&img2, &img1)) {
      it = m_imgPairs.erase(it);
    } else {
      ++it;
    }
  }
}

QStringList ZImgMerge::resolveLocations()
{
  QStringList summ;
  m_imgFinalCoords.clear();
  m_imgInfo.clear();
  m_tiles.clear();
  m_overlapRegion = ZVoxelRegion();
  m_minCoord = ZVoxelCoordinate();

  if (m_imgCoords.empty() && m_imgPairs.empty()) {
    throw ZException("Merge Imgs error: no Input Img");
  }

  const ZImgSubBlock* refImg = nullptr;
  for (const auto& imgCoord : m_imgCoords) {
    if (!refImg) {
      refImg = imgCoord.first;
    } else if (!m_imgInfos.at(imgCoord.first).isSameType(m_imgInfos.at(refImg))) {
      throw ZException("Merge Imgs error: imgs are not same type");
    }
  }
  double minCost = std::numeric_limits<double>::max();
  for (const auto& imgimgOffsetCost : m_imgPairs) {
    minCost = std::min(minCost, imgimgOffsetCost.second.second);
    if (!refImg) {
      refImg = imgimgOffsetCost.first.first;
    } else if (!m_imgInfos.at(imgimgOffsetCost.first.first).isSameType(m_imgInfos.at(refImg))) {
      throw ZException("Merge Imgs error: imgs are not same type");
    }
  }

  CHECK(refImg);
  resolveLocations_impl(m_imgFinalCoords, *refImg, minCost, summ);

  // sort m_tiles based on image name (natural order) to make sure the "First" merge mode follows image name order
  std::map<QString, const ZImgSubBlock*, QStringNaturalCompare> orderedTiles;
  for (const auto& imgName : m_imgNames) {
    orderedTiles[imgName.second] = imgName.first;
  }
  // only do it when every image name is unique
  if (orderedTiles.size() == m_imgFinalCoords.size()) {
    // should be in reverse order as we write tiles from begin to end and we want lower image overwrites higher image
    for (const auto& nameBlock : makeReverse(orderedTiles)) {
      // VLOG(1) << nameBlock.first;
      m_tiles.emplace_back(*nameBlock.second, m_imgFinalCoords[nameBlock.second]);
    }
  } else {
    for (const auto& imgCoord : m_imgFinalCoords) {
      m_tiles.emplace_back(*imgCoord.first, imgCoord.second);
    }
  }

  ZVoxelRegion allRegion;

  for (size_t i = 0; i < m_tiles.size(); ++i) {
    allRegion.addBox(m_tiles[i].location(), m_tiles[i].maxCoord());

    ZVoxelRegion r1;
    r1.addBox(m_tiles[i].location(), m_tiles[i].maxCoord());
    for (size_t j = i + 1; j < m_tiles.size(); ++j) {
      ZVoxelRegion r2;
      r2.addBox(m_tiles[j].location(), m_tiles[j].maxCoord());
      if (!r2.intersect(r1).isEmpty()) {
        m_overlapRegion.unite(r2);
      }
    }
  }

  ZVoxelCoordinate maxCoord;
  allRegion.getBoundBox(m_minCoord, maxCoord);
  ZVoxelCoordinate resSize = maxCoord - m_minCoord + 1;
  // create result with correct meta info (channel color, time stamp, voxel size...)
  m_imgInfo = m_tiles[0].imgInfo();
  m_imgInfo.width = resSize.x;
  m_imgInfo.height = resSize.y;
  m_imgInfo.depth = resSize.z;
  m_imgInfo.numChannels = resSize.c;
  m_imgInfo.numTimes = resSize.t;
  m_imgInfo.createDefaultDescriptions();

  // to get correct meta info from tile img
  std::set<size_t> neededChannelInfo;
  for (size_t i = 0; i < m_imgInfo.numChannels; ++i) {
    neededChannelInfo.insert(i);
  }
  std::set<size_t> neededTimeStamp;
  for (size_t i = 0; i < m_imgInfo.numTimes; ++i) {
    neededTimeStamp.insert(i);
  }

  for (const auto& m_tile : m_tiles) {
    ZVoxelCoordinate tileLoc = m_tile.location() - m_minCoord;
    summ.push_back(QString("tile %1 final offset %2").arg(m_imgNames.at(&(m_tile.imgBlock())), tileLoc.toQString()));

    if (!neededChannelInfo.empty()) { // some channnel info need to be filled
      size_t tileStartC = tileLoc.c;
      size_t tileEndC = tileStartC + m_tile.imgInfo().numChannels;
      for (auto it = neededChannelInfo.begin(); it != neededChannelInfo.end();) {
        if (*it >= tileStartC && *it < tileEndC) {
          m_imgInfo.channelColors[*it] = m_tile.imgInfo().channelColors[*it - tileStartC];
          m_imgInfo.channelNames[*it] = m_tile.imgInfo().channelNames[*it - tileStartC];
          it = neededChannelInfo.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (!neededTimeStamp.empty()) { // some time stamp info need to be filled
      size_t tileStartT = tileLoc.t;
      size_t tileEndT = tileStartT + m_tile.imgInfo().numTimes;
      for (auto it = neededTimeStamp.begin(); it != neededTimeStamp.end();) {
        if (*it >= tileStartT && *it < tileEndT) {
          m_imgInfo.timeStamps[*it] = m_tile.imgInfo().timeStamps[*it - tileStartT];
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

  LOG(INFO) << "merge summary:";
  for (const auto& mes : summ) {
    LOG(INFO) << mes;
  }
  return summ;
}

// ZImg ZImgMerge::slice(size_t z, size_t t, size_t ratio) const
//{
//   ZImg res;
//   CHECK(ratio == 1);
//
//   ZImgRegion rgn(0, -1, 0, -1, z, z+1, 0, -1, t, t+1);
//   res.infoRef() = rgn.clip(m_imgInfo);
//   res.allocate();
//
//   ZVoxelCoordinate minCoord = m_minCoord;
//   minCoord.z += z;
//   minCoord.t += t;
//   for (size_t i = 0; i < m_tiles.size(); ++i) {
//     m_tiles[i].createImgCache();
//     ZVoxelCoordinate tileLoc = m_tiles[i].location() - minCoord;
//     if (m_mergeMode == Mode::Max) {
//       res.pasteImgMax(m_tiles[i].img(), tileLoc, false);
//     } else {
//       res.pasteImg(m_tiles[i].img(), tileLoc, false);
//     }
//
//     m_tiles[i].clearImgCache();
//   }
//
//   if (m_mergeMode != Mode::First && m_mergeMode != Mode::Max) {
//     // now merge overlap region
//     CHECK(false); // not working yet
//     IMG_TYPED_CALL(merge_Impl, res, m_overlapRegion, minCoord, m_mergeMode, res, m_tiles);
//   }
//
//   VLOG(1) << "Assembled slice " << z << "/" << m_imgInfo.depth << ".";
//
//   return res;
// }
//
// ZImg ZImgMerge::allSlices(size_t t, size_t ratio) const
//{
//   ZImg res;
//   CHECK(ratio == 1);
//
//   ZImgRegion rgn(0, -1, 0, -1, 0, -1, 0, -1, t, t+1);
//   res.infoRef() = rgn.clip(m_imgInfo);
//   res.allocate();
//
//   ZVoxelCoordinate minCoord = m_minCoord;
//   minCoord.t += t;
//   for (size_t i = 0; i < m_tiles.size(); ++i) {
//     m_tiles[i].createImgCache();
//     ZVoxelCoordinate tileLoc = m_tiles[i].location() - minCoord;
//     if (m_mergeMode == Mode::Max) {
//       res.pasteImgMax(m_tiles[i].img(), tileLoc, false);
//     } else {
//       res.pasteImg(m_tiles[i].img(), tileLoc, false);
//     }
//
//     m_tiles[i].clearImgCache();
//   }
//
//   if (m_mergeMode != Mode::First && m_mergeMode != Mode::Max) {
//     // now merge overlap region
//     IMG_TYPED_CALL(merge_Impl, res, m_overlapRegion, minCoord, m_mergeMode, res, m_tiles);
//   }
//
//   return res;
// }
//
// ZImg ZImgMerge::wholeImg(size_t ratio) const
//{
//   ZImg res;
//   CHECK(ratio == 1);
//
//   res.infoRef() = m_imgInfo;
//   res.allocate();
//
//   ZVoxelCoordinate minCoord = m_minCoord;
//   for (size_t i = 0; i < m_tiles.size(); ++i) {
//     m_tiles[i].createImgCache();
//     ZVoxelCoordinate tileLoc = m_tiles[i].location() - minCoord;
//     if (m_mergeMode == Mode::Max) {
//       res.pasteImgMax(m_tiles[i].img(), tileLoc);
//     } else {
//       res.pasteImg(m_tiles[i].img(), tileLoc);
//     }
//
//     m_tiles[i].clearImgCache();
//   }
//
//   if (m_mergeMode != Mode::First && m_mergeMode != Mode::Max) {
//     // now merge overlap region
//     IMG_TYPED_CALL(merge_Impl, res, m_overlapRegion, minCoord, m_mergeMode, res, m_tiles);
//   }
//
//   return res;
// }

size_t ZImgMerge::numBlocks() const
{
  return m_tiles.size();
}

ZImg ZImgMerge::block(size_t blockIdx) const
{
  m_tiles[blockIdx].createImgCache();
  ZImg res = m_tiles[blockIdx].img();
  m_tiles[blockIdx].clearImgCache();
  return res;
}

ZVoxelCoordinate ZImgMerge::blockCoord(size_t blockIdx) const
{
  return m_tiles[blockIdx].location() - m_minCoord;
}

ZImg ZImgMerge::wholeImg() const
{
  ZImg res(m_imgInfo);

  ZVoxelCoordinate minCoord = m_minCoord;
  for (const auto& m_tile : m_tiles) {
    m_tile.createImgCache();
    ZVoxelCoordinate tileLoc = m_tile.location() - minCoord;
    if (m_mergeMode == ImgMergeMode::Max) {
      res.pasteImgMax(m_tile.img(), tileLoc);
    } else {
      res.pasteImg(m_tile.img(), tileLoc);
    }

    m_tile.clearImgCache();
  }

  if (m_mergeMode != ImgMergeMode::First && m_mergeMode != ImgMergeMode::Max) {
    // now merge overlap region
    IMG_TYPED_CALL(merge_Impl, res.info(), m_overlapRegion, minCoord, m_mergeMode, res, m_tiles)
  }

  return res;
}

void ZImgMerge::save(const QString& fileName, FileFormat format, const ZImgWriteParameters& paras)
{
  if (imgInfo().byteNumber() * 3 > ZCpuInfo::instance().nPhysicalRAM &&
      (m_mergeMode == ImgMergeMode::Max || m_overlapRegion.isEmpty())) {
    ZImg::writeImg(fileName, *this, format, paras);
  } else {
    wholeImg().save(fileName, format, paras);
  }
}

void ZImgMerge::resolveLocations_impl(std::map<const ZImgSubBlock*, ZVoxelCoordinate>& imgCoords,
                                      const ZImgSubBlock& refImg,
                                      double minCost,
                                      QStringList& summary) const
{
  imgCoords = m_imgCoords;

  if (m_imgPairs.empty()) {
    return;
  }

  ZMinimumSpanningTree mst;
  std::vector<const ZImgSubBlock*> imgs;
  std::map<const ZImgSubBlock*, size_t> imgToVertexMapper;

  size_t refImgIndex = std::numeric_limits<size_t>::max();
  for (const auto& [img, coord] : imgCoords) {
    imgs.push_back(img);
    imgToVertexMapper[img] = imgs.size() - 1;
    if (&refImg == img) {
      refImgIndex = imgs.size() - 1;
    }
  }
  // use low cost to connect imgs with absolute location
  for (size_t i = 1; i < imgs.size(); ++i) {
    mst.addEdge(i - 1, i, minCost - 1e4);
  }

  for (const auto& [img1Img2, offsetCost] : m_imgPairs) {
    auto&& [img1, img2] = img1Img2;
    auto&& [img2Offset, cost] = offsetCost;
    auto img1Vertex = imgs.size();
    auto img1Iter = imgToVertexMapper.find(img1);
    if (img1Iter != imgToVertexMapper.end()) {
      img1Vertex = img1Iter->second;
    } else {
      imgs.push_back(img1);
      imgToVertexMapper[img1] = img1Vertex;
    }
    if (&refImg == img1) {
      refImgIndex = img1Vertex;
    }
    auto img2Vertex = imgs.size();
    auto img2Iter = imgToVertexMapper.find(img2);
    if (img2Iter != imgToVertexMapper.end()) {
      img2Vertex = img2Iter->second;
    } else {
      imgs.push_back(img2);
      imgToVertexMapper[img2] = img2Vertex;
    }
    if (&refImg == img2) {
      refImgIndex = img2Vertex;
    }
    mst.addEdge(img1Vertex, img2Vertex, cost);
  }
  CHECK(refImgIndex != std::numeric_limits<size_t>::max());

  if (imgCoords.empty()) {
    imgCoords[&refImg] = ZVoxelCoordinate(0, 0, 0, 0, 0);
  }

  for (const auto& [i1, i2] : mst.runMST(refImgIndex, false)) {
    auto img1 = imgs[i1];
    auto img2 = imgs[i2];
    bool img1HasLocation = imgCoords.contains(img1);
    bool img2HasLocation = imgCoords.contains(img2);

    if (img1HasLocation && !img2HasLocation) {
      auto pairIt = m_imgPairs.find(std::make_pair(img1, img2));
      if (pairIt != m_imgPairs.end()) {
        imgCoords[img2] = imgCoords[img1] + pairIt->second.first;
      } else {
        pairIt = m_imgPairs.find(std::make_pair(img2, img1)); // must exist
        imgCoords[img2] = imgCoords[img1] - pairIt->second.first;
      }
      summary.push_back(QString("tile %1 connects to tile %2 with cost %3, offset %4")
                          .arg(m_imgNames.at(pairIt->first.first))
                          .arg(m_imgNames.at(pairIt->first.second))
                          .arg(pairIt->second.second)
                          .arg(pairIt->second.first.toQString()));
    } else if (!img1HasLocation && img2HasLocation) {
      auto pairIt = m_imgPairs.find(std::make_pair(img1, img2));
      if (pairIt != m_imgPairs.end()) {
        imgCoords[img1] = imgCoords[img2] - pairIt->second.first;
      } else {
        pairIt = m_imgPairs.find(std::make_pair(img2, img1)); // must exist
        imgCoords[img1] = imgCoords[img2] + pairIt->second.first;
      }
      summary.push_back(QString("tile %1 connects to tile %2 with cost %3, offset %4")
                          .arg(m_imgNames.at(pairIt->first.first))
                          .arg(m_imgNames.at(pairIt->first.second))
                          .arg(pairIt->second.second)
                          .arg(pairIt->second.first.toQString()));
    } else {
      CHECK(img1HasLocation && img2HasLocation);
    }
  }
}

void ZImgMerge::mergeImgs(ZImg& res,
                          const std::map<const ZImgSubBlock*, ZVoxelCoordinate>& imgs,
                          ImgMergeMode mode,
                          QString& summary) const
{
  std::vector<ZImgTile> tiles;
  for (const auto& imgCoord : imgs) {
    tiles.emplace_back(*imgCoord.first, imgCoord.second);
  }

  ZVoxelRegion allRegion;
  ZVoxelRegion overlapRegion;

  for (size_t i = 0; i < tiles.size(); ++i) {
    allRegion.addBox(tiles[i].location(), tiles[i].maxCoord());

    ZVoxelRegion r1;
    r1.addBox(tiles[i].location(), tiles[i].maxCoord());
    for (size_t j = i + 1; j < tiles.size(); ++j) {
      ZVoxelRegion r2;
      r2.addBox(tiles[j].location(), tiles[j].maxCoord());
      if (!r2.intersect(r1).isEmpty()) {
        overlapRegion.unite(r2);
      }
    }
  }

  ZVoxelCoordinate minCoord;
  ZVoxelCoordinate maxCoord;
  allRegion.getBoundBox(minCoord, maxCoord);
  ZVoxelCoordinate resSize = maxCoord - minCoord + 1;
  // create result with correct meta info (channel color, time stamp, voxel size...)
  auto info = tiles[0].imgInfo();
  info.width = resSize.x;
  info.height = resSize.y;
  info.depth = resSize.z;
  info.numChannels = resSize.c;
  info.numTimes = resSize.t;
  info.createDefaultDescriptions();
  res = ZImg(info);

  // to get correct meta info from tile img
  std::set<size_t> neededChannelInfo;
  for (size_t i = 0; i < res.numChannels(); ++i) {
    neededChannelInfo.insert(i);
  }
  std::set<size_t> neededTimeStamp;
  for (size_t i = 0; i < res.numTimes(); ++i) {
    neededTimeStamp.insert(i);
  }

  for (const auto& tile : tiles) {
    tile.createImgCache();
    ZVoxelCoordinate tileLoc = tile.location() - minCoord;
    if (mode == ImgMergeMode::Max) {
      res.pasteImgMax(tile.img(), tileLoc);
    } else {
      res.pasteImg(tile.img(), tileLoc);
    }
    summary += QString("tile %1 final offset %2\n").arg(m_imgNames.at(&(tile.imgBlock())), tileLoc.toQString());

    if (!neededChannelInfo.empty()) { // some channel info need to be filled
      size_t tileStartC = tileLoc.c;
      size_t tileEndC = tileStartC + tile.img().numChannels();
      for (auto it = neededChannelInfo.begin(); it != neededChannelInfo.end();) {
        if (*it >= tileStartC && *it < tileEndC) {
          res.infoRef().channelColors[*it] = tile.img().info().channelColors[*it - tileStartC];
          res.infoRef().channelNames[*it] = tile.img().info().channelNames[*it - tileStartC];
          it = neededChannelInfo.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (!neededTimeStamp.empty()) { // some time stamp info need to be filled
      size_t tileStartT = tileLoc.t;
      size_t tileEndT = tileStartT + tile.img().numTimes();
      for (auto it = neededTimeStamp.begin(); it != neededTimeStamp.end();) {
        if (*it >= tileStartT && *it < tileEndT) {
          res.infoRef().timeStamps[*it] = tile.img().info().timeStamps[*it - tileStartT];
          it = neededTimeStamp.erase(it);
        } else {
          ++it;
        }
      }
    }
    tile.clearImgCache();
  }
  if (!neededChannelInfo.empty() || !neededTimeStamp.empty()) {
    LOG(FATAL) << "check";
  }

  if (mode != ImgMergeMode::First && mode != ImgMergeMode::Max) {
    // now merge overlap region
    IMG_TYPED_CALL(merge_Impl, res.info(), overlapRegion, minCoord, mode, res, tiles)
  }
}

} // namespace nim
