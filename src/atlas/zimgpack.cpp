#include "zimgpack.h"

#include "zcpuinfo.h"
#include "zimgio.h"
#include "z3dgpuinfo.h"
#include "zlog.h"
#include <QFileInfo>
#include <QPoint>
#include <QDir>
#include <tbb/parallel_for.h>
#include <boost/iterator/function_output_iterator.hpp>
#include <cmath>
#include <zbenchtimer.h>

namespace {

struct MaxOp
{
  template<typename TVoxel, typename TVoxelOther>
  TVoxel operator()(TVoxel voxelRef, TVoxelOther otherVoxel) const
  {
    return std::max(voxelRef, static_cast<TVoxel>(otherVoxel));
  }
};

}  // namespace

namespace nim {

ZImgPackSubBlock::ZImgPackSubBlock(std::shared_ptr<ZImg>& img,
                                   size_t ratio, size_t t, size_t z, index_t x, index_t y, size_t width, size_t height)
  : ZImgSubBlock(t, x, y, z, width, height, 1, ratio, ratio, 1)
  , m_img(img)
{
}

std::shared_ptr<ZImg> ZImgPackSubBlock::read() const
{
  return m_img;
}

ZImgInfo ZImgPackSubBlock::readInfo() const
{
  return m_img->info();
}

ZImgPack::ZImgPack(ZImgSource imgSource)
  : m_imgSource(std::move(imgSource))
  , m_hasUnsavedChange(false)
  , m_diskCached(true)
{
  std::vector<std::shared_ptr<ZImgSubBlock>> sceneSubBlock;
  ZImgIO::instance().readInfo(m_imgSource, m_imgInfo, &sceneSubBlock);
  ZImgIO::instance().readMetadata(m_imgSource, m_imgMetaData);

  m_minMaxState = MinMaxState::Invalid;

  bool hasPyramidal = false;
  for (const auto& b: sceneSubBlock) {
    if (b->xRatio > 1) {
      hasPyramidal = true;
      break;
    }
  }

  bool needScale = Z3DGpuInfo::instance().needScaleDataForTexture(m_imgInfo.width, m_imgInfo.height, m_imgInfo.depth);
  if (m_imgSource.totalFileSize <= m_fastReadSizeThreshold && !needScale) {
    m_diskCached = false;
    ZImgIO::instance().readImg(m_imgSource, m_img);
    m_img.computeMinMax(m_minIntensity, m_maxIntensity);
    m_minMaxState = MinMaxState::Complete;
  } else if (hasPyramidal || !needScale) {
    buildFastReadIndex(sceneSubBlock);
  } else {
    buildPyramidal();
  }

  updateDerivedData();
}

const QString& ZImgPack::sizeInfo() const
{
  if (m_sizeInfo.isEmpty()) {
    m_sizeInfo = QString("%1: (w:%2, h:%3, d:%4, c:%5").arg(m_imgInfo.typeAsQString()).arg(m_imgInfo.width)
      .arg(m_imgInfo.height).arg(m_imgInfo.depth).arg(m_imgInfo.numChannels);
    if (m_imgInfo.numTimes > 1) {
      m_sizeInfo += QString(", t:%1)").arg(m_imgInfo.numTimes);
    } else {
      m_sizeInfo += ")";
    }
  }
  return m_sizeInfo;
}

const QString& ZImgPack::detailedInfo() const
{
  if (m_detailedInfo.isEmpty()) {
    QStringList info;
    info << QString("Width: %1").arg(m_imgInfo.width);
    info << QString("Height: %1").arg(m_imgInfo.height);
    info << QString("Depth: %1").arg(m_imgInfo.depth);
    info << QString("Number of Channels: %1").arg(m_imgInfo.numChannels);
    info << QString("Number of Times: %1").arg(m_imgInfo.numTimes);
    info << QString("Bytes per Voxel: %1").arg(m_imgInfo.bytesPerVoxel);
    info << QString("Voxel Format: %1").arg(enumToQString(m_imgInfo.voxelFormat));
    info << QString("Voxel Size Unit: %1").arg(enumToQString(m_imgInfo.voxelSizeUnit));
    info << QString("Voxel Size X: %1").arg(m_imgInfo.voxelSizeX);
    info << QString("Voxel Size Y: %1").arg(m_imgInfo.voxelSizeY);
    info << QString("Voxel Size Z: %1").arg(m_imgInfo.voxelSizeZ);
    if (m_imgInfo.lastChannelIsAlphaChannel && m_imgInfo.numChannels > 0) {
      info << QString("Alpha Channel: %1").arg(m_imgInfo.numChannels - 1);
    }
    if (m_imgInfo.validBitCount > 0) {
      info << QString("Valid Bit Count: %1").arg(m_imgInfo.validBitCount);
    }
    m_detailedInfo = info.join("\n");
    m_detailedInfo += "\n\n";

    for (const auto& meta : m_imgMetaData.topLevelAttachments()) {
      m_detailedInfo += meta.toQString();
      m_detailedInfo += "\n";
    }
  }
  return m_detailedInfo;
}

void ZImgPack::setChannelColor(size_t c, col4 col)
{
  CHECK(c < m_imgInfo.numChannels);
  m_imgInfo.channelColors[c] = col;
  if (!m_diskCached) {
    m_img.infoRef().channelColors[c] = col;
  }
}

void ZImgPack::save(const QString& fileName, FileFormat format, const ZImgWriteParameters& paras)
{
  if (m_diskCached) {
    ZImgIO::instance().writeImg(fileName, *this, format, paras);
  } else {
    m_img.save(fileName, format, paras);
    m_diskCached = true;
  }
  m_imgSource = ZImgSource(fileName);
  m_hasUnsavedChange = false;

  for (size_t i = 0; i < m_allTiles.size(); ++i) {
    ZImgCache::instance().remove(ImageCacheHashKeyType(this, i));
  }
  std::vector<ZImgInfo> infos;
  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  ZImgIO::instance().readInfos(m_imgSource.filenames[0], infos, &subBlocks, m_imgSource.format);
  CHECK(!infos.empty() && !subBlocks.empty());
  m_imgInfo = infos[0];
  ZImgIO::instance().readMetadata(m_imgSource, m_imgMetaData);
  buildFastReadIndex(subBlocks[0]);

  updateDerivedData();
}

bool ZImgPack::needUpdate(const QRectF& viewport, double scale, const QRectF& oldViewport, double oldScale, size_t t,
                          size_t z, bool mip) const
{
  if (!m_diskCached)
    return false;

  auto readRatio = ratioForScale(scale, scale, 1.);
  auto oldReadRatio = ratioForScale(oldScale, oldScale, 1.);
  if (readRatio != oldReadRatio)
    return true;

  if (m_imgInfo.depth == 1)
    mip = false;

  if (mip) { // for now mip is always full-resolution
    return false;
  }

#if 1
  auto tiit = m_rtToTileBoxRTree.find(std::make_tuple(readRatio[0], readRatio[1], readRatio[2], t));
  if (tiit != m_rtToTileBoxRTree.end()) {
    TileBoxType queryBox1(TileCornerType(std::floor(viewport.x()), std::floor(viewport.y()), z),
                          TileCornerType(std::ceil(viewport.right()), std::ceil(viewport.bottom()), z));
    TileBoxType queryBox2(TileCornerType(std::floor(oldViewport.x()), std::floor(oldViewport.y()), z),
                          TileCornerType(std::ceil(oldViewport.right()), std::ceil(oldViewport.bottom()), z));
    std::set<size_t> queryResult1;
    std::set<size_t> queryResult2;
    tiit->second->query(bgi::intersects(queryBox1),
                        boost::make_function_output_iterator([&queryResult1](auto const& val) {
                          queryResult1.insert(val.second);
                        }));
    tiit->second->query(bgi::intersects(queryBox2),
                        boost::make_function_output_iterator([&queryResult2](auto const& val) {
                          queryResult2.insert(val.second);
                        }));
    return queryResult1 != queryResult2;
  }
#else
  auto tiit = m_rtzToTileIndice.find(std::make_tuple(readRatio, t, mip ? -1 : int(z)));
  if (tiit != m_rtzToTileIndice.end()) {
    const std::vector<size_t>& tileIndice = tiit->second;
    for (size_t i=0; i<tileIndice.size(); ++i) {
      const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
      QRectF tileRect(tile.x, tile.y, tile.width, tile.height);
      bool vpIntersect = tileRect.intersects(viewport);
      bool oldVpIntersect = tileRect.intersects(oldViewport);
      if (vpIntersect != oldVpIntersect)
        return true;
    }
  }
#endif

  return false;
}

void ZImgPack::retrieveCoveredImgs(std::vector<std::shared_ptr<ZImg>>& imgs, std::vector<QPoint>& locs,
                                   std::vector<double>& scales,
                                   size_t z, size_t t, const QRectF& viewport, double scale) const
{
  CHECK(m_diskCached);

  imgs.clear();
  locs.clear();
  scales.clear();

  auto readRatio = ratioForScale(scale, scale, 1);

#if 1
  bool finish = false;  // in case of multiple image with pyramidal levels (like czi) concated together to form
                        // one stack, some z slice might have less pyramidal levels so we can not trust readRatio in such case
  while (!finish) {
    auto tiit = m_rtToTileBoxRTree.find(std::make_tuple(readRatio[0], readRatio[1], readRatio[2], t));
    if (tiit != m_rtToTileBoxRTree.end()) {
      TileBoxType queryBox(TileCornerType(std::floor(viewport.x()), std::floor(viewport.y()), z),
                           TileCornerType(std::ceil(viewport.right()), std::ceil(viewport.bottom()), z));
      std::vector<RTreeValueType> queryResult;
      tiit->second->query(bgi::intersects(queryBox), std::back_inserter(queryResult));
      if (!queryResult.empty()) {
        imgs.resize(queryResult.size());
        locs.resize(queryResult.size());
        scales.resize(queryResult.size(), readRatio[0]);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, queryResult.size()), [&](const tbb::blocked_range<size_t>& r) {
          for (size_t i = r.begin(); i != r.end(); ++i) {
            const ZImgSubBlock& tile = *m_allTiles[queryResult[i].second].get();
            locs[i] = QPoint(tile.x, tile.y);
            imgs[i] = ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, queryResult[i].second), tile);
          }
        });
        finish = true;
      } else {
        // move to previous readRatio
        finish = true;
        for (auto rit = m_pyramidalRatios.rbegin(); rit != m_pyramidalRatios.rend(); ++rit) {
          if (*rit == readRatio) {
            ++rit;
            if (rit != m_pyramidalRatios.rend()) {
              readRatio = *rit;
              finish = false;
            }
            break;
          }
        }
      }
    } else {
      finish = true;
    }
  }
