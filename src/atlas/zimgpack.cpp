#include "zimgpack.h"

#include <QFileInfo>
#include <cmath>
#include <QPoint>
#include <QDir>
#include "zimgcache.h"
#include "zsysteminfo.h"
#include "zcpuinfo.h"
#include "zimgio.h"
#include <boost/functional/hash.hpp>
#include <boost/function_output_iterator.hpp>
#include "z3dgpuinfo.h"
#include "zlog.h"

namespace {

struct MaxOp
{
  template<typename TVoxel, typename TVoxelOther>
  TVoxel operator()(TVoxel voxelRef, TVoxelOther otherVoxel) const
  {
    return std::max(voxelRef, static_cast<TVoxel>(otherVoxel));
  }
};

}

namespace nim {

ZImgPackSubBlock::ZImgPackSubBlock(const QString& fn,
                                   size_t ratio, size_t t, size_t z, int64_t x, int64_t y, size_t width, size_t height)
  : ZImgSubBlock(ratio, t, z, x, y, width, height)
  , m_type(Type::CacheFile)
  , m_imgSource(fn)
{
}

ZImgPackSubBlock::ZImgPackSubBlock(const ZImgSource& imgSource,
                                   size_t ratio, size_t t, size_t z, int64_t x, int64_t y, size_t width, size_t height)
  : ZImgSubBlock(ratio, t, z, x, y, width, height)
  , m_type(Type::OrigSource)
  , m_imgSource(imgSource)
{
  m_imgSource.region.start.z = z;
  m_imgSource.region.end.z = z + 1;
  m_imgSource.region.start.t = t;
  m_imgSource.region.end.t = t + 1;
}

ZImgPackSubBlock::ZImgPackSubBlock(const ZImgSource& imgSource,
                                   size_t ratio, size_t t, size_t sliceStart, size_t sliceEnd, int64_t x, int64_t y,
                                   size_t width, size_t height)
  : ZImgSubBlock(ratio, t, -1, x, y, width, height)
  , m_type(Type::OrigSourceMIP)
  , m_imgSource(imgSource)
  , m_zStart(sliceStart)
  , m_zEnd(sliceEnd)
{
  m_imgSource.region.start.t = t;
  m_imgSource.region.end.t = t + 1;
}

ZImgPackSubBlock::ZImgPackSubBlock(std::shared_ptr<ZImg>& img,
                                   size_t ratio, size_t t, size_t z, int64_t x, int64_t y, size_t width, size_t height)
  : ZImgSubBlock(ratio, t, z, x, y, width, height)
  , m_type(Type::OrigSource)
  , m_img(img)
{
}

ZImgPackSubBlock::ZImgPackSubBlock(std::shared_ptr<ZImg>& img,
                                   size_t ratio, size_t t, size_t sliceStart, size_t sliceEnd, int64_t x, int64_t y,
                                   size_t width, size_t height)
  : ZImgSubBlock(ratio, t, -1, x, y, width, height)
  , m_type(Type::OrigSourceMIP)
  , m_zStart(sliceStart)
  , m_zEnd(sliceEnd)
  , m_img(img)
{

}

std::shared_ptr<ZImg> ZImgPackSubBlock::read() const
{
  if (m_img) {
    return m_img;
  }

  std::shared_ptr<ZImg> res(new ZImg());
  switch (m_type) {
    case Type::CacheFile:
      res->load(m_imgSource);
      break;
    case Type::OrigSource:
      res->load(m_imgSource);
      break;
    case Type::OrigSourceMIP:
      for (size_t slc = m_zStart; slc < m_zEnd; ++slc) {
        ZImgSource imgSource = m_imgSource;
        imgSource.region.start.z = slc;
        imgSource.region.end.z = slc + 1;
        ZImg tImg;
        tImg.load(imgSource);    //todo: reading result not cached
        if (slc == m_zStart) {
          res->swap(tImg);
        } else {
          res->binaryOperation(tImg, MaxOp());
        }
      }
      break;
    default:
      CHECK(false);
      break;
  }
  return res;
}

ZImgPack::ZImgPack(ZImg& img, const QString& fileName)
  : m_imgInfo(img.info())
  , m_imgSource(fileName)
  , m_numScenes(1), m_hasUnsavedChange(false)
  , m_offsetX(0), m_offsetY(0), m_offsetZ(0), m_offsetT(0)
  , m_diskCached(true)
{
  if (m_imgInfo.isEmpty()) {
    throw ZIOException("Can not create ImgPack from empty img");
  }

  m_rangeMin = m_imgInfo.dataRangeMin<double>();
  m_rangeMax = m_imgInfo.dataRangeMax<double>();
  m_minIntensity = m_rangeMin;
  m_maxIntensity = m_rangeMax;
  m_minMaxState = MinMaxState::Partial;

  //createPyramidalFolder(m_imgSource.filenames[0]);
  buildPyramidal(img);

  updateDerivedData();
}

ZImgPack::ZImgPack(const QString& fileName, size_t scene, FileFormat format, size_t numScene,
                   const ZImgInfo* info, const std::vector<std::shared_ptr<ZImgSubBlock>>* subBlock)
  : m_imgSource(fileName, ZImgRegion(), scene, format)
  , m_hasUnsavedChange(false)
  , m_offsetX(0), m_offsetY(0), m_offsetZ(0), m_offsetT(0)
  , m_diskCached(true)
{
  const std::vector<std::shared_ptr<ZImgSubBlock>>* sceneSubBlock = nullptr;
  std::vector<std::shared_ptr<ZImgSubBlock>> ssb;
  if (numScene > 0 && info && subBlock) {
    m_imgInfo = *info;
    m_numScenes = numScene;
    if (scene >= m_numScenes) {
      throw ZIOException("invalid scene");
    }
    sceneSubBlock = subBlock;
  } else {
    std::vector<ZImgInfo> infos;
    std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
    ZImgIOInstance.readInfo(m_imgSource.filenames[0], infos, &subBlocks, nullptr, m_imgSource.format);
    if (scene >= infos.size()) {
      throw ZIOException("invalid scene");
    }
    m_imgInfo = infos[scene];
    m_numScenes = infos.size();
    ssb.swap(subBlocks[scene]);
    sceneSubBlock = &ssb;
  }

  m_rangeMin = m_imgInfo.dataRangeMin<double>();
  m_rangeMax = m_imgInfo.dataRangeMax<double>();
  m_minIntensity = m_rangeMin;
  m_maxIntensity = m_rangeMax;
  m_minMaxState = MinMaxState::Partial;

  bool hasPyramidal = false;
  for (size_t i = 0; i < sceneSubBlock->size(); ++i) {
    if (sceneSubBlock->at(i)->ratio > 1) {
      hasPyramidal = true;
      break;
    }
  }

  bool needScale = Z3DGpuInfoInstance.needScaleDataForTexture(m_imgInfo.width, m_imgInfo.height, m_imgInfo.depth);
  if (m_imgSource.totalFileSize <= m_fastReadSizeThreshold && !needScale) {
    m_diskCached = false;
    ZImgIOInstance.readImg(m_imgSource, m_img);
  } else if (hasPyramidal || !needScale) {
    buildFastReadIndex(*sceneSubBlock);
  } else {
    buildPyramidal();
  }

  updateDerivedData();
}

ZImgPack::ZImgPack(const QStringList& files, Dimension catDim, size_t scene, FileFormat format, size_t numScene,
                   const ZImgInfo* info, const std::vector<std::shared_ptr<ZImgSubBlock>>* subBlock)
  : m_imgSource(files, catDim, ZImgRegion(), scene, format, true)
  , m_hasUnsavedChange(false)
  , m_offsetX(0), m_offsetY(0), m_offsetZ(0), m_offsetT(0)
  , m_diskCached(true)
{
  const std::vector<std::shared_ptr<ZImgSubBlock>>* sceneSubBlock = nullptr;
  std::vector<std::shared_ptr<ZImgSubBlock>> ssb;
  if (numScene > 0 && info && subBlock) {
    m_imgInfo = *info;
    m_numScenes = numScene;
    if (scene >= m_numScenes) {
      throw ZIOException("invalid scene");
    }
    sceneSubBlock = subBlock;
  } else {
    std::vector<ZImgInfo> infos;
    std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
    ZImgIOInstance.readInfo(m_imgSource.filenames, m_imgSource.catDim, infos, &subBlocks, m_imgSource.format);
    if (scene >= infos.size()) {
      throw ZIOException("invalid scene");
    }
    m_imgInfo = infos[scene];
    m_numScenes = infos.size();
    ssb.swap(subBlocks[scene]);
    sceneSubBlock = &ssb;
  }

  m_rangeMin = m_imgInfo.dataRangeMin<double>();
  m_rangeMax = m_imgInfo.dataRangeMax<double>();
  m_minIntensity = m_rangeMin;
  m_maxIntensity = m_rangeMax;
  m_minMaxState = MinMaxState::Partial;

  bool hasPyramidal = false;
  for (size_t i = 0; i < sceneSubBlock->size(); ++i) {
    if (sceneSubBlock->at(i)->ratio > 1) {
      hasPyramidal = true;
      break;
    }
  }

  bool needScale = Z3DGpuInfoInstance.needScaleDataForTexture(m_imgInfo.width, m_imgInfo.height, m_imgInfo.depth);
  if (m_imgSource.totalFileSize <= m_fastReadSizeThreshold && !needScale) {
    m_diskCached = false;
    ZImgIOInstance.readImg(m_imgSource, m_img);
  } else if (hasPyramidal || !needScale) {
    buildFastReadIndex(*sceneSubBlock);
  } else {
    buildPyramidal();
  }

  updateDerivedData();
}

ZImgPack::~ZImgPack()
{
  for (size_t i = 0; i < m_allTiles.size(); ++i) {
    ZImgCacheInstance.remove(boost::hash_value(HashKeyType(this, i)));
  }
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
    info << QString("Voxel Format: %1").arg(enumToString(m_imgInfo.voxelFormat));
    info << QString("Voxel Size Unit: %1").arg(enumToString(m_imgInfo.voxelSizeUnit));
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

void ZImgPack::save(QString fileName, FileFormat format, Compression comp)
{
  if (m_diskCached) {
    ZImgIOInstance.writeImg(fileName, *this, format, comp);
  } else {
    m_img.save(fileName, format, comp);
    m_diskCached = true;
  }
  m_imgSource = ZImgSource(fileName);
  m_numScenes = 1;
  m_hasUnsavedChange = false;

  for (size_t i = 0; i < m_allTiles.size(); ++i) {
    ZImgCacheInstance.remove(boost::hash_value(HashKeyType(this, i)));
  }
  std::vector<ZImgInfo> infos;
  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  ZImgIOInstance.readInfo(m_imgSource.filenames[0], infos, &subBlocks, nullptr, m_imgSource.format);
  CHECK(!infos.empty() && !subBlocks.empty());
  m_imgInfo = infos[0];
  m_numScenes = infos.size();
  buildFastReadIndex(subBlocks[0]);

  updateDerivedData();
}

bool ZImgPack::needUpdate(const QRectF& viewport, double scale, const QRectF& oldViewport, double oldScale, size_t t,
                          size_t z, bool mip) const
{
  if (!m_diskCached)
    return false;

  size_t readRatio = ratioForScale(scale);
  size_t oldReadRatio = ratioForScale(oldScale);
  if (readRatio != oldReadRatio)
    return true;

  if (m_imgInfo.depth == 1)
    mip = false;

#if 1
  auto tiit = m_rtzToTileBoxRTree.find(std::make_tuple(readRatio, t, mip ? -1 : int(z)));
  if (tiit != m_rtzToTileBoxRTree.end()) {
    TileBoxType queryBox1(TileCornerType(std::floor(viewport.x()), std::floor(viewport.y())),
                          TileCornerType(std::ceil(viewport.right()), std::ceil(viewport.bottom())));
    TileBoxType queryBox2(TileCornerType(std::floor(oldViewport.x()), std::floor(oldViewport.y())),
                          TileCornerType(std::ceil(oldViewport.right()), std::ceil(oldViewport.bottom())));
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
                                   size_t z, size_t t, const QRectF& viewport, double scale, bool mip) const
{
  CHECK(m_diskCached);

  imgs.clear();
  locs.clear();
  scales.clear();

  size_t readRatio = ratioForScale(scale);

  if (m_imgInfo.depth == 1)
    mip = false;

#if 1
  auto tiit = m_rtzToTileBoxRTree.find(std::make_tuple(readRatio, t, mip ? -1 : int(z)));
  if (tiit != m_rtzToTileBoxRTree.end()) {
    TileBoxType queryBox(TileCornerType(std::floor(viewport.x()), std::floor(viewport.y())),
                         TileCornerType(std::ceil(viewport.right()), std::ceil(viewport.bottom())));
    std::vector<RTreeValueType> queryResult;
    tiit->second->query(bgi::intersects(queryBox), std::back_inserter(queryResult));
    for (size_t i = 0; i < queryResult.size(); ++i) {
      const ZImgSubBlock& tile = *m_allTiles[queryResult[i].second].get();
      std::shared_ptr<ZImg>* imgPtr = ZImgCacheInstance.getOrRead(
        boost::hash_value(HashKeyType(this, queryResult[i].second)), tile);
      imgs.push_back(*imgPtr);
      locs.push_back(QPoint(tile.x, tile.y));
      scales.push_back(readRatio);
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
        std::shared_ptr<ZImg> *imgPtr = ZImgCacheInstance.getOrRead(boost::hash_value(HashKeyType(this, tileIndice[i])), tile);
        imgs.push_back(*imgPtr);
        locs.push_back(QPoint(tile.x, tile.y));
        scales.push_back(readRatio);
        //LOG(INFO) << level << " " << (1<<level) << " " << x << " " << y << " " << width << " " << height;
      }
    }
  }
#endif
}

double ZImgPack::value(size_t x, size_t y, size_t z, size_t c, size_t t, bool mip) const
{
  if (m_diskCached) {
    if (m_imgInfo.depth == 1)
      mip = false;
    auto tiit = m_rtzToTileIndice.find(std::tuple<size_t, size_t, int>(1, t, mip ? -1 : static_cast<int>(z)));
    if (tiit != m_rtzToTileIndice.end()) {
      const std::vector<size_t>& tileIndice = tiit->second;
      for (size_t i = 0; i < tileIndice.size(); ++i) {
        const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
        CHECK(tile.x >= 0 && tile.y >= 0);
        if (static_cast<int64_t>(x) >= tile.x && static_cast<int64_t>(x) < tile.x + tile.width &&
            static_cast<int64_t>(y) >= tile.y && static_cast<int64_t>(y) < tile.y + tile.height) {
          std::shared_ptr<ZImg>* imgPtr = ZImgCacheInstance.getOrRead(
            boost::hash_value(HashKeyType(this, tileIndice[i])), tile);
          return (*imgPtr)->value<double>(x - tile.x, y - tile.y, 0, c, 0);
        }
      }
    }
    return 0;
  } else {
    if (mip) {
      CHECK(!m_maximumProjectedAlongZImg.isEmpty());
      return m_maximumProjectedAlongZImg.value<double>(x, y, 0, c, t);
    } else {
      return m_img.value<double>(x, y, z, c, t);
    }
  }
}

double ZImgPack::displayValue(size_t x, size_t y, size_t z, size_t c, size_t t, bool mip) const
{
  if (m_diskCached) {
    if (m_imgInfo.depth == 1)
      mip = false;
    bool hasTile = false;
    int64_t ix = x;
    int64_t iy = y;

    for (auto it = m_ratioToSize.begin(); it != m_ratioToSize.end(); ++it) {
      size_t ratio = it->first;
      auto tiit = m_rtzToTileIndice.find(std::make_tuple(ratio, t, mip ? -1 : int(z)));
      if (tiit != m_rtzToTileIndice.end()) {
        const std::vector<size_t>& tileIndice = tiit->second;
        for (size_t i = 0; i < tileIndice.size(); ++i) {
          const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
          if (ix >= tile.x && ix < tile.x + tile.width && iy >= tile.y && iy < tile.y + tile.height) {
            if (ratio == 1)
              hasTile = true;
            std::shared_ptr<ZImg>* imgPtr = ZImgCacheInstance.object(
              boost::hash_value(HashKeyType(this, tileIndice[i])));
            if (imgPtr) {
              return (*imgPtr)->value<double>((ix - tile.x) / (1.0 * ratio), (iy - tile.y) / (1.0 * ratio), 0, c, 0);
            }
          }
        }
      }
    }

    return hasTile ? value(x, y, z, c, t, mip) : 0;
  } else {
    if (mip) {
      CHECK(!m_maximumProjectedAlongZImg.isEmpty());
      return m_maximumProjectedAlongZImg.value<double>(x, y, 0, c, t);
    } else {
      return m_img.value<double>(x, y, z, c, t);
    }
  }
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
  typedef ZVoxelCoordinate::value_type TCoordinate;

  for (TCoordinate t = rgn.tStart(); t < rgn.tEnd(); ++t) {
    for (TCoordinate z = rgn.zStart(); z < rgn.zEnd(); ++z) {
      auto tiit = m_rtzToTileIndice.find(std::tuple<size_t, size_t, int>(1, t, z));
      if (tiit != m_rtzToTileIndice.end()) {
        const std::vector<size_t>& tileIndice = tiit->second;
        for (size_t i = 0; i < tileIndice.size(); ++i) {
          const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
          ZVoxelCoordinate tileStart(tile.x, tile.y, z, 0, t);
          ZVoxelCoordinate start = tileStart - rgn.start;
          if ((start.x < 0 && start.x + static_cast<TCoordinate>(tile.width) <= 0) ||
              start.x >= static_cast<TCoordinate>(res.width()) ||
              (start.y < 0 && start.y + static_cast<TCoordinate>(tile.height) <= 0) ||
              start.y >= static_cast<TCoordinate>(res.height())) {
            continue;
          }

          std::shared_ptr<ZImg>* imgPtr = ZImgCacheInstance.getOrRead(
            boost::hash_value(HashKeyType(this, tileIndice[i])), tile);
          res.pasteImg(*imgPtr->get(), start);
        }
      }
    }
  }

  return res;
}

ZImg ZImgPack::resizedImg(size_t width, size_t height, size_t depth, size_t t) const
{
  LOG(INFO) << width << " " << height << " " << depth;
  CHECK(width <= m_imgInfo.width && height <= m_imgInfo.height && depth <= m_imgInfo.depth &&
        width > 0 && height > 0 && depth > 0);
  ZImg res;

  if (!m_diskCached) {
    res = m_img.createView(-1, t).resized(width, height, depth);
    return res;
  }

  size_t ratio = 1;
  for (auto it = m_ratioToSize.begin(); it != m_ratioToSize.end(); ++it) {
    if (it->second.width() < int(width) || it->second.height() < int(height)) {
      break;
    }
    ratio = it->first;
  }

  if (m_imgInfo.width == width && m_imgInfo.height == height && m_imgInfo.depth == depth) {
    res = assembleImg(ratio, t);
  } else {
    ZImgInfo info = m_imgInfo;
    info.width = width;
    info.height = height;
    if (m_imgInfo.depth == 1) {  // depth == 1
      res = assembleImg(ratio, t, 0);
      res.resize(width, height, 1);
    } else {
      res = ZImg(info);
      for (size_t z = 0; z < info.depth; ++z) {
        ZImg tmpImg = assembleImg(ratio, t, z);
        tmpImg.resize(width, height, 1);
        res.pasteImg(tmpImg, ZVoxelCoordinate(0, 0, z, 0, 0));
      }
      res.resize(width, height, depth);
    }
  }
  LOG(INFO) << "end";
  return res;
}

void ZImgPack::readRegionToImg(size_t xyRatio, size_t zRatio, int64_t sx, int64_t sy, int64_t sz, size_t sc, size_t t,
                               ZImg& res) const
{
  size_t readRatio = readRatioOf(xyRatio);
  if (readRatio == xyRatio) {
    TileBoxType queryBox(TileCornerType(sx * static_cast<int64_t>(xyRatio),
                                        sy * static_cast<int64_t>(xyRatio)),
                         TileCornerType((sx + static_cast<int64_t>(res.width())) * static_cast<int64_t>(xyRatio) - 1,
                                        (sy + static_cast<int64_t>(res.height())) * static_cast<int64_t>(xyRatio) - 1));
    int64_t zEnd = std::min(static_cast<int64_t>(m_imgInfo.depth),
                            (sz + static_cast<int64_t>(res.depth())) * static_cast<int64_t>(zRatio));
    size_t zIdx = 0;
    for (int64_t z = sz * static_cast<int64_t>(zRatio); z < zEnd; z += static_cast<int64_t>(zRatio), ++zIdx) {
      if (z < 0)
        continue;

      auto tiit = m_rtzToTileBoxRTree.find(std::make_tuple(xyRatio, t, int(z)));
      if (tiit != m_rtzToTileBoxRTree.end()) {
        std::vector<RTreeValueType> queryResult;
        tiit->second->query(bgi::intersects(queryBox), std::back_inserter(queryResult));
        for (size_t i = 0; i < queryResult.size(); ++i) {
          const ZImgSubBlock& tile = *m_allTiles[queryResult[i].second].get();
          ZVoxelCoordinate start(tile.x / static_cast<int64_t>(xyRatio) - sx,
                                 tile.y / static_cast<int64_t>(xyRatio) - sy,
                                 zIdx,
                                 -ZVoxelCoordinate::value_type(sc),
                                 0);
          std::shared_ptr<ZImg>* imgPtr = ZImgCacheInstance.getOrRead(
            boost::hash_value(HashKeyType(this, queryResult[i].second)), tile);
          if ((*imgPtr)->isSameType(res)) {
            if (m_imgInfo.validBitCount != 0 && m_imgInfo.validBitCount != 8 && m_imgInfo.validBitCount != 16) {
              ZImg tmp = (*imgPtr)->normalized(m_minIntensity, m_maxIntensity);
              res.pasteImg(tmp, start);
            } else {
              res.pasteImg(*imgPtr->get(), start);
            }
          } else {
            ZImg tmp = (*imgPtr)->convertTo(m_minIntensity, m_maxIntensity, res);
            res.pasteImg(tmp, start);
          }
        }
      }
    }
  } else {
    ZImgInfo info = res.info();
    info.width = std::round(info.width * xyRatio * 1.0 / readRatio);
    info.height = std::round(info.height * xyRatio * 1.0 / readRatio);
    ZImg tmp(info);
    readRegionToImg(readRatio, zRatio, std::round(sx * xyRatio * 1.0 / readRatio),
                    std::round(sy * xyRatio * 1.0 / readRatio), sz, sc, t, tmp);
    res = tmp.resized(res.width(), res.height(), res.depth());
  }
}

const ZImg& ZImgPack::maxZProjectedImg() const
{
  CHECK(!m_diskCached);
  if (m_maximumProjectedAlongZImg.isEmpty()) {
    m_img.maximumZProjection().swap(m_maximumProjectedAlongZImg);
  }
  return m_maximumProjectedAlongZImg;
}

ZImg& ZImgPack::maxZProjectedImg()
{
  CHECK(!m_diskCached);
  if (m_maximumProjectedAlongZImg.isEmpty()) {
    m_img.maximumZProjection().swap(m_maximumProjectedAlongZImg);
  }
  return m_maximumProjectedAlongZImg;
}

ZImg ZImgPack::slice(size_t z, size_t t) const
{
  CHECK(m_diskCached);
  return assembleImg(1, t, z);
}

ZImg ZImgPack::allSlices(size_t t) const
{
  CHECK(m_diskCached);
  return assembleImg(1, t);
}

void ZImgPack::createSliceTiles(ZImg* img, size_t z, size_t t, bool mip)
{
  if (false || (img->width() <= m_tileSize && img->height() <= m_tileSize)) {
    if (mip) {
      m_allTiles.emplace_back(
        new ZImgPackSubBlock(m_imgSource, 1, t, 0, m_imgInfo.depth, 0, 0, img->width(), img->height()));
    } else {
      m_allTiles.emplace_back(new ZImgPackSubBlock(m_imgSource, 1, t, z, 0, 0, img->width(), img->height()));
    }
    ZImgCacheInstance.insert(boost::hash_value(HashKeyType(this, m_allTiles.size() - 1)),
                             new std::shared_ptr<ZImg>(img), std::max<size_t>(1, img->byteNumber() / 1024 / 1024));
    return;
  }

  size_t ratio = 1;
  while (true) {
    size_t numX = (img->width() + m_tileSize - 1) / m_tileSize;
    size_t numY = (img->height() + m_tileSize - 1) / m_tileSize;
    if (numX == 1 && numY == 1) {
      size_t width = m_imgInfo.width;
      size_t height = m_imgInfo.height;
      if (img->width() <= 32 && img->height() <= 32) {
        std::shared_ptr<ZImg> simg(img);
        if (mip) {
          m_allTiles.emplace_back(new ZImgPackSubBlock(simg, ratio, t, 0, m_imgInfo.depth, 0, 0, width, height));
        } else {
          m_allTiles.emplace_back(new ZImgPackSubBlock(simg, ratio, t, z, 0, 0, width, height));
        }
        ZImgCacheInstance.insert(boost::hash_value(HashKeyType(this, m_allTiles.size() - 1)),
                                 new std::shared_ptr<ZImg>(simg),
                                 std::max<size_t>(1, simg->byteNumber() / 1024 / 1024));
        break;
      } else {
        std::shared_ptr<ZImg> simg(new ZImg(*img));
        if (mip) {
          m_allTiles.emplace_back(new ZImgPackSubBlock(simg, ratio, t, 0, m_imgInfo.depth, 0, 0, width, height));
        } else {
          m_allTiles.emplace_back(new ZImgPackSubBlock(simg, ratio, t, z, 0, 0, width, height));
        }
        ZImgCacheInstance.insert(boost::hash_value(HashKeyType(this, m_allTiles.size() - 1)),
                                 new std::shared_ptr<ZImg>(simg),
                                 std::max<size_t>(1, simg->byteNumber() / 1024 / 1024));

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
          img->crop(ZImgRegion(startX, endX, startY, endY, 0, 1,
                               0, -1, 0, 1)).swap(*cropped);
          startX = startX * ratio;
          startY = startY * ratio;

          if (mip) {
            m_allTiles.emplace_back(
              new ZImgPackSubBlock(cropped, ratio, t, 0, m_imgInfo.depth, startX, startY, width, height));
          } else {
            m_allTiles.emplace_back(new ZImgPackSubBlock(cropped, ratio, t, z, startX, startY, width, height));
          }
          ZImgCacheInstance.insert(boost::hash_value(HashKeyType(this, m_allTiles.size() - 1)),
                                   new std::shared_ptr<ZImg>(cropped),
                                   std::max<size_t>(1, cropped->byteNumber() / 1024 / 1024));
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
  m_rangeMin = std::min(m_rangeMin, m_minIntensity);
  m_rangeMax = std::max(m_rangeMax, m_maxIntensity);
  m_minMaxState = MinMaxState::Complete;

  if (m_imgInfo.depth == 1) {
    for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
      if (m_imgInfo.numTimes == 1) {
        ZImg* tImg = new ZImg();
        tImg->swap(img);
        createSliceTiles(tImg, 0, 0);
      } else {
        ZImgRegion rgn;
        rgn.start.t = t;
        rgn.end.t = t + 1;
        ZImg* tImg = new ZImg();
        img.crop(rgn).swap(*tImg);
        createSliceTiles(tImg, 0, t);
      }
    }
  } else {
    for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
      ZImg* mipImg = new ZImg();
      for (size_t z = 0; z < m_imgInfo.depth; ++z) {
        ZImgRegion rgn;
        rgn.start.z = z;
        rgn.end.z = z + 1;
        rgn.start.t = t;
        rgn.end.t = t + 1;
        ZImg* tImg = new ZImg();
        img.crop(rgn).swap(*tImg);
        if (z == 0) {
          *mipImg = *tImg;
        } else {
          mipImg->binaryOperation(*tImg, MaxOp());
        }
        createSliceTiles(tImg, z, t);
      }
      createSliceTiles(mipImg, 0, t, true);
    }
  }

  createTileIndexStructure();
}

void ZImgPack::buildPyramidal()
{
  if (m_imgInfo.byteNumber() <= ZCpuInfoInstance.nPhysicalRAM / 2) {
    ZImg tImg;
    tImg.load(m_imgSource.filenames, m_imgSource.catDim, ZImgRegion(), m_imgSource.scene, m_imgSource.format);
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
      ZImg* tImg = new ZImg();
      tImg->load(m_imgSource.filenames, m_imgSource.catDim, rgn, m_imgSource.scene, m_imgSource.format);
      tImg->computeMinMax(minV, maxV);
      m_minIntensity = std::min(m_minIntensity, minV);
      m_maxIntensity = std::max(m_maxIntensity, maxV);
      createSliceTiles(tImg, 0, t);
    }
  } else {
    for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
      ZImg* mipImg = new ZImg();
      for (size_t z = 0; z < m_imgInfo.depth; ++z) {
        ZImgRegion rgn;
        rgn.start.z = z;
        rgn.end.z = z + 1;
        rgn.start.t = t;
        rgn.end.t = t + 1;
        ZImg* tImg = new ZImg();
        tImg->load(m_imgSource.filenames, m_imgSource.catDim, rgn, m_imgSource.scene, m_imgSource.format);
        tImg->computeMinMax(minV, maxV);
        m_minIntensity = std::min(m_minIntensity, minV);
        m_maxIntensity = std::max(m_maxIntensity, maxV);
        if (z == 0) {
          *mipImg = *tImg;
        } else {
          mipImg->binaryOperation(*tImg, MaxOp());
        }
        createSliceTiles(tImg, z, t);
      }
      createSliceTiles(mipImg, 0, t, true);
    }
  }

  m_rangeMin = std::min(m_rangeMin, m_minIntensity);
  m_rangeMax = std::max(m_rangeMax, m_maxIntensity);
  m_minMaxState = MinMaxState::Complete;

  createTileIndexStructure();
}

void ZImgPack::buildFastReadIndex(const std::vector<std::shared_ptr<ZImgSubBlock>>& subBlocks)
{
  m_allTiles = subBlocks;

  if (m_imgInfo.depth > 1) {
    for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
      m_allTiles.emplace_back(
        new ZImgPackSubBlock(m_imgSource, 1, t, 0, m_imgInfo.depth, 0, 0, m_imgInfo.width, m_imgInfo.height));
    }
  }

