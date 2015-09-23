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

namespace {

struct MaxOp {
  template<typename TVoxel, typename TVoxelOther>
  TVoxel operator()(TVoxel voxelRef, TVoxelOther otherVoxel) const
  {
    return std::max(voxelRef, (TVoxel)otherVoxel);
  }
};

}

namespace nim {

ZImgPackSubBlock::ZImgPackSubBlock(const QString &fn,
                                   const ZImgTileKey &key)
  : ZImgSubBlock(std::get<1>(key),
                 std::get<2>(key),
                 std::get<3>(key),
                 std::get<4>(key),
                 std::get<5>(key),
                 std::get<6>(key),
                 std::get<7>(key))
  , m_type(Type::CacheFile)
  , m_imgSource(fn)
{
}

ZImgPackSubBlock::ZImgPackSubBlock(const ZImgSource &imgSource, size_t t, size_t slice,
                                   const ZImgTileKey &key)
  : ZImgSubBlock(std::get<1>(key),
                 std::get<2>(key),
                 std::get<3>(key),
                 std::get<4>(key),
                 std::get<5>(key),
                 std::get<6>(key),
                 std::get<7>(key))
  , m_type(Type::OrigSource)
  , m_imgSource(imgSource)
{
  m_imgSource.region.start.z = slice;
  m_imgSource.region.end.z = slice+1;
  m_imgSource.region.start.t = t;
  m_imgSource.region.end.t = t+1;
}

ZImgPackSubBlock::ZImgPackSubBlock(const ZImgSource &imgSource, size_t t, size_t sliceStart, size_t sliceEnd,
                                   const ZImgTileKey &key)
  : ZImgSubBlock(std::get<1>(key),
                 std::get<2>(key),
                 std::get<3>(key),
                 std::get<4>(key),
                 std::get<5>(key),
                 std::get<6>(key),
                 std::get<7>(key))
  , m_type(Type::OrigSourceMIP)
  , m_imgSource(imgSource)
  , m_zStart(sliceStart)
  , m_zEnd(sliceEnd)
{
  m_imgSource.region.start.t = t;
  m_imgSource.region.end.t = t+1;
}

ZImg ZImgPackSubBlock::read()
{
  ZImg res;
  switch (m_type) {
  case Type::CacheFile:
    res.load(m_imgSource);
    break;
  case Type::OrigSource:
    res.load(m_imgSource);
    break;
  case Type::OrigSourceMIP:
    for (size_t slc=m_zStart; slc<m_zEnd; ++slc) {
      m_imgSource.region.start.z = slc;
      m_imgSource.region.end.z = slc+1;
      ZImg tImg;
      tImg.load(m_imgSource);    //todo: reading result not cached
      if (slc == m_zStart) {
        res.swap(tImg);
      } else {
        res.binaryOperation(tImg, MaxOp());
      }
    }
    break;
  default:
    assert(false);
    break;
  }
  return res;
}

ZImgPack::ZImgPack(ZImg &img, const QString &fileName)
  : m_imgInfo(img.info())
  , m_imgSource(fileName)
  , m_numScenes(1), m_hasUnsavedChange(false)
  , m_offsetX(0), m_offsetY(0), m_offsetZ(0), m_offsetT(0)
  , m_minMaxState(MinMaxState::Invalid)
  , m_diskCached(true)
{
  if (m_imgInfo.isEmpty()) {
    throw ZIOException("Can not create ImgPack from empty img");
  }

  m_rangeMin = m_imgInfo.dataRangeMin<double>();
  m_rangeMax = m_imgInfo.dataRangeMax<double>();

  //createPyramidalFolder(m_imgSource.filenames[0]);
  buildPyramidal(img);
  m_ratioWidths.push_back(0);
  m_ratioHeights.push_back(0);
  for (size_t i=1; i<m_ratioTileMaps.size(); ++i) {
    m_ratioWidths.push_back(std::ceil(double(m_imgInfo.width) / i));
    m_ratioHeights.push_back(std::ceil(double(m_imgInfo.height) / i));
  }

  updateDerivedData();
}

ZImgPack::ZImgPack(const QString &fileName, size_t scene, FileFormat format, size_t numScene,
                   const ZImgInfo *info, const std::vector<std::shared_ptr<ZImgSubBlock>> *subBlock)
  : m_imgSource(fileName, ZImgRegion(), scene, format)
  , m_hasUnsavedChange(false)
  , m_offsetX(0), m_offsetY(0), m_offsetZ(0), m_offsetT(0)
  , m_minMaxState(MinMaxState::Invalid)
  , m_diskCached(true)
{
  const std::vector<std::shared_ptr<ZImgSubBlock>> *sceneSubBlock = nullptr;
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

  //todo: Is it reasonable to read whole image if it is float only to get its data range?
  if (sceneSubBlock->empty() || m_imgInfo.voxelFormat == VoxelFormat::Float) {
    m_diskCached = false;
    ZImgIOInstance.readImg(m_imgSource, m_img);
  } else {
    if (m_imgSource.totalFileSize <= m_fastReadSizeThreshold && m_imgInfo.byteNumber() <= std::numeric_limits<int>::max()) {
      //createPyramidalFolder(m_imgSource.filenames[0]);
      buildPyramidal();
    } else {
      buildFastReadIndex(*sceneSubBlock);
    }
    m_ratioWidths.push_back(0);
    m_ratioHeights.push_back(0);
    for (size_t i=1; i<m_ratioTileMaps.size(); ++i) {
      m_ratioWidths.push_back(std::ceil(double(m_imgInfo.width) / i));
      m_ratioHeights.push_back(std::ceil(double(m_imgInfo.height) / i));
    }
  }

  updateDerivedData();
}

ZImgPack::ZImgPack(const QStringList &files, Dimension catDim, size_t scene, FileFormat format, size_t numScene,
                   const ZImgInfo *info, const std::vector<std::shared_ptr<ZImgSubBlock>> *subBlock)
  : m_imgSource(files, catDim, ZImgRegion(), scene, format, true)
  , m_hasUnsavedChange(false)
  , m_offsetX(0), m_offsetY(0), m_offsetZ(0), m_offsetT(0)
  , m_minMaxState(MinMaxState::Invalid)
  , m_diskCached(true)
{
  const std::vector<std::shared_ptr<ZImgSubBlock>> *sceneSubBlock = nullptr;
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

  //todo: Is it reasonable to read whole image if it is float only to get its data range?
  if (sceneSubBlock->empty() || m_imgInfo.voxelFormat == VoxelFormat::Float) {
    m_diskCached = false;
    ZImgIOInstance.readImg(m_imgSource, m_img);
  } else {
    if (m_imgSource.totalFileSize <= m_fastReadSizeThreshold && m_imgInfo.byteNumber() <= std::numeric_limits<int>::max()) {
      //createPyramidalFolder(m_imgSource.filenames[0]);
      buildPyramidal();
    } else {
      buildFastReadIndex(*sceneSubBlock);
    }
    m_ratioWidths.push_back(0);
    m_ratioHeights.push_back(0);
    for (size_t i=1; i<m_ratioTileMaps.size(); ++i) {
      m_ratioWidths.push_back(std::ceil(double(m_imgInfo.width) / i));
      m_ratioHeights.push_back(std::ceil(double(m_imgInfo.height) / i));
    }
  }

  updateDerivedData();
}

ZImgPack::~ZImgPack()
{
  for (size_t l=0; l<m_ratioTileMaps.size(); ++l) {
    for (auto it = m_ratioTileMaps[l].begin(); it != m_ratioTileMaps[l].end(); ++it) {
      ZImgCacheInstance.remove(boost::hash_value(it->first));
    }
  }

  if (!m_pyramidalFolder.isEmpty()) {
    QDir dir(m_pyramidalFolder);
    dir.removeRecursively();
  }
}

const QString &ZImgPack::sizeInfo() const
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