#else
  auto tiit = m_rtzToTileIndice.find(std::make_tuple(readRatio, t, mip ? -1 : int(z)));
  if (tiit != m_rtzToTileIndice.end()) {
    const std::vector<size_t>& tileIndice = tiit->second;
    for (size_t i=0; i<tileIndice.size(); ++i) {
      const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
      QRectF tileRect(tile.x, tile.y, tile.width, tile.height);
      if (tileRect.intersects(viewport)) {
        std::shared_ptr<ZImg> *imgPtr = ZImgCache::instance().getOrRead(boost::hash_value(HashKeyType(this, tileIndice[i])), tile);
        imgs.push_back(*imgPtr);
        locs.push_back(QPoint(tile.x, tile.y));
        scales.push_back(readRatio);
        //LOG(INFO) << level << " " << (1<<level) << " " << x << " " << y << " " << width << " " << height;
      }
    }
  }
#endif
}

void ZImgPack::retrieveCoveredMIPImgs(std::vector<std::shared_ptr<ZImg>>& imgs, std::vector<QPoint>& locs,
                                      std::vector<double>& scales,
                                      size_t zStart, size_t zEnd, size_t t, const QRectF&, double) const
{
  CHECK(m_diskCached);

  imgs.clear();
  locs.clear();
  scales.clear();

  if (m_mipImgs.empty()) {
    m_mipImgs.resize(m_imgInfo.numTimes);
  }
  if (m_mipImgs[t] && zStart == m_mipZStart && zEnd == m_mipZEnd) {
    imgs.push_back(m_mipImgs[t]);
    locs.emplace_back(0, 0);
    scales.push_back(1);
    return;
  }

  m_mipImgs[t] = std::make_shared<ZImg>(assembleImg(std::array<size_t, 3>{1, 1, 1}, t, zStart));
  for (size_t z = zStart + 1; z <= zEnd; ++z) {
    m_mipImgs[t]->binaryOperation(assembleImg(std::array<size_t, 3>{1, 1, 1}, t, z), MaxOp());
  }
  m_mipZStart = zStart;
  m_mipZEnd = zEnd;
  LOG(INFO) << "MIP: " << m_mipZStart << " " << m_mipZEnd;

  imgs.push_back(m_mipImgs[t]);
  locs.emplace_back(0, 0);
  scales.push_back(1);
}