  createTileIndexStructure();
}

void ZImgPack::createTileIndexStructure()
{
  m_ratioToSize.clear();
  m_rtzToTileIndice.clear();
  m_rtzToTileBoxRTree.clear();

  for (size_t i = 0; i < m_allTiles.size(); ++i) {
    const ZImgSubBlock& tile = *m_allTiles[i].get();
    if (m_ratioToSize.find(tile.ratio) == m_ratioToSize.end()) {
      m_ratioToSize[tile.ratio] = QSize(std::ceil(double(m_imgInfo.width) / tile.ratio),
                                        std::ceil(double(m_imgInfo.height) / tile.ratio));
    }
    m_rtzToTileIndice[std::tie(tile.ratio, tile.t, tile.z)].push_back(i);
  }
  for (auto it = m_rtzToTileIndice.begin(); it != m_rtzToTileIndice.end(); ++it) {
    std::vector<RTreeValueType> values(it->second.size());
    for (size_t i = 0; i < it->second.size(); ++i) {
      const ZImgSubBlock& tile = *m_allTiles[it->second[i]].get();
      values[i] = std::make_pair(TileBoxType(TileCornerType(tile.x, tile.y),
                                             TileCornerType(tile.x + tile.width - 1,
                                                            tile.y + tile.height - 1)),
                                 it->second[i]);
    }
    m_rtzToTileBoxRTree.emplace_hint(m_rtzToTileBoxRTree.end(), it->first, std::make_unique<RTreeType>(values));
  }
}