  std::vector<ZImgInfo> infos;
  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  ZImgIOInstance.readInfo(m_imgSource.filenames[0], infos, &subBlocks, nullptr, m_imgSource.format);
  assert(!infos.empty() && !subBlocks.empty());
  m_imgInfo = infos[0];
  m_numScenes = infos.size();
  buildFastReadIndex(subBlocks[0]);
  m_ratioHeights.clear();
  m_ratioWidths.clear();
  m_ratioWidths.push_back(0);
  m_ratioHeights.push_back(0);
  for (size_t i=1; i<m_ratioTileMaps.size(); ++i) {
    m_ratioWidths.push_back(std::ceil(double(m_imgInfo.width) / i));
    m_ratioHeights.push_back(std::ceil(double(m_imgInfo.height) / i));
  }

  updateDerivedData();
}

bool ZImgPack::needUpdate(const QRectF &viewport, double scale, const QRectF &oldViewport, double oldScale) const
{
  if (!m_diskCached)
    return false;

  size_t readRatio = ratioForScale(scale);
  size_t oldReadRatio = ratioForScale(oldScale);
  if (readRatio != oldReadRatio)
    return true;


  for (auto it = m_ratioTileMaps[readRatio].begin(); it != m_ratioTileMaps[readRatio].end(); ++it) {
    if (std::get<2>(it->first) != 0 || std::get<3>(it->first) != 0 || std::get<8>(it->first))
      continue;
    int64_t x = std::get<4>(it->first);
    int64_t y = std::get<5>(it->first);
    size_t width = std::get<6>(it->first);
    size_t height = std::get<7>(it->first);
    QRectF tileRect(x, y, width, height);
    bool vpIntersect = tileRect.intersects(viewport);
    bool oldVpIntersect = tileRect.intersects(oldViewport);
    if (vpIntersect != oldVpIntersect)
      return true;
  }

  return false;
}