double ZImgPack::value(size_t x, size_t y, size_t z, size_t c, size_t t, bool mip) const
{
  if (m_diskCached) {
    if (m_imgInfo.depth == 1)
      mip = false;
    if (mip) {
      CHECK(m_mipImgs[t] && !m_mipImgs[t]->isEmpty());
      return m_mipImgs[t]->value<double>(x, y, 0, c, 0);
    } else {
      auto tiit = m_rtToTileIndice.find(std::make_tuple(1_uz, 1_uz, 1_uz, t));
      if (tiit != m_rtToTileIndice.end()) {
        const std::vector<size_t>& tileIndice = tiit->second;
        for (auto tileIndex : tileIndice) {
          const ZImgSubBlock& tile = *m_allTiles[tileIndex].get();
          CHECK(tile.x >= 0 && tile.y >= 0 && tile.z >= 0);
          if (static_cast<index_t>(x) >= tile.x && static_cast<index_t>(x) < tile.x + index_t(tile.width) &&
              static_cast<index_t>(y) >= tile.y && static_cast<index_t>(y) < tile.y + index_t(tile.height) &&
              static_cast<index_t>(z) >= tile.z && static_cast<index_t>(z) < tile.z + index_t(tile.depth)) {
            std::shared_ptr<ZImg> imgPtr = ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndex), tile);
            return imgPtr->value<double>(x - tile.x, y - tile.y, z - tile.z, c, 0);
          }
        }
      }
    }
    return 0;
  }
  if (mip) {
    CHECK(!m_maximumProjectedAlongZImg.isEmpty());
    return m_maximumProjectedAlongZImg.value<double>(x, y, 0, c, t);
  }
  return m_img.value<double>(x, y, z, c, t);
}

double ZImgPack::displayValue(size_t x, size_t y, size_t z, size_t c, size_t t, bool mip) const
{
  if (m_diskCached) {
    if (m_imgInfo.depth == 1)
      mip = false;
    bool hasTile = false;
    index_t ix = x;
    index_t iy = y;
    index_t iz = z;

    if (mip) {
      hasTile = true;
    } else {
      for (const auto& ratio : m_pyramidalRatios) {
        auto tiit = m_rtToTileIndice.find(std::make_tuple(ratio[0], ratio[1], ratio[2], t));
        if (tiit != m_rtToTileIndice.end()) {
          const std::vector<size_t>& tileIndice = tiit->second;
          for (auto tileIndex : tileIndice) {
            const ZImgSubBlock& tile = *m_allTiles[tileIndex].get();
            if (ix >= tile.x && ix < tile.x + index_t(tile.width) &&
                iy >= tile.y && iy < tile.y + index_t(tile.height) &&
                iz >= tile.z && iz < tile.z + index_t(tile.depth)) {
              if (ratio[0] == 1 && ratio[1] == 1 && ratio[2] == 1)
                hasTile = true;
              std::shared_ptr<ZImg> imgPtr = ZImgCache::instance().get(ImageCacheHashKeyType(this, tileIndex));
              if (imgPtr) {
                return imgPtr->value<double>((ix - tile.x) / (ratio[0]),
                                             (iy - tile.y) / (ratio[1]),
                                             (iz - tile.z) / (ratio[2]),
                                             c, 0);
              }
            }
          }
        }
      }
    }

    return hasTile ? value(x, y, z, c, t, mip) : 0;
  }
  if (mip) {
    CHECK(!m_maximumProjectedAlongZImg.isEmpty());
    return m_maximumProjectedAlongZImg.value<double>(x, y, 0, c, t);
  }
  return m_img.value<double>(x, y, z, c, t);
}