ZImg ZImgPack::assembleImg(size_t ratio) const
{
  CHECK(ratio >= 1);
  ZImgInfo info = m_imgInfo;
  if (ratio > 1) {
    info.width = m_ratioToSize.at(ratio).width();
    info.height = m_ratioToSize.at(ratio).height();
  }
  //LOG(INFO) << info.toQString();
  ZImg res(info);

  for (size_t t = 0; t < m_imgInfo.numTimes; ++t) {
    for (size_t z = 0; z < m_imgInfo.depth; ++z) {
      auto tiit = m_rtzToTileIndice.find(std::make_tuple(ratio, t, int(z)));
      if (tiit != m_rtzToTileIndice.end()) {
        const std::vector<size_t>& tileIndice = tiit->second;
        for (size_t i = 0; i < tileIndice.size(); ++i) {
          const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
          ZVoxelCoordinate start(tile.x / double(ratio), tile.y / double(ratio), z, 0, t);

          std::shared_ptr<ZImg>* imgPtr = ZImgCacheInstance.getOrRead(
            boost::hash_value(HashKeyType(this, tileIndice[i])), tile);
          res.pasteImg(*imgPtr->get(), start);
        }
      }
    }
  }

  //LOG(INFO) << "end assemble level " << level;
  return res;
}