void ZImgPack::retrieveCoveredImgs(std::vector<std::shared_ptr<ZImg>> &imgs, std::vector<QPoint> &locs, std::vector<double> &scales,
                                   size_t z, size_t t, const QRectF &viewport, double scale, bool mip) const
{
  assert(m_diskCached);

  imgs.clear();
  locs.clear();
  scales.clear();

  size_t readRatio = ratioForScale(scale);

  if (m_imgInfo.depth == 1)
    mip = false;
  for (auto it = m_ratioTileMaps[readRatio].begin(); it != m_ratioTileMaps[readRatio].end(); ++it) {
    if (std::get<2>(it->first) != t || std::get<3>(it->first) != z || std::get<8>(it->first) != mip)
      continue;
    int64_t x = std::get<4>(it->first);
    int64_t y = std::get<5>(it->first);
    size_t width = std::get<6>(it->first);
    size_t height = std::get<7>(it->first);
    QRectF tileRect(x, y, width, height);
    if (tileRect.intersects(viewport)) {
      size_t keyHash = boost::hash_value(it->first);
      std::shared_ptr<ZImg> *imgPtr = ZImgCacheInstance.object(keyHash);
      if (imgPtr) {
        imgs.push_back(*imgPtr);
      } else {
        // read from disk
        imgPtr = new std::shared_ptr<ZImg>(new ZImg());
        **imgPtr = it->second->read();
        imgs.push_back(*imgPtr);
        ZImgCacheInstance.insert(keyHash, imgPtr, std::max(size_t(1), (*imgPtr)->byteNumber() / 1024 / 1024));
      }
      locs.push_back(QPoint(x, y));
      scales.push_back(readRatio);
      //LINFO() << level << (1<<level) << x << y << width << height;
    }
  }
}

double ZImgPack::value(size_t x, size_t y, size_t z, size_t c, size_t t, bool mip) const
{
  if (m_diskCached) {
    assert(!m_ratioTileMaps.empty());
    if (m_imgInfo.depth == 1)
      mip = false;
    for (auto it = m_ratioTileMaps[1].begin(); it != m_ratioTileMaps[1].end(); ++it) {
      if (std::get<2>(it->first) != t || std::get<3>(it->first) != z || std::get<8>(it->first) != mip)
        continue;
      assert(std::get<4>(it->first) >= 0 && std::get<5>(it->first) >= 0);
      size_t tx = std::get<4>(it->first);
      size_t ty = std::get<5>(it->first);
      size_t twidth = std::get<6>(it->first);
      size_t theight = std::get<7>(it->first);
      if (x >= tx && x < tx + twidth && y >= ty && y < ty + theight) {
        size_t keyHash = boost::hash_value(it->first);
        std::shared_ptr<ZImg> *imgPtr = ZImgCacheInstance.object(keyHash);
        if (imgPtr) {
          return (*imgPtr)->value<double>(x-tx, y-ty, 0, c, 0);
        } else {
          // read from disk
          imgPtr = new std::shared_ptr<ZImg>(new ZImg());
          **imgPtr = it->second->read();
          double retval = (*imgPtr)->value<double>(x-tx, y-ty, 0, c, 0);
          ZImgCacheInstance.insert(keyHash, imgPtr, std::max(size_t(1), (*imgPtr)->byteNumber() / 1024 / 1024));
          return retval;
        }
      }
    }
    return 0;
  } else {
    if (mip) {
      assert(!m_maximumProjectedAlongZImg.isEmpty());
      return m_maximumProjectedAlongZImg.value<double>(x, y, 0, c, t);
    } else {
      return m_img.value<double>(x, y, z, c, t);
    }
  }
}