ZImg ZImgPack::crop(const ZImgRegion& region) const
{
  ZImg res;

  if (!m_diskCached) {
    res = m_img.crop(region);
    return res;
  }

  if (region.isEmpty()) {
    return res;
  }

  ZImgRegion rgn = region;
  if (!rgn.isValid(m_imgInfo)) {
    throw ZImgException(QString("Try to crop img <%1> with invalid region <%2>")
                          .arg(m_imgInfo.toQString()).arg(rgn.toQString()));
  }

  rgn.resolveRegionEnd(m_imgInfo);
  ZImgInfo resInfo = rgn.clip(m_imgInfo);
  // create destination
  res = ZImg(resInfo);
  // start copy data
  using TCoordinate = ZVoxelCoordinate::value_type;

  for (TCoordinate t = rgn.tStart(); t < rgn.tEnd(); ++t) {
    auto tiit = m_rtToTileIndice.find(std::make_tuple(1, 1, 1, t));
    if (tiit != m_rtToTileIndice.end()) {
      const std::vector<size_t>& tileIndice = tiit->second;
      for (auto tileIndex : tileIndice) {
        const ZImgSubBlock& tile = *m_allTiles[tileIndex].get();
        ZVoxelCoordinate tileStart(tile.x, tile.y, tile.z, 0, t);
        ZVoxelCoordinate start = tileStart - rgn.start;
        if ((start.x < 0 && start.x + static_cast<TCoordinate>(tile.width) <= 0) ||
            start.x >= static_cast<TCoordinate>(res.width()) ||
            (start.y < 0 && start.y + static_cast<TCoordinate>(tile.height) <= 0) ||
            start.y >= static_cast<TCoordinate>(res.height()) ||
            (start.z < 0 && start.z + static_cast<TCoordinate>(tile.depth) <= 0) ||
            start.z >= static_cast<TCoordinate>(res.depth())) {
          continue;
        }

        std::shared_ptr<ZImg> imgPtr = ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndex), tile);
        res.pasteImg(*imgPtr, start);
      }
    }
  }

  return res;
}

ZImg ZImgPack::resizedImg(size_t width, size_t height, size_t depth, size_t t) const
{
  //LOG(INFO) << width << " " << height << " " << depth;
  CHECK(width <= m_imgInfo.width && height <= m_imgInfo.height && depth <= m_imgInfo.depth &&
        width > 0 && height > 0 && depth > 0);
  ZImg res;

  if (!m_diskCached) {
    res = m_img.createView(-1, t).resized(width, height, depth);
    return res;
  }

  auto ratio = readRatioOf(std::max(1.0, std::floor(1.0 * m_imgInfo.width / width)),
                           std::max(1.0, std::floor(1.0 * m_imgInfo.height / height)),
                           std::max(1.0, std::floor(1.0 * m_imgInfo.depth / depth)));

  res = assembleImg(ratio, t);
  if (res.width() != width || res.height() != height || res.depth() != depth) {
    res.resize(width, height, depth);
  }
  return res;
}

void ZImgPack::readRegionToImg(index_t xyRatio, index_t zRatio, index_t sx, index_t sy, index_t sz, size_t sc, size_t t,
                               const ZImgInfo& resInfo, ZImg& res) const
{
  CHECK(xyRatio >= 1 && zRatio >= 1);
  //ZBenchTimer bt_read(fmt::format("reading and assembling image block"));
  //bt_read.start();
  auto readRatio = readRatioOf(xyRatio, xyRatio, zRatio);
  auto tmpResInfo = resInfo;
  tmpResInfo.width = std::ceil(resInfo.width * xyRatio * 1.0 / readRatio[0]);
  tmpResInfo.height = std::ceil(resInfo.height * xyRatio * 1.0 / readRatio[1]);
  tmpResInfo.depth = std::ceil(resInfo.depth * zRatio * 1.0 / readRatio[2]);
  res = ZImg(tmpResInfo);
  auto tiit = m_rtToTileBoxRTree.find(std::make_tuple(readRatio[0], readRatio[1], readRatio[2], t));
  if (tiit != m_rtToTileBoxRTree.end()) {
    std::vector<RTreeValueType> queryResult;
    TileBoxType queryBox(TileCornerType(sx * xyRatio,
                                        sy * xyRatio,
                                        sz * zRatio),
                         TileCornerType((sx + static_cast<index_t>(resInfo.width)) * xyRatio - 1,
                                        (sy + static_cast<index_t>(resInfo.height)) * xyRatio - 1,
                                        (sz + static_cast<index_t>(resInfo.depth)) * zRatio - 1));
    tiit->second->query(bgi::intersects(queryBox), std::back_inserter(queryResult));
    for (auto& i: queryResult) {
      const ZImgSubBlock& tile = *m_allTiles[i.second].get();
      ZVoxelCoordinate start(std::round((tile.x * 1.0 / xyRatio - sx) * xyRatio / readRatio[0]),
                             std::round((tile.y * 1.0 / xyRatio - sy) * xyRatio / readRatio[1]),
                             std::round((tile.z * 1.0 / zRatio - sz) * zRatio / readRatio[2]),
                             -ZVoxelCoordinate::value_type(sc),
                             0);
      auto imgPtr = ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, i.second), tile,
                                                    ZImgCache::FindStategy::NoUpdateLRUList);
      if (imgPtr->isSameType(res)) {
        if (m_imgInfo.validBitCount != 0 && m_imgInfo.validBitCount != 8 && m_imgInfo.validBitCount != 16) {
          ZImg tmp = imgPtr->normalized(m_minIntensity, m_maxIntensity);
          res.pasteImg(tmp, start);
        } else {
          res.pasteImg(*imgPtr, start);
        }
      } else {
        ZImg tmp = imgPtr->convertTo(m_minIntensity, m_maxIntensity, res);
        res.pasteImg(tmp, start);
      }
    }
  }
  //bt_read.pause();
  if (res.width() != resInfo.width || res.height() != resInfo.height || res.depth() != resInfo.depth) {
    res.resize(resInfo.width, resInfo.height, resInfo.depth);
  }
  //bt_read.resume();
  //STOP_AND_LOG(bt_read)
}