ZImg ZImgPack::assembleImg(size_t ratio, size_t t) const
{
  CHECK(ratio >= 1);
  //LOG(INFO) << "assemble level " << level;
  ZImgRegion rgn(0, -1, 0, -1, 0, -1, 0, -1, t, t + 1);
  ZImgInfo info = rgn.clip(m_imgInfo);
  if (ratio > 1) {
    info.width = m_ratioToSize.at(ratio).width();
    info.height = m_ratioToSize.at(ratio).height();
  }
  ZImg res(info);

  for (size_t z = 0; z < m_imgInfo.depth; ++z) {
    auto tiit = m_rtzToTileIndice.find(std::make_tuple(ratio, t, int(z)));
    if (tiit != m_rtzToTileIndice.end()) {
      const std::vector<size_t>& tileIndice = tiit->second;
      for (size_t i = 0; i < tileIndice.size(); ++i) {
        const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
        ZVoxelCoordinate start(tile.x / double(ratio), tile.y / double(ratio), z, 0, 0);

        std::shared_ptr<ZImg>* imgPtr = ZImgCacheInstance.getOrRead(boost::hash_value(HashKeyType(this, tileIndice[i])),
                                                                    tile);
        res.pasteImg(*imgPtr->get(), start);
      }
    }
  }
  //LOG(INFO) << "end assemble level " << level;
  return res;
}