double ZImgPack::displayValue(size_t x, size_t y, size_t z, size_t c, size_t t, bool mip) const
{
  if (m_diskCached) {
    assert(!m_ratioTileMaps.empty());
    if (m_imgInfo.depth == 1)
      mip = false;
    bool hasTile = false;
    int64_t ix = x;
    int64_t iy = y;
    for (size_t i=1; i<m_ratioTileMaps.size(); ++i) {
      for (auto it = m_ratioTileMaps[i].begin(); it != m_ratioTileMaps[i].end(); ++it) {
        if (std::get<2>(it->first) != t || std::get<3>(it->first) != z || std::get<8>(it->first) != mip)
          continue;
        int64_t tx = std::get<4>(it->first);
        int64_t ty = std::get<5>(it->first);
        int64_t twidth = std::get<6>(it->first);
        int64_t theight = std::get<7>(it->first);
        if (ix >= tx && ix < tx + twidth && iy >= ty && iy < ty + theight) {
          if (i == 1)
            hasTile = true;
          size_t keyHash = boost::hash_value(it->first);
          std::shared_ptr<ZImg> *imgPtr = ZImgCacheInstance.object(keyHash);
          if (imgPtr) {
            return (*imgPtr)->value<double>((ix-tx)/(1.0*i), (iy-ty)/(1.0*i), 0, c, 0);
          }
        }
      }
    }
    return hasTile ? value(x, y, z, c, t, mip) : 0;
  } else {
    if (mip) {
      assert(!m_maximumProjectedAlongZImg.isEmpty());
      return m_maximumProjectedAlongZImg.value<double>(x, y, 0, c, t);
    } else {
      return m_img.value<double>(x, y, z, c, t);
    }
  }
}

ZImg ZImgPack::crop(const ZImgRegion &region) const
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
  for (auto it = m_ratioTileMaps[1].begin(); it != m_ratioTileMaps[1].end(); ++it) {
    if (std::get<8>(it->first))
      continue;
    size_t t = std::get<2>(it->first);
    size_t z = std::get<3>(it->first);
    int64_t x = std::get<4>(it->first);
    int64_t y = std::get<5>(it->first);
    size_t width = std::get<6>(it->first);
    size_t height = std::get<7>(it->first);
    ZVoxelCoordinate tileStart(x, y, z, 0, t);
    ZVoxelCoordinate start = tileStart - rgn.start;
    if ((start.x < 0 && start.x + (TCoordinate)width <= 0) || start.x >= (TCoordinate)res.width() ||
        (start.y < 0 && start.y + (TCoordinate)height <= 0) || start.y >= (TCoordinate)res.height() ||
        (start.z < 0 && start.z + (TCoordinate)1 <= 0) || start.z >= (TCoordinate)res.depth() ||
        (start.t < 0 && start.t + (TCoordinate)1 <= 0) || start.t >= (TCoordinate)res.numTimes()) {
      continue;
    }

    size_t keyHash = boost::hash_value(it->first);
    std::shared_ptr<ZImg> *imgPtr = ZImgCacheInstance.object(keyHash);
    if (imgPtr) {
      res.pasteImg(*imgPtr->get(), start);
    } else {
      // read from disk
      imgPtr = new std::shared_ptr<ZImg>(new ZImg());
      **imgPtr = it->second->read();
      res.pasteImg(*imgPtr->get(), start);
      ZImgCacheInstance.insert(keyHash, imgPtr, std::max(size_t(1), (*imgPtr)->byteNumber() / 1024 / 1024));
    }
  }

  return res;
}