folly::Future<ZImg> ZImgPack::readRegionToImg(index_t xyRatio, index_t zRatio, index_t sx, index_t sy, index_t sz,
                                              size_t sc, size_t t, const ZImgInfo& resInfo) const
{
  CHECK(xyRatio >= 1 && zRatio >= 1);
  auto readRatio = readRatioOf(xyRatio, xyRatio, zRatio);
  auto tmpResInfo = resInfo;
  tmpResInfo.width = std::ceil(resInfo.width * xyRatio * 1.0 / readRatio[0]);
  tmpResInfo.height = std::ceil(resInfo.height * xyRatio * 1.0 / readRatio[1]);
  tmpResInfo.depth = std::ceil(resInfo.depth * zRatio * 1.0 / readRatio[2]);
  auto cpuExecutor = folly::getGlobalCPUExecutor();
  auto ioExecutor = folly::getGlobalIOExecutor();
  return folly::via(cpuExecutor, [=]() {
    auto tiit = m_rtToTileBoxRTree.find(std::make_tuple(readRatio[0], readRatio[1], readRatio[2], t));
    std::vector<folly::Future<std::tuple<ZVoxelCoordinate, std::shared_ptr<ZImg>>>> tileFutures;
    if (tiit != m_rtToTileBoxRTree.end()) {
      std::vector<RTreeValueType> queryResult;
      TileBoxType queryBox(TileCornerType(sx * xyRatio,
                                          sy * xyRatio,
                                          sz * zRatio),
                           TileCornerType((sx + static_cast<index_t>(resInfo.width)) * xyRatio - 1,
                                          (sy + static_cast<index_t>(resInfo.height)) * xyRatio - 1,
                                          (sz + static_cast<index_t>(resInfo.depth)) * zRatio - 1));
      tiit->second->query(bgi::intersects(queryBox), std::back_inserter(queryResult));

      for (auto& i: queryResult) {
        const ZImgSubBlock* tile = m_allTiles[i.second].get();
        tileFutures.push_back(folly::via(cpuExecutor, [=]() {
          return std::make_tuple(ZVoxelCoordinate(std::round((tile->x * 1.0 / xyRatio - sx) * xyRatio / readRatio[0]),
                                                  std::round((tile->y * 1.0 / xyRatio - sy) * xyRatio / readRatio[1]),
                                                  std::round((tile->z * 1.0 / zRatio - sz) * zRatio / readRatio[2]),
                                                  -ZVoxelCoordinate::value_type(sc),
                                                  0),
                                 ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, i.second), *tile));
        }));
      }
    }
    return folly::collect(tileFutures);
  }).then([=](const folly::Try<std::vector<std::tuple<ZVoxelCoordinate, std::shared_ptr<ZImg>>>>& tiles) {
    ZImg res(tmpResInfo);
    if (tiles.hasValue()) {
      for (const auto& [start, imgPtr]: tiles.value()) {
        if (imgPtr->isSameType(res)) {
          if (m_imgInfo.validBitCount != 0 && m_imgInfo.validBitCount != 8 && m_imgInfo.validBitCount != 16) {
            ZImg tmp = imgPtr->normalized(m_minIntensity, m_maxIntensity);
            res.pasteImg(tmp, start);
          } else {
            res.pasteImg(*imgPtr, start);
          }
        } else {
          ZImg tmp = imgPtr->convertTo(m_minIntensity, m_maxIntensity, res);
          res.pasteImg(tmp, start);
        }
      }
      if (res.width() != resInfo.width || res.height() != resInfo.height || res.depth() != resInfo.depth) {
        res.resize(resInfo.width, resInfo.height, resInfo.depth);
      }
    }
    return res;
  });
}

std::set<ImageCacheHashKeyType> ZImgPack::collectCacheKeysForReadRegionToImg(index_t xyRatio,
                                                                             index_t zRatio,
                                                                             index_t sx,
                                                                             index_t sy,
                                                                             index_t sz,
                                                                             index_t width,
                                                                             index_t height,
                                                                             index_t depth,
                                                                             size_t t,
                                                                             bool onlyCollectNotInCacheKeys) const
{
  CHECK(xyRatio >= 1 && zRatio >= 1);
  auto readRatio = readRatioOf(xyRatio, xyRatio, zRatio);

  std::set<ImageCacheHashKeyType> res;

  TileBoxType queryBox(TileCornerType(sx * xyRatio,
                                      sy * xyRatio,
                                      sz * zRatio),
                       TileCornerType((sx + width) * xyRatio - 1,
                                      (sy + height) * xyRatio - 1,
                                      (sz + depth) * zRatio - 1));
  auto tiit = m_rtToTileBoxRTree.find(std::make_tuple(readRatio[0], readRatio[1], readRatio[2], t));
  if (tiit != m_rtToTileBoxRTree.end()) {
    std::vector<RTreeValueType> queryResult;
    tiit->second->query(bgi::intersects(queryBox), std::back_inserter(queryResult));
    if (onlyCollectNotInCacheKeys) {
      for (auto& i : queryResult) {
        ImageCacheHashKeyType key(this, i.second);
        if (!ZImgCache::instance().contains(key)) {
          res.insert(key);
        }
      }
    } else {
      for (auto& i : queryResult) {
        res.insert(ImageCacheHashKeyType(this, i.second));
      }
    }
  }
  return res;
}

void ZImgPack::preloadImageCache(const ImageCacheHashKeyType& key) const
{
#ifdef USE_KeyWithMemoizedHash
  auto index = key.index();
#else
  auto index = std::get<1>(key);
#endif
  ZImgCache::instance().insert(ImageCacheHashKeyType(this, index), m_allTiles[index]->read());
}