ZImg ZImgPack::assembleImg(size_t ratio, size_t t, size_t z) const
{
  CHECK(ratio >= 1);
  ZImgRegion rgn(0, -1, 0, -1, z, z + 1, 0, -1, t, t + 1);
  ZImgInfo info = rgn.clip(m_imgInfo);
  if (ratio > 1) {
    info.width = m_ratioToSize.at(ratio).width();
    info.height = m_ratioToSize.at(ratio).height();
  }
  ZImg res(info);

  auto tiit = m_rtzToTileIndice.find(std::make_tuple(ratio, t, int(z)));
  if (tiit != m_rtzToTileIndice.end()) {
    const std::vector<size_t>& tileIndice = tiit->second;
    for (size_t i = 0; i < tileIndice.size(); ++i) {
      const ZImgSubBlock& tile = *m_allTiles[tileIndice[i]].get();
      ZVoxelCoordinate start(tile.x / double(ratio), tile.y / double(ratio), 0, 0, 0);

      std::shared_ptr<ZImg>* imgPtr = ZImgCacheInstance.getOrRead(boost::hash_value(HashKeyType(this, tileIndice[i])),
                                                                  tile);
      res.pasteImg(*imgPtr->get(), start);
    }
  }
  return res;
}

void ZImgPack::updateDerivedData()
{
  //LOG(INFO) << m_imgInfo.toQString();
  if (!m_diskCached) {
    m_maximumProjectedAlongZImg.clear();
    m_img.computeMinMax(m_minIntensity, m_maxIntensity);
    m_minMaxState = MinMaxState::Complete;
    m_rangeMin = m_img.dataRangeMin<double>();
    m_rangeMax = m_img.dataRangeMax<double>();
    //    if (m_imgInfo.voxelByteNumber() == 2 && m_imgInfo.voxelFormat == VoxelFormat::Unsigned && m_maxIntensity < 4096) {
    //      m_rangeMax = 4095;
    //    }
    if (m_imgInfo.voxelFormat == VoxelFormat::Float && (m_maxIntensity > 1.0 || m_minIntensity < 0.0)) {
      m_rangeMin = m_minIntensity;
      m_rangeMax = m_maxIntensity;
    }
    if (m_imgInfo.depth == 1) {
      m_maximumProjectedAlongZImg = m_img.createView();
    }
  } else {
    m_img.clear();
    m_maximumProjectedAlongZImg.clear();
  }

  m_sizeInfo.clear();
  m_detailedInfo.clear();

  updateNameTootip();
}