ZImg ZImgPack::resizedImg(size_t width, size_t height, size_t depth, size_t t) const
{
  assert(width <= m_imgInfo.width && height <= m_imgInfo.height && depth <= m_imgInfo.depth &&
         width > 0 && height > 0 && depth > 0);
  ZImg res;

  if (!m_diskCached) {
    res = m_img.createView(-1,t).resized(width, height, depth);
    return res;
  }

  size_t l = 1;
  for (; l < m_ratioTileMaps.size()-1; ++l) {
    if (m_ratioWidths[l] < width || m_ratioHeights[l] < height) {
      break;
    }
  }
  l = std::max(l, size_t(2));
  l -= 1;
  if (m_imgInfo.width == width && m_imgInfo.height == height && m_imgInfo.depth == depth) {
    res = assembleImg(l, t);
  } else {
    ZImgInfo info = m_imgInfo;
    info.width = width;
    info.height = height;
    if (m_imgInfo.depth == 1) {  // depth == 1
      res = assembleImg(l, t, 0);
      res.resize(width, height, 1);
    } else {
      res = ZImg(info);
      for (size_t z=0; z<info.depth; ++z) {
        ZImg tmpImg = assembleImg(l,t,z);
        tmpImg.resize(width, height, 1);
        res.pasteImg(tmpImg, ZVoxelCoordinate(0,0,z,0,0));
      }
      res.resize(width, height, depth);
    }
  }
  return res;
}

const ZImg &ZImgPack::maxZProjectedImg() const
{
  assert(!m_diskCached);
  if (m_maximumProjectedAlongZImg.isEmpty()) {
    m_img.maximumZProjection().swap(m_maximumProjectedAlongZImg);
  }
  return m_maximumProjectedAlongZImg;
}

ZImg &ZImgPack::maxZProjectedImg()
{
  assert(!m_diskCached);
  if (m_maximumProjectedAlongZImg.isEmpty()) {
    m_img.maximumZProjection().swap(m_maximumProjectedAlongZImg);
  }
  return m_maximumProjectedAlongZImg;
}

ZImg ZImgPack::slice(size_t z, size_t t) const
{
  assert(m_diskCached);
  return assembleImg(1, t, z);
}

ZImg ZImgPack::allSlices(size_t t) const
{
  assert(m_diskCached);
  return assembleImg(1, t);
}

void ZImgPack::createPyramidalFolder(const QString &fileName)
{
  QString flder = ZSystemInfoInstance.imgCachePath(m_imgInfo.byteNumber() * 1.5);
  if (flder.isEmpty()) {
    throw ZIOException("No enough space in cache folder");
  }
  QDir cacheFolder(flder);
  QString fn = QFileInfo(fileName).fileName();
  for (size_t i=0; i<1e8; ++i) {
    QString folderName = QString("%1_%2_cache").arg(fn).arg(i);
    if (!cacheFolder.exists(folderName)) {
      if (!cacheFolder.mkpath(QString("%1/mip").arg(folderName))) {
        throw ZIOException(QString("Can not create folder"));
      }
      m_pyramidalFolder = cacheFolder.filePath(folderName);
      return;
    }
  }
}