void ZImgPack::prefetchImageCache(const ImageCacheHashKeyType& key) const
{
#ifdef USE_KeyWithMemoizedHash
  auto index = key.index();
#else
  auto index = std::get<1>(key);
#endif
  m_allTiles[index]->prefetch();
}

const ZImg& ZImgPack::maxZProjectedImg(size_t zStart, size_t zEnd) const
{
  CHECK(!m_diskCached);
  if (m_maximumProjectedAlongZImg.isEmpty() || zStart != m_mipZStart || zEnd != m_mipZEnd) {
    m_img.maximumZProjection(zStart, zEnd).swap(m_maximumProjectedAlongZImg);
    m_mipZStart = zStart;
    m_mipZEnd = zEnd;
  }
  return m_maximumProjectedAlongZImg;
}

ZImg ZImgPack::slice(size_t z, size_t t) const
{
  CHECK(m_diskCached);
  return assembleImg({1, 1, 1}, t, z);
}

ZImg ZImgPack::allSlices(size_t t) const
{
  CHECK(m_diskCached);
  return assembleImg({1, 1, 1}, t);
}

ZImg ZImgPack::wholeImg() const
{
  CHECK(m_diskCached);
  return assembleImg({1, 1, 1});
}

void ZImgPack::createSliceTiles(ZImg* img, size_t z, size_t t)
{
  size_t ratio = 1;
  while (true) {
    size_t numX = (img->width() + m_tileSize - 1) / m_tileSize;
    size_t numY = (img->height() + m_tileSize - 1) / m_tileSize;
    if (numX == 1 && numY == 1) {
      size_t width = m_imgInfo.width;
      size_t height = m_imgInfo.height;
      if (img->width() <= 64 && img->height() <= 64) {
        std::shared_ptr<ZImg> simg(img);
        m_allTiles.emplace_back(new ZImgPackSubBlock(simg, ratio, t, z, 0, 0, width, height));

        ZImgCache::instance().insert(ImageCacheHashKeyType(this, m_allTiles.size() - 1), std::move(simg));
        break;
      } else {
        std::shared_ptr<ZImg> simg(new ZImg(*img));
        m_allTiles.emplace_back(new ZImgPackSubBlock(simg, ratio, t, z, 0, 0, width, height));

        ZImgCache::instance().insert(ImageCacheHashKeyType(this, m_allTiles.size() - 1), std::move(simg));

        img->zoom(0.5, 0.5);
        ratio *= 2;
      }
    } else {
      for (size_t x = 0; x < numX; ++x) {
        for (size_t y = 0; y < numY; ++y) {
          size_t startX = x * m_tileSize;
          size_t endX = std::min(img->width(), startX + m_tileSize);
          size_t startY = y * m_tileSize;
          size_t endY = std::min(img->height(), startY + m_tileSize);
          size_t width = std::min(m_imgInfo.width, (endX - startX) * ratio);
          size_t height = std::min(m_imgInfo.height, (endY - startY) * ratio);
          std::shared_ptr<ZImg> cropped(new ZImg());
          img->crop(ZImgRegion(startX, endX, startY, endY, 0, 1, 0, -1, 0, 1)).swap(*cropped);
          startX = startX * ratio;
          startY = startY * ratio;

          m_allTiles.emplace_back(new ZImgPackSubBlock(cropped, ratio, t, z, startX, startY, width, height));

          ZImgCache::instance().insert(ImageCacheHashKeyType(this, m_allTiles.size() - 1), std::move(cropped));
        }
      }

      img->zoom(0.5, 0.5);
      ratio *= 2;
    }
  }
}

void ZImgPack::buildPyramidal(ZImg& img)
{
  img.computeMinMax(m_minIntensity, m_maxIntensity);
  m_minMaxState = MinMaxState::Complete;

  if (m_imgInfo.depth == 1) {
    for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
      if (m_imgInfo.numTimes == 1) {
        auto tImg = new ZImg();
        tImg->swap(img);
        createSliceTiles(tImg, 0, 0);
      } else {
        ZImgRegion rgn;
        rgn.start.t = t;
        rgn.end.t = t + 1;
        auto tImg = new ZImg();
        img.crop(rgn).swap(*tImg);
        createSliceTiles(tImg, 0, t);
      }
    }
  } else {
    for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
      for (size_t z = 0; z < m_imgInfo.depth; ++z) {
        ZImgRegion rgn;
        rgn.start.z = z;
        rgn.end.z = z + 1;
        rgn.start.t = t;
        rgn.end.t = t + 1;
        auto tImg = new ZImg();
        img.crop(rgn).swap(*tImg);

        createSliceTiles(tImg, z, t);
      }
    }
  }

  createTileIndexStructure();
}

void ZImgPack::buildPyramidal()
{
  if (m_imgInfo.byteNumber() <= ZCpuInfo::instance().nPhysicalRAM / 2) {
    ZImg tImg;
    tImg.load(m_imgSource);
    buildPyramidal(tImg);
    return;
  }

  double minV;
  double maxV;
  m_minIntensity = std::numeric_limits<double>::max();
  m_maxIntensity = std::numeric_limits<double>::lowest();

  if (m_imgInfo.depth == 1) {
    for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
      ZImgRegion rgn;
      rgn.start.t = t;
      rgn.end.t = t + 1;
      auto tImg = new ZImg();
      tImg->load(m_imgSource.filenames, m_imgSource.catDim, m_imgSource.catScenes, rgn, m_imgSource.scene,
                 1, 1, 1, m_imgSource.format);
      tImg->computeMinMax(minV, maxV);
      m_minIntensity = std::min(m_minIntensity, minV);
      m_maxIntensity = std::max(m_maxIntensity, maxV);
      createSliceTiles(tImg, 0, t);
    }
  } else {
    for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
      for (size_t z = 0; z < m_imgInfo.depth; ++z) {
        ZImgRegion rgn;
        rgn.start.z = z;
        rgn.end.z = z + 1;
        rgn.start.t = t;
        rgn.end.t = t + 1;
        auto tImg = new ZImg();
        tImg->load(m_imgSource.filenames, m_imgSource.catDim, m_imgSource.catScenes, rgn, m_imgSource.scene,
                   1, 1, 1, m_imgSource.format);
        tImg->computeMinMax(minV, maxV);
        m_minIntensity = std::min(m_minIntensity, minV);
        m_maxIntensity = std::max(m_maxIntensity, maxV);

        createSliceTiles(tImg, z, t);
      }
    }
  }

  m_minMaxState = MinMaxState::Complete;

  createTileIndexStructure();
}