void ZImgPack::updateNameTootip()
{
  if (isSequence()) {
    m_name =
      QFileInfo(m_imgSource.filenames[0]).fileName() + QString(" %1 Sequence").arg(enumToString(m_imgSource.catDim));
    m_tooltip = m_imgSource.filenames.join("\n");
  } else {
    if (m_numScenes == 1) {
      m_name = QFileInfo(m_imgSource.filenames[0]).fileName();
      m_tooltip = m_imgSource.filenames[0];
    } else {
      m_name = QFileInfo(m_imgSource.filenames[0]).fileName() +
               QString(" scene %1 of %2").arg(m_imgSource.scene + 1).arg(m_numScenes);
      m_tooltip = m_imgSource.filenames[0] + QString(" scene %1 of %2").arg(m_imgSource.scene + 1).arg(m_numScenes);
    }
  }
}

size_t ZImgPack::ratioForScale(double scale) const
{
  CHECK(!m_ratioToSize.empty());
  size_t needRatio = std::max(1.0, std::floor(1.0 / scale));
  return readRatioOf(needRatio);
}

size_t ZImgPack::readRatioOf(size_t needRatio) const
{
  size_t readRatio = 1;
  for (auto it = m_ratioToSize.begin(); it != m_ratioToSize.end(); ++it) {
    if (it->first > needRatio) {
      break;
    }
    readRatio = it->first;
  }
  return readRatio;
}

} // namespace