void ZImgPack::createSliceTiles(ZImg *img, size_t z, size_t t, bool mip)
{
  if (true || (img->width() * img->height() <= 20480 * 20480 &&
      img->width() <= 32767 && img->height() <= 32767)) {
    size_t ratio = 1;
    if (m_ratioTileMaps.size() <= ratio) {
      m_ratioTileMaps.resize(ratio+1);
    }
    ZImgTileKey key(this, ratio, t, z, 0, 0, img->width(), img->height(), mip);
    if (mip) {
      //QString fn = mipDir.filePath(QString("%1_%2_%3_%4_%5_%6_%7.tif").arg(level).arg(t).arg(z).arg(0).arg(0).arg(img->width()).arg(img->height()));
      //img->save(fn);
      m_ratioTileMaps[ratio][key] = std::make_shared<ZImgPackSubBlock>(m_imgSource, t, 0, m_imgInfo.depth, key);
    } else {
      m_ratioTileMaps[ratio][key] = std::make_shared<ZImgPackSubBlock>(m_imgSource, t, z, key);;
    }
    ZImgCacheInstance.insert(boost::hash_value(key), new std::shared_ptr<ZImg>(img), std::max(size_t(1), img->byteNumber() / 1024 / 1024));
    return;
  }

  QDir dir(m_pyramidalFolder);
  QDir mipDir(dir.filePath("mip"));

  size_t level = 0;
  while (true) {
    if (m_ratioTileMaps.size() <= level) {
      m_ratioTileMaps.resize(level+1);
    }
    size_t lastCol = img->width() % m_tileSize;
    size_t lastRow = img->height() % m_tileSize;
    size_t numX = img->width() / m_tileSize + (lastCol > 0);
    size_t numY = img->height() / m_tileSize + (lastRow > 0);
    if (numX == 1 && numY == 1) {
      size_t width = m_imgInfo.width;
      size_t height = m_imgInfo.height;
      ZImgTileKey key(this, level, t, z, 0, 0, width, height, mip);
      QString fn;
      if (mip) {
        fn = mipDir.filePath(QString("%1_%2_%3_%4_%5_%6_%7.tif").arg(level).arg(t).arg(z).arg(0).arg(0).arg(width).arg(height));
      } else {
        fn = dir.filePath(QString("%1_%2_%3_%4_%5_%6_%7.tif").arg(level).arg(t).arg(z).arg(0).arg(0).arg(width).arg(height));
      }
      img->save(fn);
      m_ratioTileMaps[level][key] = std::make_shared<ZImgPackSubBlock>(fn, key);
      ZImgCacheInstance.insert(boost::hash_value(key), new std::shared_ptr<ZImg>(img), std::max(size_t(1), img->byteNumber() / 1024 / 1024));
      break;
    } else {
      for (size_t x=0; x<numX; ++x) {
        for (size_t y=0; y<numY; ++y) {
          size_t startX = x * m_tileSize;
          size_t endX = std::min(img->width(), startX + m_tileSize);
          size_t startY = y * m_tileSize;
          size_t endY = std::min(img->height(), startY + m_tileSize);
          size_t width = std::min(m_imgInfo.width, (endX - startX) << level);
          size_t height = std::min(m_imgInfo.height, (endY - startY) << level);
          ZImg *cropped = new ZImg();
          img->crop(ZImgRegion(startX, endX, startY, endY, 0, 1,
                               0, -1, 0, 1)).swap(*cropped);
          startX = startX << level;
          startY = startY << level;
          ZImgTileKey key(this, level, t, z, startX, startY, width, height, mip);
          QString fn;
          if (mip) {
            fn = mipDir.filePath(QString("%1_%2_%3_%4_%5_%6_%7.tif").arg(level).arg(t).arg(z).arg(startX).arg(startY).arg(width).arg(height));
          } else {
            fn = dir.filePath(QString("%1_%2_%3_%4_%5_%6_%7.tif").arg(level).arg(t).arg(z).arg(startX).arg(startY).arg(width).arg(height));
          }
          m_ratioTileMaps[level][key] = std::make_shared<ZImgPackSubBlock>(fn, key);
          cropped->save(fn);
          ZImgCacheInstance.insert(boost::hash_value(key), new std::shared_ptr<ZImg>(cropped), std::max(size_t(1), cropped->byteNumber() / 1024 / 1024));
        }
      }

      img->zoom(0.5, 0.5);
      ++level;
    }
  }
}

void ZImgPack::buildPyramidal(ZImg &img)
{
  img.computeMinMax(m_minIntensity, m_maxIntensity);
  m_rangeMin = std::min(m_rangeMin, m_minIntensity);
  m_rangeMax = std::max(m_rangeMax, m_maxIntensity);
  m_minMaxState = MinMaxState::Complete;

  if (m_imgInfo.depth == 1) {
    for (size_t t=0; t<m_imgInfo.numTimes; ++t) {
      if (m_imgInfo.numTimes == 1) {
        ZImg *tImg = new ZImg();
        tImg->swap(img);
        createSliceTiles(tImg, 0, 0);
      } else {
        ZImgRegion rgn;
        rgn.start.t = t;
        rgn.end.t = t+1;
        ZImg *tImg = new ZImg();
        img.crop(rgn).swap(*tImg);
        createSliceTiles(tImg, 0, t);
      }
    }
  } else {
    for (size_t t=0; t<m_imgInfo.numTimes; ++t) {
      ZImg *mipImg = new ZImg();
      for (size_t z=0; z<m_imgInfo.depth; ++z) {
        ZImgRegion rgn;
        rgn.start.z = z;
        rgn.end.z = z+1;
        rgn.start.t = t;
        rgn.end.t = t+1;
        ZImg *tImg = new ZImg();
        img.crop(rgn).swap(*tImg);
        if (z==0) {
          *mipImg = *tImg;
        } else {
          mipImg->binaryOperation(*tImg, MaxOp());
        }
        createSliceTiles(tImg, z, t);
      }
      createSliceTiles(mipImg, 0, t, true);
    }
  }
}