void ZImgPack::buildFastReadIndex(const std::vector<std::shared_ptr<ZImgSubBlock>>& subBlocks)
{
  // LOG(INFO) << "here";
  m_allTiles = subBlocks;

  createTileIndexStructure();

  //ZBenchTimer bt;
  //bt.start();

  // get estimation of minmax
  auto ratio = *m_pyramidalRatios.rbegin();

  double minV;
  double maxV;
  m_minIntensity = std::numeric_limits<double>::max();
  m_maxIntensity = std::numeric_limits<double>::lowest();

  for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
    auto tiit = m_rtToTileIndice.find(std::make_tuple(ratio[0], ratio[1], ratio[2], t));
    if (tiit != m_rtToTileIndice.end()) {
      const std::vector<size_t>& tileIndice = tiit->second;
      for (auto idx : tileIndice) {
        const ZImgSubBlock& tile = *m_allTiles[idx].get();
        std::shared_ptr<ZImg> imgPtr =
          ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, idx), tile);
        imgPtr->computeMinMax(minV, maxV);
        m_minIntensity = std::min(m_minIntensity, minV);
        m_maxIntensity = std::max(m_maxIntensity, maxV);
      }
    }
  }

  m_minMaxState = ratio == std::array<size_t, 3>{1, 1, 1} ? MinMaxState::Complete : MinMaxState::Partial;

  //bt.stop();
  //LOG(INFO) << bt;
}

void ZImgPack::createTileIndexStructure()
{
  m_pyramidalRatios.clear();
  m_rtToTileIndice.clear();
  m_rtToTileBoxRTree.clear();

  for (size_t i = 0; i < m_allTiles.size(); ++i) {
    const ZImgSubBlock& tile = *m_allTiles[i].get();
    m_pyramidalRatios.insert(std::array<size_t, 3>{tile.xRatio, tile.yRatio, tile.zRatio});
    m_rtToTileIndice[std::make_tuple(tile.xRatio, tile.yRatio, tile.zRatio, tile.t)].push_back(i);
  }
  for (const auto& rtTileIndice : m_rtToTileIndice) {
    std::vector<RTreeValueType> values(rtTileIndice.second.size());
    for (size_t i = 0; i < rtTileIndice.second.size(); ++i) {
      const ZImgSubBlock& tile = *m_allTiles[rtTileIndice.second[i]].get();
      values[i] = std::make_pair(TileBoxType(TileCornerType(tile.x, tile.y, tile.z),
                                             TileCornerType(tile.x + tile.width - 1,
                                                            tile.y + tile.height - 1,
                                                            tile.z + tile.depth - 1)),
                                 rtTileIndice.second[i]);
    }
    m_rtToTileBoxRTree.emplace_hint(m_rtToTileBoxRTree.end(), rtTileIndice.first,
                                    std::make_unique<RTreeType>(values));
  }
}

ZImg ZImgPack::assembleImg(std::array<size_t, 3> ratio) const
{
  CHECK(m_pyramidalRatios.find(ratio) != m_pyramidalRatios.end());
  ZImgInfo info = m_imgInfo;
  info.width = (m_imgInfo.width + ratio[0] - 1) / ratio[0];
  info.height = (m_imgInfo.height + ratio[1] - 1) / ratio[1];
  info.depth = (m_imgInfo.depth + ratio[2] - 1) / ratio[2];
  ZImg res(info);

  for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
    auto tiit = m_rtToTileIndice.find(std::make_tuple(ratio[0], ratio[1], ratio[2], t));
    if (tiit != m_rtToTileIndice.end()) {
      const std::vector<size_t>& tileIndice = tiit->second;
      tbb::parallel_for(tbb::blocked_range<size_t>(0, tileIndice.size()),
                        [&](const tbb::blocked_range<size_t>& r) {
                          for (size_t i = r.begin(); i != r.end(); ++i) {
                            const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
                            ZVoxelCoordinate start(std::round(tile.x * 1.0 / ratio[0]),
                                                   std::round(tile.y * 1.0 / ratio[1]),
                                                   std::round(tile.z * 1.0 / ratio[2]), 0, t);

                            std::shared_ptr<ZImg> imgPtr =
                              ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndice[i]), tile);
                            res.pasteImg(*imgPtr, start);
                          }
                        }
      );
    }
  }

  //LOG(INFO) << "end assemble level " << level;
  return res;
}