void ZImgPack::buildPyramidal()
{
  if (true || m_imgInfo.byteNumber() <= ZCpuInfoInstance.nPhysicalRAM / 2) {
    ZImg tImg;
    tImg.load(m_imgSource.filenames, m_imgSource.catDim, ZImgRegion(), m_imgSource.scene, m_imgSource.format);
    buildPyramidal(tImg);
    return;
  }

  if (m_imgInfo.depth == 1) {
    for (size_t t=0; t<m_imgInfo.numTimes; ++t) {
      ZImgRegion rgn;
      rgn.start.t = t;
      rgn.end.t = t+1;
      ZImg *tImg = new ZImg();
      tImg->load(m_imgSource.filenames, m_imgSource.catDim, rgn, m_imgSource.scene, m_imgSource.format);
      createSliceTiles(tImg, 0, t);
    }
  } else {
    for (size_t t=0; t<m_imgInfo.numTimes; ++t) {
      ZImg *mipImg = new ZImg();
      for (size_t z=0; z<m_imgInfo.depth; ++z) {
        ZImgRegion rgn;
        rgn.start.z = z;
        rgn.end.z = z+1;
        rgn.start.t = t;
        rgn.end.t = t+1;
        ZImg *tImg = new ZImg();
        tImg->load(m_imgSource.filenames, m_imgSource.catDim, rgn, m_imgSource.scene, m_imgSource.format);
        if (z==0) {
          *mipImg = *tImg;
        } else {
          mipImg->binaryOperation(*tImg, MaxOp());
        }
        createSliceTiles(tImg, z, t);
      }
      createSliceTiles(mipImg, 0, t, true);
    }
  }
}

void ZImgPack::buildFastReadIndex(const std::vector<std::shared_ptr<ZImgSubBlock>> &subBlocks)
{
  m_ratioTileMaps.clear();
  for (size_t i=0; i<subBlocks.size(); ++i) {
    ZImgSubBlock *sb = subBlocks[i].get();
    if (m_ratioTileMaps.size() <= sb->ratio)
      m_ratioTileMaps.resize(sb->ratio+1);
    ZImgTileKey key(this, sb->ratio, sb->t, sb->z, sb->x, sb->y, sb->width, sb->height, false);
    m_ratioTileMaps[sb->ratio][key] = subBlocks[i];
  }

  if (m_imgInfo.depth > 1) {
    for (size_t t=0; t<m_imgInfo.numTimes; ++t) {
      ZImgTileKey key(this, 0, t, 0, 0, 0, m_imgInfo.width, m_imgInfo.height, true);
      m_ratioTileMaps[1][key] = std::make_shared<ZImgPackSubBlock>(m_imgSource, t, 0, m_imgInfo.depth, key);
    }
  }
}

ZImg ZImgPack::assembleImg(size_t ratio) const
{
  assert(ratio >= 1);
  ZImgInfo info = m_imgInfo;
  if (ratio > 1) {
    info.width = m_ratioWidths[ratio];
    info.height = m_ratioHeights[ratio];
  }
  //LINFO() << info.toQString();
  ZImg res(info);

  for (auto it = m_ratioTileMaps[ratio].begin(); it != m_ratioTileMaps[ratio].end(); ++it) {
    if (std::get<8>(it->first))
      continue;
    size_t t = std::get<2>(it->first);
    size_t z = std::get<3>(it->first);
    int64_t x = std::get<4>(it->first);
    int64_t y = std::get<5>(it->first);
    ZVoxelCoordinate start(x / double(ratio), y / double(ratio), z, 0, t);

    size_t keyHash = boost::hash_value(it->first);
    std::shared_ptr<ZImg> *imgPtr = ZImgCacheInstance.object(keyHash);
    if (imgPtr) {
      res.pasteImg(*imgPtr->get(), start);
    } else {
      // read from disk
      imgPtr = new std::shared_ptr<ZImg>(new ZImg());
      **imgPtr = it->second->read();
      res.pasteImg(*imgPtr->get(), start);
      ZImgCacheInstance.insert(keyHash, imgPtr, std::max(size_t(1), (*imgPtr)->byteNumber() / 1024 / 1024));
    }
  }
  //LINFO() << "end assemble level" << level;
  return res;
}