ZImg ZImgPack::assembleImg(std::array<size_t, 3> ratio, size_t t) const
{
  CHECK(m_pyramidalRatios.find(ratio) != m_pyramidalRatios.end());
  ZImgInfo info = m_imgInfo;
  info.width = (m_imgInfo.width + ratio[0] - 1) / ratio[0];
  info.height = (m_imgInfo.height + ratio[1] - 1) / ratio[1];
  info.depth = (m_imgInfo.depth + ratio[2] - 1) / ratio[2];
  info.numTimes = 1;
  ZImg res(info);

  auto tiit = m_rtToTileIndice.find(std::make_tuple(ratio[0], ratio[1], ratio[2], t));
  if (tiit != m_rtToTileIndice.end()) {
    const std::vector<size_t>& tileIndice = tiit->second;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, tileIndice.size()),
                      [&](const tbb::blocked_range<size_t>& r) {
                        for (size_t i = r.begin(); i != r.end(); ++i) {
                          const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
                          ZVoxelCoordinate start(std::round(tile.x * 1.0 / ratio[0]),
                                                 std::round(tile.y * 1.0 / ratio[1]),
                                                 std::round(tile.z * 1.0 / ratio[2]), 0, 0);

                          std::shared_ptr<ZImg> imgPtr =
                            ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndice[i]), tile);
                          res.pasteImg(*imgPtr, start);
                        }
                      }
    );
  }

  //LOG(INFO) << "end assemble level " << level;
  return res;
}

ZImg ZImgPack::assembleImg(std::array<size_t, 3> ratio, size_t t, size_t z) const
{
  CHECK(m_pyramidalRatios.find(ratio) != m_pyramidalRatios.end() && ratio[2] == 1);
  ZImgInfo info = m_imgInfo;
  info.width = (m_imgInfo.width + ratio[0] - 1) / ratio[0];
  info.height = (m_imgInfo.height + ratio[1] - 1) / ratio[1];
  info.depth = 1;
  info.numTimes = 1;
  ZImg res(info);

  auto tiit = m_rtToTileIndice.find(std::make_tuple(ratio[0], ratio[1], ratio[2], t));
  if (tiit != m_rtToTileIndice.end()) {
    const std::vector<size_t>& tileIndice = tiit->second;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, tileIndice.size()),
                      [&](const tbb::blocked_range<size_t>& r) {
                        for (size_t i = r.begin(); i != r.end(); ++i) {
                          const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
                          if (index_t(z) >= tile.z && index_t(z) < (tile.z + index_t(tile.depth))) {
                            ZVoxelCoordinate start(std::round(tile.x * 1.0 / ratio[0]),
                                                   std::round(tile.y * 1.0 / ratio[1]),
                                                   tile.z - index_t(z), 0, 0);

                            std::shared_ptr<ZImg> imgPtr =
                              ZImgCache::instance().getOrRead(ImageCacheHashKeyType(this, tileIndice[i]), tile);
                            res.pasteImg(*imgPtr, start);
                          }
                        }
                      }
    );
  }
  return res;
}

void ZImgPack::updateDerivedData()
{
  //LOG(INFO) << m_imgInfo.toQString();
  if (!m_diskCached) {
    m_maximumProjectedAlongZImg.clear();
    if (m_imgInfo.depth == 1) {
      m_maximumProjectedAlongZImg = m_img.createView();
    }
  } else {
    m_img.clear();
    m_maximumProjectedAlongZImg.clear();
  }

  CHECK(m_minMaxState != MinMaxState::Invalid);
  if (m_imgInfo.validBitCount != 12 && m_imgInfo.voxelByteNumber() == 2 &&
      m_imgInfo.voxelFormat == VoxelFormat::Unsigned && m_maxIntensity < 4096) {
    m_imgInfo.validBitCount = 12;
    if (!m_diskCached)
      m_img.infoRef().validBitCount = 12;
  }
  if (m_imgInfo.validBitCount != 1 && m_imgInfo.voxelByteNumber() == 1 &&
      m_imgInfo.voxelFormat == VoxelFormat::Unsigned && m_maxIntensity < 2) {
    m_imgInfo.validBitCount = 1;
    if (!m_diskCached)
      m_img.infoRef().validBitCount = 1;
  }
  m_rangeMin = m_imgInfo.dataRangeMin<double>();
  m_rangeMax = m_imgInfo.dataRangeMax<double>();
  if (m_imgInfo.voxelFormat == VoxelFormat::Float && (m_maxIntensity > 1.0 || m_minIntensity < 0.0)) {
    m_rangeMin = m_minIntensity;
    m_rangeMax = m_maxIntensity;
  }

  m_sizeInfo.clear();
  m_detailedInfo.clear();

  updateNameTootip();
}

void ZImgPack::updateNameTootip()
{
  if (isSequence()) {
    m_name =
      QFileInfo(m_imgSource.filenames[0]).fileName() + QString(" %1 Sequence").arg(enumToQString(m_imgSource.catDim));
    m_tooltip = m_imgSource.filenames.join("\n");
  } else {
    m_name = QFileInfo(m_imgSource.filenames[0]).fileName() +
               QString(" scene %1").arg(m_imgSource.scene + 1);
    m_tooltip = m_imgSource.filenames[0] + QString(" scene %1").arg(m_imgSource.scene + 1);
  }
}

std::array<size_t, 3> ZImgPack::ratioForScale(double xScale, double yScale, double zScale) const
{
  CHECK(!m_pyramidalRatios.empty());
  return readRatioOf(std::max(1.0, std::floor(1.0 / xScale)),
                     std::max(1.0, std::floor(1.0 / yScale)),
                     std::max(1.0, std::floor(1.0 / zScale)));
}

std::array<size_t, 3> ZImgPack::readRatioOf(size_t xRatio, size_t yRatio, size_t zRatio) const
{
  std::array<size_t, 3> readRatio = {1, 1, 1};
  size_t lastRatioSum = 3;
  for (const auto& ratio : m_pyramidalRatios) {
    if (ratio[0] <= xRatio && ratio[1] <= yRatio && ratio[2] <= zRatio) {
      size_t ratioSum = ratio[0] + ratio[1] + ratio[2];
      if (ratioSum > lastRatioSum) {
        lastRatioSum = ratioSum;
        readRatio = ratio;
      }
    }
  }
  return readRatio;
}

} // namespace nim