ZImg ZImgPack::assembleImg(size_t ratio, size_t t) const
{
  assert(ratio >= 1);
  //LINFO() << "assemble level" << level;
  ZImgRegion rgn(0,-1,0,-1,0,-1,0,-1,t,t+1);
  ZImgInfo info = rgn.clip(m_imgInfo);
  if (ratio > 1) {
    info.width = m_ratioWidths[ratio];
    info.height = m_ratioHeights[ratio];
  }
  ZImg res(info);

  for (auto it = m_ratioTileMaps[ratio].begin(); it != m_ratioTileMaps[ratio].end(); ++it) {
    size_t tt = std::get<2>(it->first);
    if (tt != t)
      continue;
    if (std::get<8>(it->first))
      continue;
    size_t z = std::get<3>(it->first);
    int64_t x = std::get<4>(it->first);
    int64_t y = std::get<5>(it->first);
    ZVoxelCoordinate start(x / double(ratio), y / double(ratio), z, 0, 0);

    size_t keyHash = boost::hash_value(it->first);
    std::shared_ptr<ZImg> *imgPtr = ZImgCacheInstance.object(keyHash);
    if (imgPtr) {
      res.pasteImg(*imgPtr->get(), start);
    } else {
      // read from disk
      imgPtr = new std::shared_ptr<ZImg>(new ZImg());
      **imgPtr = it->second->read();
      res.pasteImg(*imgPtr->get(), start);
      ZImgCacheInstance.insert(keyHash, imgPtr, std::max(size_t(1), (*imgPtr)->byteNumber() / 1024 / 1024));
    }
  }
  //LINFO() << "end assemble level" << level;
  return res;
}

ZImg ZImgPack::assembleImg(size_t ratio, size_t t, size_t z) const
{
  assert(ratio >= 1);
  ZImgRegion rgn(0,-1,0,-1,z,z+1,0,-1,t,t+1);
  ZImgInfo info = rgn.clip(m_imgInfo);
  if (ratio > 1) {
    info.width = m_ratioWidths[ratio];
    info.height = m_ratioHeights[ratio];
  }
  ZImg res(info);

  for (auto it = m_ratioTileMaps[ratio].begin(); it != m_ratioTileMaps[ratio].end(); ++it) {
    size_t tt = std::get<2>(it->first);
    if (tt != t)
      continue;
    size_t zz = std::get<3>(it->first);
    if (zz != z)
      continue;
    if (std::get<8>(it->first))
      continue;
    int64_t x = std::get<4>(it->first);
    int64_t y = std::get<5>(it->first);
    ZVoxelCoordinate start(x / double(ratio), y / double(ratio), 0, 0, 0);

    size_t keyHash = boost::hash_value(it->first);
    std::shared_ptr<ZImg> *imgPtr = ZImgCacheInstance.object(keyHash);
    if (imgPtr) {
      res.pasteImg(*imgPtr->get(), start);
    } else {
      // read from disk
      imgPtr = new std::shared_ptr<ZImg>(new ZImg());
      **imgPtr = it->second->read();
      res.pasteImg(*imgPtr->get(), start);
      ZImgCacheInstance.insert(keyHash, imgPtr, std::max(size_t(1), (*imgPtr)->byteNumber() / 1024 / 1024));
    }
  }
  return res;
}

void ZImgPack::updateDerivedData()
{
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

  updateNameTootip();
}

void ZImgPack::updateNameTootip()
{
  if (isSequence()) {
    m_name = QFileInfo(m_imgSource.filenames[0]).fileName() + QString(" %1 Sequence").arg(enumToString(m_imgSource.catDim));
    m_tooltip = m_imgSource.filenames.join("\n");
  } else {
    if (m_numScenes == 1) {
      m_name = QFileInfo(m_imgSource.filenames[0]).fileName();
      m_tooltip = m_imgSource.filenames[0];
    } else {
      m_name = QFileInfo(m_imgSource.filenames[0]).fileName() + QString(" scene %1 of %2").arg(m_imgSource.scene+1).arg(m_numScenes);
      m_tooltip = m_imgSource.filenames[0] + QString(" scene %1 of %2").arg(m_imgSource.scene+1).arg(m_numScenes);
    }
  }
}

size_t ZImgPack::ratioForScale(double scale) const
{
  if (m_ratioTileMaps.size() == 2)
    return 1;
  assert(!m_ratioTileMaps.empty());
  size_t needRatio = std::max(1.0, std::floor(1.0 / scale));
  size_t readRatio = std::min(m_ratioTileMaps.size()-1, needRatio);
  while (m_ratioTileMaps[readRatio].empty() && readRatio > 1) {
    --readRatio;
  }
  return readRatio;
}

} // namespace
