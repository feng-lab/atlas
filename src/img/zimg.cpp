#include "zimg.h"

#include "zimgio.h"
#include "zlog.h"
#include "zimage3dutils.h"
#include "zstatisticsutils.h"
#include "zrandom.h"
#include "zbenchtimer.h"
#include <QTextStream>
#include <QFileInfo>
#include <QRegularExpression>
#include <boost/endian/conversion.hpp>
#include <algorithm>

namespace {

struct MinOp
{
  template<typename TVoxel, typename TVoxelOther>
  TVoxel operator()(TVoxel voxelRef, TVoxelOther otherVoxel) const
  {
    return std::min(voxelRef, static_cast<TVoxel>(otherVoxel));
  }
};

struct MaxOp
{
  template<typename TVoxel, typename TVoxelOther>
  TVoxel operator()(TVoxel voxelRef, TVoxelOther otherVoxel) const
  {
    return std::max(voxelRef, static_cast<TVoxel>(otherVoxel));
  }
};

} // namespace

namespace nim {

QString ZImgMetadata::toQString() const
{
  QString res;

  for (const auto& attachPointTags : m_data) {
    if (!attachPointTags.second.empty()) {
      res = res % QString("Attach Point: z: %1, c: %2, t: %3\n").arg(attachPointTags.first.z)
        .arg(attachPointTags.first.c).arg(attachPointTags.first.t);
      for (const auto& tag : attachPointTags.second) {
        res = res % "  " % tag.toQString() % "\n";
      }
    }
  }

  return res;
}

QString ZImgThumbernail::toQString() const
{
  QString res;

  for (const auto& attachPointsImgs : m_data) {
    if (!attachPointsImgs.second.empty()) {
      res = res % QString("Attach Point: z: %1, c: %2, t: %3, Number of Thumbnails: %4\n").arg(attachPointsImgs.first.z)
        .arg(attachPointsImgs.first.c).arg(attachPointsImgs.first.t).arg(attachPointsImgs.second.size());
      for (const auto& img : attachPointsImgs.second) {
        res = res % "  thumb <" % img.info().toQString() % ">\n";
      }
    }
  }

  return res;
}

ZImgSource::ZImgSource(const QString& fn, const ZImgRegion& rgn, size_t scene_, FileFormat format_)
  : region(rgn), scene(scene_), format(format_)
{
  QFileInfo fi(fn);
  if (fi.exists()) {
    filenames << fi.canonicalFilePath();
    totalFileSize += fi.size();
  } else {
    throw ZIOException(QString("file %1 does not exist").arg(fn));
  }
}

ZImgSource::ZImgSource(const QStringList& fns, Dimension catDim_, bool catScenes_,
                       const ZImgRegion& rgn, size_t scene_, FileFormat format_,
                       bool expandXY_, bool expandWithMaxValue_)
  : catDim(catDim_), catScenes(catScenes_), region(rgn), scene(scene_), format(format_), expandXY(expandXY_)
  , expandWithMaxValue(expandWithMaxValue_)
{
  for (const auto& fn : fns) {
    QFileInfo fi(fn);
    if (fi.exists()) {
      filenames << fi.canonicalFilePath();
      totalFileSize += fi.size();
    } else {
      throw ZIOException(QString("file %1 does not exist").arg(fn));
    }
  }
}

//QString ZImgSource::toQString() const
//{
//  QString res;
//  if (filenames.size() > 1) {
//    res = filenames[0] + QString(" %1 Sequence Scene %2").arg(enumToString(catDim)).arg(scene);
//    if (!region.isDefault()) {
//      res += QString(" Region %1").arg(region.toQString());
//    }
//  } else if (filenames.size() == 1) {
//    res = filenames[0] + QString(" Scene %2").arg(scene);
//    if (!region.isDefault()) {
//      res += QString(" Region %1").arg(region.toQString());
//    }
//  }
//  return res;
//}

void tag_invoke(const json::value_from_tag&, json::value& jv, const ZImgSource& imgSource)
{
  auto& jo = jv.emplace_object();
  jo["filenames"] = json::value_from(imgSource.filenames);
  jo["catDim"] = json::value_from(enumToString(imgSource.catDim));
  jo["catScenes"] = imgSource.catScenes;
  jo["region"] = json::value_from(imgSource.region);
  jo["scene"] = imgSource.scene;
  jo["format"] = json::value_from(enumToString(imgSource.format));
  jo["expandXY"] = imgSource.expandXY;
  jo["expandWithMaxValue"] = imgSource.expandWithMaxValue;
}

ZImgSource tag_invoke(const json::value_to_tag<ZImgSource>&, const json::value& jv)
{
  QStringList filenames;
  const auto& jo = jv.as_object();
  if (jo.contains("Path")) {  // compat to old version
    filenames = json::value_to<QStringList>(jo.at("Path"));
  } else {
    filenames = json::value_to<QStringList>(jo.at("filenames"));
  }
  //LOG(INFO) << filenames;

  auto catDim = Dimension::Z;
  if (jo.contains("CatDimension")) {  // compat to old version
    catDim = stringToDimension(json::value_to<QString>(jo.at("catDimension")));
  } else if (jo.contains("CatDim")) {
    catDim = stringToDimension(json::value_to<QString>(jo.at("catDim")));
  }
  size_t scene = 0;
  if (jo.contains("TileIndex")) {  // compat to old version
    scene = json::value_to<size_t>(jo.at("TileIndex"));
  } else if (jo.contains("scene")) {
    scene = json::value_to<size_t>(jo.at("scene"));
  }

  auto catScenes = false;
  if (jo.contains("catScenes")) {
    catScenes = jo.at("catScenes").as_bool();
  }
  ZImgRegion region;
  if (jo.contains("region")) {
    region = json::value_to<ZImgRegion>(jo.at("region"));
  }
  auto format = FileFormat::Unknown;
  if (jo.contains("format")) {
    format = stringToFileFormat(json::value_to<QString>(jo.at("format")));
  }
  auto expandXY = true;
  if (jo.contains("expandXY")) {
    expandXY = jo.at("expandXY").as_bool();
  }
  bool expandWithMaxValue = false;
  if (jo.contains("expandWithMaxValue")) {
    expandWithMaxValue = jo.at("expandWithMaxValue").as_bool();
  }
  return ZImgSource(filenames, catDim, catScenes, region, scene, format, expandXY, expandWithMaxValue);
}

ZImgSubBlock::~ZImgSubBlock() = default;

ZImgTileSubBlock::ZImgTileSubBlock(ZImgSource source, size_t xRatio, size_t yRatio,
                                   size_t zRatio, ImgMergeMode downsampleCombineMode)
  : ZImgSubBlock(source.region.start.t, source.region.start.x, source.region.start.y, source.region.start.z,
                 source.region.end.x - source.region.start.x,
                 source.region.end.y - source.region.start.y,
                 source.region.end.z - source.region.start.z,
                 xRatio, yRatio, zRatio)
  , m_source(std::move(source))
  , m_xRatio(xRatio)
  , m_yRatio(yRatio)
  , m_zRatio(zRatio)
  , m_downsampleCombineMode(downsampleCombineMode)
{
  CHECK(m_xRatio > 0);
  CHECK(m_yRatio > 0);
  CHECK(m_zRatio > 0);
}

std::shared_ptr<ZImg> ZImgTileSubBlock::read() const
{
  auto res = std::make_shared<ZImg>(m_source);
  if (m_xRatio > 1 || m_yRatio > 1 || m_zRatio > 1) {
    res->blockDownsample(m_xRatio, m_yRatio, m_zRatio, m_downsampleCombineMode);
  }
  return res;
}

ZImgInfo ZImgTileSubBlock::readInfo() const
{
  ZImgInfo info;
  ZImgIO().readInfo(m_source, info);
  info.voxelSizeX *= m_xRatio;
  info.voxelSizeY *= m_yRatio;
  info.voxelSizeZ *= m_zRatio;
  info.width = (info.width + m_xRatio - 1) / m_xRatio;
  info.height = (info.height + m_yRatio - 1) / m_yRatio;
  info.depth = (info.depth + m_zRatio - 1) / m_zRatio;
  return info;
}

//-----------------------------------------------------------------------------------

ZImg::ZImg()
  : m_ownData(true)
{
}

ZImg::ZImg(ZImgInfo info)
  : m_info(std::move(info))
  , m_ownData(true)
{
  allocate();
}

ZImg::ZImg(const ZImg& other)
{
  m_info = other.m_info;
  m_metadata = other.m_metadata;
  m_thumbnail = other.m_thumbnail;
  //m_ownData = other.m_ownData;
  m_ownData = true;
  if (m_ownData) { // deep copy
    allocate();
    for (size_t t = 0; t < numTimes(); ++t) {
      std::memcpy(timeData<uint8_t>(t), other.timeData<uint8_t>(t), timeByteNumber());
    }
  } else { // shallow copy
    m_data.resize(numTimes());
    for (size_t t = 0; t < numTimes(); ++t) {
      m_data[t] = other.m_data[t];
    }
  }
}

ZImg::ZImg(ZImg&& other) noexcept
{
  swap(other);
}

ZImg::ZImg(const QString& filename, ZImgRegion region, size_t scene,
           size_t xRatio, size_t yRatio, size_t zRatio, FileFormat format)
{
  load(filename, region, scene, xRatio, yRatio, zRatio, format);
}

ZImg::ZImg(const QStringList& fileList, Dimension catDim, bool catScenes,
           const ZImgRegion& region,
           size_t scene,
           size_t xRatio, size_t yRatio, size_t zRatio,
           FileFormat format,
           bool expandXY,
           bool expandWithMaxValue)
{
  load(fileList, catDim, catScenes, region, scene,
       xRatio, yRatio, zRatio,
       format, expandXY, expandWithMaxValue);
}

ZImg::ZImg(const ZImgSource& imgSource, size_t xRatio, size_t yRatio, size_t zRatio)
{
  load(imgSource, xRatio, yRatio, zRatio);
}

ZImg::~ZImg()
{
  clearData();
}

void ZImg::clear()
{
  clearData();
  m_thumbnail.clear();
  m_info.clear();
  m_metadata.clear();
  m_ownData = true;
}

void ZImg::swap(ZImg& other) noexcept
{
  m_data.swap(other.m_data);
  m_thumbnail.swap(other.m_thumbnail);
  m_info.swap(other.m_info);
  m_metadata.swap(other.m_metadata);
  std::swap(m_ownData, other.m_ownData);
}

void ZImg::getQtReadNameFilter(QStringList& filters, std::vector<FileFormat>& formats)
{
  ZImgIO().getQtReadNameFilter(filters, formats);
}

void ZImg::getQtWriteNameFilter(QStringList& filters, std::vector<FileFormat>& formats, std::vector<Compression>& comps)
{
  ZImgIO().getQtWriteNameFilter(filters, formats, comps);
}

bool ZImg::fileExtensionReadSupported(const QString& filename)
{
  return ZImgIO().fileExtensionReadSupported(filename);
}

bool ZImg::fileExtensionWriteSupported(const QString& filename)
{
  return ZImgIO().fileExtensionWriteSupported(filename);
}

void ZImg::load(const QString& filename, size_t scene, size_t xRatio, size_t yRatio, size_t zRatio, FileFormat format)
{
  clear();
  ZImgIO().readImg(filename, *this, ZImgRegion(), scene, xRatio, yRatio, zRatio, format);
}

void ZImg::load(const QString& filename, ZImgRegion region, size_t scene,
                size_t xRatio, size_t yRatio, size_t zRatio, FileFormat format)
{
  clear();
  ZImgIO().readImg(filename, *this, region, scene, xRatio, yRatio, zRatio, format);
}

void ZImg::load(const ZImgSource& imgSource, size_t xRatio, size_t yRatio, size_t zRatio)
{
  clear();
  ZImgIO().readImg(imgSource, *this, xRatio, yRatio, zRatio);
}

void ZImg::save(const QString& filename, FileFormat format, const ZImgWriteParameters& paras) const
{
  ZImgIO().writeImg(filename, *this, format, paras);
}

void ZImg::load(const QStringList& fileList, Dimension catDim, bool catScenes, size_t scene,
                size_t xRatio, size_t yRatio, size_t zRatio,
                FileFormat format,
                bool expandXY, bool expandWithMaxValue)
{
  clear();
  ZImgIO().readImg(fileList, catDim, catScenes, *this, scene, xRatio, yRatio, zRatio,
                   format, expandXY, expandWithMaxValue);
}

void
ZImg::load(const QStringList& fileList, Dimension catDim, bool catScenes, const ZImgRegion& region, size_t scene,
           size_t xRatio, size_t yRatio, size_t zRatio,
           FileFormat format,
           bool expandXY, bool expandWithMaxValue)
{
  clear();
  ZImgIO().readImg(fileList, catDim, catScenes, region, *this, scene, xRatio, yRatio, zRatio,
                   format, expandXY, expandWithMaxValue);
}

std::vector<ZImgInfo>
ZImg::readImgInfos(const QString& filename, std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                   FileFormat format)
{
  std::vector<ZImgInfo> res;
  ZImgIO().readInfos(filename, res, subBlocks, format);
  return res;
}

std::vector<ZImgInfo> ZImg::readImgInfos(const QStringList& fileList, Dimension catDim, bool catScenes,
                                         std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                                         FileFormat format, bool expandXY)
{
  std::vector<ZImgInfo> res;
  ZImgIO().readInfos(fileList, catDim, catScenes, res, subBlocks, format, expandXY);
  return res;
}

ZImgInfo ZImg::readImgInfo(const ZImgSource& imgSource,
                           std::vector<std::shared_ptr<ZImgSubBlock>>* subBlocks)
{
  ZImgInfo res;
  ZImgIO().readInfo(imgSource, res, subBlocks);
  return res;
}

ZImg ZImg::readSubBlock(const QString& filename, size_t scene, size_t blockIndex, FileFormat format)
{
  std::vector<ZImgInfo> infos;
  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  ZImgIO().readInfos(filename, infos, &subBlocks, format);
  if (scene >= subBlocks.size()) {
    throw ZIOException(QString("scene %1 overflow, max %2").arg(scene).arg(subBlocks.size()));
  }
  if (blockIndex >= subBlocks[scene].size()) {
    throw ZIOException(QString("blockIndex %1 overflow, max %2").arg(blockIndex).arg(subBlocks[scene].size()));
  }
  auto img = subBlocks[scene][blockIndex]->read();
  ZImg res;
  img->swap(res);
  return res;
}

ZImg ZImg::readSubBlock(const QStringList& fileList, nim::Dimension catDim, bool catScenes, size_t scene, size_t blockIndex,
                        nim::FileFormat format, bool expandXY)
{
  std::vector<ZImgInfo> infos;
  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  ZImgIO().readInfos(fileList, catDim, catScenes, infos, &subBlocks, format, expandXY);
  if (scene >= subBlocks.size()) {
    throw ZIOException(QString("scene %1 overflow, max %2").arg(scene).arg(subBlocks.size()));
  }
  if (blockIndex >= subBlocks[scene].size()) {
    throw ZIOException(QString("blockIndex %1 overflow, max %2").arg(blockIndex).arg(subBlocks[scene].size()));
  }
  auto img = subBlocks[scene][blockIndex]->read();
  ZImg res;
  img->swap(res);
  return res;
}

std::vector<std::vector<ZImgRegion>> ZImg::getInternalSubRegions(const QString& filename, nim::FileFormat format)
{
  return ZImgIO().getInternalSubRegions(filename, format);
}

void ZImg::wrapData(void* data, const ZImgInfo& info)
{
  clear();

  m_info = info;

  m_ownData = false;

  m_data.resize(m_info.numTimes);

  for (size_t i = 0; i < m_info.numTimes; ++i) {
    // reinterpret_cast allowed (AliasedType is char or unsigned char: this permits
    // examination of the object representation of any object as an array of unsigned char.)
    m_data[i] = reinterpret_cast<uint8_t*>(data) + i * info.timeVoxelNumber();
  }
}

void ZImg::wrapData(const std::vector<void*>& data, const ZImgInfo& info)
{
  clear();

  m_info = info;

  m_ownData = false;

  m_data.resize(m_info.numTimes);

  CHECK(m_data.size() == data.size());

  for (size_t i = 0; i < m_info.numTimes; ++i) {
    // reinterpret_cast allowed (AliasedType is char or unsigned char: this permits
    // examination of the object representation of any object as an array of unsigned char.)
    m_data[i] = reinterpret_cast<uint8_t*>(data[i]);
  }
}

template<typename TVoxel>
void ZImg::wrapData(TVoxel* data, size_t width, size_t height, size_t depth,
                    size_t numChannels, size_t numTimes)
{
  clear();

  m_info.setVoxelFormat<TVoxel>();

  m_ownData = false;

  m_info.width = width;
  m_info.height = height;
  m_info.depth = depth;
  m_info.numChannels = numChannels;
  m_info.numTimes = numTimes;

  m_info.bytesPerVoxel = sizeof(TVoxel);

  m_info.voxelSizeUnit = VoxelSizeUnit::none;
  m_info.voxelSizeX = 1;
  m_info.voxelSizeY = 1;
  m_info.voxelSizeZ = 1;

  m_info.createDefaultDescriptions();

  m_data.resize(m_info.numTimes);

  for (size_t i = 0; i < m_info.numTimes; ++i) {
    m_data[i] = reinterpret_cast<uint8_t*>(data + i * width * height * depth * numChannels);
  }
}

template void ZImg::wrapData(uint8_t*, size_t, size_t, size_t, size_t, size_t);

template void ZImg::wrapData(uint16_t*, size_t, size_t, size_t, size_t, size_t);

template void ZImg::wrapData(uint32_t*, size_t, size_t, size_t, size_t, size_t);

template void ZImg::wrapData(uint64_t*, size_t, size_t, size_t, size_t, size_t);

template void ZImg::wrapData(int8_t*, size_t, size_t, size_t, size_t, size_t);

template void ZImg::wrapData(int16_t*, size_t, size_t, size_t, size_t, size_t);

template void ZImg::wrapData(int32_t*, size_t, size_t, size_t, size_t, size_t);

template void ZImg::wrapData(int64_t*, size_t, size_t, size_t, size_t, size_t);

template void ZImg::wrapData(float*, size_t, size_t, size_t, size_t, size_t);

template void ZImg::wrapData(double*, size_t, size_t, size_t, size_t, size_t);

void ZImg::allocate()
{
  clearData();
  if (m_info.isEmpty()) {
    return;
  }
  m_data.resize(m_info.numTimes);

  for (size_t t = 0; t < m_info.numTimes; ++t) {
    m_data[t] = static_cast<uint8_t*>(boost::alignment::aligned_alloc(64, timeByteNumber()));

    if (m_data[t] == nullptr) {
      clearData();
      ZImgInfo info = m_info;
      m_info.clear();
      throw ZImgException(QString("Can not allocate memory for img <%1>").arg(info.toQString()));
    } else {
      std::memset(m_data[t], 0, timeByteNumber());
    }
  }

  if (m_info.voxelFormat == VoxelFormat::Signed) {
    switch (m_info.bytesPerVoxel) {
      case 1:
        fill(std::numeric_limits<int8_t>::min());
        break;
      case 2:
        fill(std::numeric_limits<int16_t>::min());
        break;
      case 4:
        fill(std::numeric_limits<int32_t>::min());
        break;
      case 8:
        fill(std::numeric_limits<int64_t>::min());
        break;
      default:
        break;
    }
  }
}

void ZImg::reverseEndianness()
{
  if (m_info.bytesPerVoxel == 1) {
    return;
  }

  if (m_info.bytesPerVoxel == 2) {
    for (size_t t = 0; t < numTimes(); ++t) {
      auto data = timeData<uint16_t>(t);
      size_t count = timeVoxelNumber();
      for (size_t v = 0; v < count; ++v) {
        boost::endian::endian_reverse_inplace(data[v]);
      }
    }
  } else if (m_info.bytesPerVoxel == 4) {
    for (size_t t = 0; t < numTimes(); ++t) {
      auto data = timeData<uint32_t>(t);
      size_t count = timeVoxelNumber();
      for (size_t v = 0; v < count; ++v) {
        boost::endian::endian_reverse_inplace(data[v]);
      }
    }
  } else if (m_info.bytesPerVoxel == 8) {
    for (size_t t = 0; t < numTimes(); ++t) {
      auto data = timeData<uint64_t>(t);
      size_t count = timeVoxelNumber();
      for (size_t v = 0; v < count; ++v) {
        boost::endian::endian_reverse_inplace(data[v]);
      }
    }
  }
}

QString ZImg::toQString() const
{
  QString res;
  if (isEmpty()) {
    return res;
  }
  IMG_TYPED_CALL(showContentAsQString_Impl, m_info, res)
  return res;
}

ZImg ZImg::createView(int c, int t)
{
  ZImgRegion rgn;
  if (c >= 0) {
    rgn.start.c = c;
    rgn.end.c = c + 1;
  }
  if (t >= 0) {
    rgn.start.t = t;
    rgn.end.t = t + 1;
  }
  if (!rgn.isValid(m_info)) {
    throw ZImgException(QString("Invalid view of img, c:%1, t:%2, rgn:%3, img:%4")
                          .arg(c).arg(t).arg(rgn.toQString()).arg(m_info.toQString()));
  }
  ZImg res;
  res.m_info = rgn.clip(m_info);
  res.m_data.resize(res.numTimes());
  for (size_t lt = 0; lt < res.numTimes(); ++lt) {
    res.m_data[lt] = channelData<uint8_t>(rgn.start.c, lt + rgn.start.t);
  }
  res.m_ownData = false;
  return res;
}

ZImg ZImg::createView(int c, int t) const
{
  ZImgRegion rgn;
  if (c >= 0) {
    rgn.start.c = c;
    rgn.end.c = c + 1;
  }
  if (t >= 0) {
    rgn.start.t = t;
    rgn.end.t = t + 1;
  }
  if (!rgn.isValid(m_info)) {
    throw ZImgException(QString("Invalid view of img, c:%1, t:%2, rgn:%3, img:%4")
                          .arg(c).arg(t).arg(rgn.toQString()).arg(m_info.toQString()));
  }
  ZImg res;
  res.m_info = rgn.clip(m_info);
  res.m_data.resize(res.numTimes());
  for (size_t lt = 0; lt < res.numTimes(); ++lt) {
    res.m_data[lt] = m_data[lt + rgn.start.t] + rgn.start.c * m_info.channelByteNumber();
  }
  res.m_ownData = false;
  return res;
}

ZImg ZImg::createView(size_t z, size_t c, size_t t)
{
  ZImgRegion rgn;
  rgn.start.z = z;
  rgn.end.z = z + 1;
  rgn.start.c = c;
  rgn.end.c = c + 1;
  rgn.start.t = t;
  rgn.end.t = t + 1;
  if (!rgn.isValid(m_info)) {
    throw ZImgException(QString("Invalid view of img, z: %1, c:%2, t:%3, rgn:%4, img:%5")
                          .arg(z).arg(c).arg(t).arg(rgn.toQString()).arg(m_info.toQString()));
  }
  ZImg res;
  res.m_info = rgn.clip(m_info);
  res.m_data.resize(res.numTimes());
  for (size_t lt = 0; lt < res.numTimes(); ++lt) {
    res.m_data[lt] = planeData<uint8_t>(rgn.start.z, rgn.start.c, lt + rgn.start.t);
  }
  res.m_ownData = false;
  return res;
}

ZImg ZImg::createView(size_t z, size_t c, size_t t) const
{
  ZImgRegion rgn;
  rgn.start.z = z;
  rgn.end.z = z + 1;
  rgn.start.c = c;
  rgn.end.c = c + 1;
  rgn.start.t = t;
  rgn.end.t = t + 1;
  if (!rgn.isValid(m_info)) {
    throw ZImgException(QString("Invalid view of img, z: %1, c:%2, t:%3, rgn:%4, img:%5")
                          .arg(z).arg(c).arg(t).arg(rgn.toQString()).arg(m_info.toQString()));
  }
  ZImg res;
  res.m_info = rgn.clip(m_info);
  res.m_data.resize(res.numTimes());
  for (size_t lt = 0; lt < res.numTimes(); ++lt) {
    res.m_data[lt] = m_data[lt + rgn.start.t] +
                     rgn.start.c * m_info.channelByteNumber() + rgn.start.z * m_info.planeByteNumber();
  }
  res.m_ownData = false;
  return res;
}

template<typename TValue>
void ZImg::computeMinMax(TValue& min, TValue& max) const
{
  IMG_TYPED_CALL(computeMinMax_Impl, m_info, min, max)
}

template void ZImg::computeMinMax(uint8_t&, uint8_t&) const;

template void ZImg::computeMinMax(uint16_t&, uint16_t&) const;

template void ZImg::computeMinMax(uint32_t&, uint32_t&) const;

template void ZImg::computeMinMax(uint64_t&, uint64_t&) const;

template void ZImg::computeMinMax(int8_t&, int8_t&) const;

template void ZImg::computeMinMax(int16_t&, int16_t&) const;

template void ZImg::computeMinMax(int32_t&, int32_t&) const;

template void ZImg::computeMinMax(int64_t&, int64_t&) const;

template void ZImg::computeMinMax(float&, float&) const;

template void ZImg::computeMinMax(double&, double&) const;

std::vector<size_t> ZImg::histogram(size_t nbins, const ZImg& mask) const
{
  if (nbins == 0) {
    nbins = bytesPerVoxel() > 1 ? 65536 : 256;
  }
  std::vector<size_t> res(nbins, 0);

  if (mask.isEmpty()) {
    IMG_TYPED_CALL(histogram_Impl, m_info, res)
  } else if (isSameSize(mask)) {
    IMG_TYPED_CALL_2TYPE(histogramMask_Impl, m_info, mask.info(), res, mask)
  } else {
    throw ZImgException(QString("histogram mask has different size <%1> than current img <%2>")
                          .arg(mask.info().toQString()).arg(m_info.toQString()));
  }

  return res;
}

ZImg ZImg::crop(const ZImgRegion& region) const
{
  ZImg res;
  if (region.isEmpty()) {
    return res;
  }
  ZImgRegion rgn = region;
  if (!rgn.isValid(m_info)) {
    throw ZImgException(QString("Try to crop img <%1> with invalid region <%2>")
                          .arg(m_info.toQString()).arg(rgn.toQString()));
  }

  rgn.resolveRegionEnd(m_info);
  ZImgInfo resInfo = rgn.clip(m_info);
  // create destination
  res = ZImg(resInfo);
  // start copy data
  for (size_t t = rgn.start.t; t < static_cast<size_t>(rgn.end.t); ++t) {
    if (rgn.containsWholeChannel(m_info)) {
      // copy continues channel blocks
      std::memcpy(res.timeData(t - rgn.start.t),
                  channelData(rgn.start.c, t), res.timeByteNumber());
    } else if (rgn.containsWholePlane(m_info)) {
      // copy channel by channel
      for (size_t c = rgn.start.c; c < static_cast<size_t>(rgn.end.c); ++c) {
        std::memcpy(res.channelData(c - rgn.start.c, t - rgn.start.t),
                    planeData(rgn.start.z, c, t), res.channelByteNumber());

      }
    } else if (rgn.containsWholeRow(m_info)) {
      // copy plane by plane
      for (size_t c = rgn.start.c; c < static_cast<size_t>(rgn.end.c); ++c) {
        for (size_t z = rgn.start.z; z < static_cast<size_t>(rgn.end.z); ++z) {
          std::memcpy(res.planeData(z - rgn.start.z, c - rgn.start.c, t - rgn.start.t),
                      rowData(rgn.start.y, z, c, t), res.planeByteNumber());
        }
      }
    } else {
      // copy row by row
      for (size_t c = rgn.start.c; c < static_cast<size_t>(rgn.end.c); ++c) {
        for (size_t z = rgn.start.z; z < static_cast<size_t>(rgn.end.z); ++z) {
          for (size_t y = rgn.start.y; y < static_cast<size_t>(rgn.end.y); ++y) {
            std::memcpy(res.rowData(y - rgn.start.y, z - rgn.start.z, c - rgn.start.c, t - rgn.start.t),
                        data(rgn.start.x, y, z, c, t), res.rowByteNumber());
          }
        }
      }
    }
  }

  return res;
}

ZImg ZImg::extractVoxel(size_t x, size_t y, int z, int c, int t) const
{
  ZImgRegion rgn;
  rgn.start.x = x;
  rgn.end.x = x + 1;
  rgn.start.y = y;
  rgn.end.y = y + 1;
  if (z >= 0) {
    rgn.start.z = z;
    rgn.end.z = z + 1;
  }
  if (c >= 0) {
    rgn.start.c = c;
    rgn.end.c = c + 1;
  }
  if (t >= 0) {
    rgn.start.t = t;
    rgn.end.t = t + 1;
  }
  return crop(rgn);
}

ZImg ZImg::extractCol(size_t x, int z, int c, int t) const
{
  ZImgRegion rgn;
  rgn.start.x = x;
  rgn.end.x = x + 1;
  if (z >= 0) {
    rgn.start.z = z;
    rgn.end.z = z + 1;
  }
  if (c >= 0) {
    rgn.start.c = c;
    rgn.end.c = c + 1;
  }
  if (t >= 0) {
    rgn.start.t = t;
    rgn.end.t = t + 1;
  }
  return crop(rgn);
}

ZImg ZImg::extractRow(size_t y, int z, int c, int t) const
{
  ZImgRegion rgn;
  rgn.start.y = y;
  rgn.end.y = y + 1;
  if (z >= 0) {
    rgn.start.z = z;
    rgn.end.z = z + 1;
  }
  if (c >= 0) {
    rgn.start.c = c;
    rgn.end.c = c + 1;
  }
  if (t >= 0) {
    rgn.start.t = t;
    rgn.end.t = t + 1;
  }
  return crop(rgn);
}

ZImg ZImg::extractPlane(size_t z, int c, int t) const
{
  ZImgRegion rgn;
  rgn.start.z = z;
  rgn.end.z = z + 1;
  if (c >= 0) {
    rgn.start.c = c;
    rgn.end.c = c + 1;
  }
  if (t >= 0) {
    rgn.start.t = t;
    rgn.end.t = t + 1;
  }
  return crop(rgn);
}

ZImg ZImg::extractChannel(size_t c, int t) const
{
  ZImgRegion rgn;
  rgn.start.c = c;
  rgn.end.c = c + 1;
  if (t >= 0) {
    rgn.start.t = t;
    rgn.end.t = t + 1;
  }
  return crop(rgn);
}

ZImg ZImg::extractTime(size_t t) const
{
  ZImgRegion rgn;
  rgn.start.t = t;
  rgn.end.t = t + 1;
  return crop(rgn);
}

template<typename TVoxel>
void ZImg::fillRandom_Impl()
{
  if constexpr (std::is_floating_point_v<TVoxel>) {
    std::uniform_real_distribution<TVoxel> dist(dataRangeMin<TVoxel>(), dataRangeMax<TVoxel>());
    auto& eng = ZRandom::instance().engine();
    for (size_t t = 0; t < numTimes(); ++t) {
      auto data = timeData<TVoxel>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        data[v] = dist(eng);
      }
    }
  } else if constexpr (std::is_same_v<std::uint8_t, TVoxel>) {
    std::uniform_int_distribution<uint32_t> dist(dataRangeMin<uint32_t>(), dataRangeMax<uint32_t>());
    auto& eng = ZRandom::instance().engine();
    for (size_t t = 0; t < numTimes(); ++t) {
      auto data = timeData<uint8_t>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        data[v] = dist(eng);
      }
    }
  } else if constexpr (std::is_same_v<std::int8_t, TVoxel>) {
    std::uniform_int_distribution<int32_t> dist(dataRangeMin<int32_t>(), dataRangeMax<int32_t>());
    auto& eng = ZRandom::instance().engine();
    for (size_t t = 0; t < numTimes(); ++t) {
      auto data = timeData<int8_t>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        data[v] = dist(eng);
      }
    }
  } else {
    std::uniform_int_distribution<TVoxel> dist(dataRangeMin<TVoxel>(), dataRangeMax<TVoxel>());
    auto& eng = ZRandom::instance().engine();
    for (size_t t = 0; t < numTimes(); ++t) {
      auto data = timeData<TVoxel>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        data[v] = dist(eng);
      }
    }
  }
}

ZImg& ZImg::fillRandom()
{
  IMG_TYPED_CALL(fillRandom_Impl, m_info)
  return *this;
}

ZImg& ZImg::pasteImg(const ZImg& img, const ZVoxelCoordinate& start, bool warningOn)
{
  using TCoordinate = ZVoxelCoordinate::value_type;

  if (isEmpty() || img.isEmpty()) {
    if (warningOn) {
      LOG(WARNING) << "Trying to paste empty img, abort";
    }
    return *this;
  }

  if ((start.x < 0 && start.x + static_cast<TCoordinate>(img.width()) <= 0) ||
      start.x >= static_cast<TCoordinate>(width()) ||
      (start.y < 0 && start.y + static_cast<TCoordinate>(img.height()) <= 0) ||
      start.y >= static_cast<TCoordinate>(height()) ||
      (start.z < 0 && start.z + static_cast<TCoordinate>(img.depth()) <= 0) ||
      start.z >= static_cast<TCoordinate>(depth()) ||
      (start.c < 0 && start.c + static_cast<TCoordinate>(img.numChannels()) <= 0) ||
      start.c >= static_cast<TCoordinate>(numChannels()) ||
      (start.t < 0 && start.t + static_cast<TCoordinate>(img.numTimes()) <= 0) ||
      start.t >= static_cast<TCoordinate>(numTimes())) {
    if (warningOn) {
      LOG(WARNING) << "Trying to paste img with no overlap region, abort";
    }
    return *this;
  }

  if (isSameType(img)) {
    size_t desX = std::max(start.x, TCoordinate(0));
    size_t srcX = desX - start.x;
    size_t desXEnd = std::min(start.x + static_cast<TCoordinate>(img.width()), static_cast<TCoordinate>(width()));
    size_t rowByteNumber = (desXEnd - desX) * m_info.bytesPerVoxel;

    for (TCoordinate desT = std::max(start.t, TCoordinate(0));
         desT < std::min(start.t + static_cast<TCoordinate>(img.numTimes()),
                         static_cast<TCoordinate>(numTimes()));
         ++desT) {
      size_t srcT = desT - start.t;
      for (TCoordinate desC = std::max(start.c, TCoordinate(0));
           desC < std::min(start.c + static_cast<TCoordinate>(img.numChannels()),
                           static_cast<TCoordinate>(numChannels()));
           ++desC) {
        size_t srcC = desC - start.c;
        for (TCoordinate desZ = std::max(start.z, TCoordinate(0));
             desZ < std::min(start.z + static_cast<TCoordinate>(img.depth()),
                             static_cast<TCoordinate>(depth()));
             ++desZ) {
          size_t srcZ = desZ - start.z;
#if 1
          for (TCoordinate desY = std::max(start.y, TCoordinate(0));
               desY < std::min(start.y + static_cast<TCoordinate>(img.height()),
                               static_cast<TCoordinate>(height()));
               ++desY) {
            size_t srcY = desY - start.y;

            std::memcpy(data(desX, desY, desZ, desC, desT),
                        img.data(srcX, srcY, srcZ, srcC, srcT),
                        rowByteNumber);
          }
#else
          tbb::parallel_for(tbb::blocked_range<TCoordinate>(std::max(start.y, TCoordinate(0)),
                                                            std::min(start.y + static_cast<TCoordinate>(img.height()),
                                                                     static_cast<TCoordinate>(height()))),
                            [&](const tbb::blocked_range<TCoordinate>& r) {
                              for (TCoordinate desY = r.begin(); desY != r.end(); ++desY) {
                                size_t srcY = desY - start.y;
                                std::memcpy(data(desX, desY, desZ, desC, desT),
                                       img.data(srcX, srcY, srcZ, srcC, srcT),
                                       rowByteNumber);
                              }
                            }
          );
#endif
        }
      }
    }
  } else {
    IMG_TYPED_CALL_2TYPE(pasteImg_Impl, m_info, img.info(), img, start)
  }

  return *this;
}

ZImg& ZImg::pasteImgMax(const ZImg& img, const ZVoxelCoordinate& start, bool warningOn)
{
  using TCoordinate = ZVoxelCoordinate::value_type;

  if (isEmpty() || img.isEmpty()) {
    if (warningOn) {
      LOG(WARNING) << "Trying to paste empty img, abort";
    }
    return *this;
  }

  if ((start.x < 0 && start.x + static_cast<TCoordinate>(img.width()) <= 0) ||
      start.x >= static_cast<TCoordinate>(width()) ||
      (start.y < 0 && start.y + static_cast<TCoordinate>(img.height()) <= 0) ||
      start.y >= static_cast<TCoordinate>(height()) ||
      (start.z < 0 && start.z + static_cast<TCoordinate>(img.depth()) <= 0) ||
      start.z >= static_cast<TCoordinate>(depth()) ||
      (start.c < 0 && start.c + static_cast<TCoordinate>(img.numChannels()) <= 0) ||
      start.c >= static_cast<TCoordinate>(numChannels()) ||
      (start.t < 0 && start.t + static_cast<TCoordinate>(img.numTimes()) <= 0) ||
      start.t >= static_cast<TCoordinate>(numTimes())) {
    if (warningOn) {
      LOG(WARNING) << "Trying to paste img with no overlap region, abort";
    }
    return *this;
  }

  IMG_TYPED_CALL_2TYPE(pasteImgMax_Impl, m_info, img.info(), img, start)

  return *this;
}

ZImg ZImg::cat(const std::vector<ZImg>& imgsIn, Dimension dim)
{
  std::vector<const ZImg*> imgs;
  for (const auto& img : imgsIn) {
    imgs.push_back(&img);
  }
  return cat(imgs, dim);
}

ZImg ZImg::cat(const std::vector<ZImg*>& imgsIn, Dimension dim)
{
  std::vector<const ZImg*> imgs;
  imgs.insert(imgs.end(), imgsIn.begin(), imgsIn.end());
  return cat(imgs, dim);
}

ZImg ZImg::cat(const std::vector<const ZImg*>& imgsIn, Dimension dim)
{
  // remove empty img
  std::vector<const ZImg*> imgs;
  for (auto img : imgsIn) {
    if (img && !img->isEmpty()) {
      imgs.push_back(img);
    }
  }

  if (imgs.empty()) {
    return ZImg();
  }
  if (imgs.size() == 1) {
    return *(imgs[0]);
  }

  // check dimensions and type
  const ZImg* firstImg = imgs[0];
  ZImgInfo firstInfo = firstImg->info();
  ZImgInfo resInfo = firstInfo;
  firstInfo.setSize(dim, 0);
  for (size_t idx = 1; idx < imgs.size(); ++idx) {
    ZImgInfo info = imgs[idx]->info();
    resInfo.setSize(dim, resInfo.size(dim) + info.size(dim));
    info.setSize(dim, 0);
    if (!info.isSameType(firstInfo) || !info.isSameSize(firstInfo)) {
      throw ZImgException(
        QString("Can not concat img <%1> and img <%2>").arg(info.toQString()).arg(firstInfo.toQString()));
    }
  }
  if (dim == Dimension::C) {
    resInfo.channelNames.clear();
    resInfo.channelColors.clear();
    for (auto img : imgs) {
      ZImgInfo info = img->info();
      resInfo.channelNames.insert(resInfo.channelNames.end(), info.channelNames.begin(), info.channelNames.end());
      resInfo.channelColors.insert(resInfo.channelColors.end(), info.channelColors.begin(), info.channelColors.end());
    }
  }

  // create result img
  ZImg res(resInfo);

  if (dim == Dimension::T) {
    size_t tIdx = 0;
    for (auto img : imgs) {
      for (size_t t = 0; t < img->numTimes(); ++t) {
        size_t desT = tIdx++;
        std::memcpy(res.timeData(desT), img->timeData(t), res.timeByteNumber());
      }
    }
  } else if (dim == Dimension::C) {
    for (size_t t = 0; t < res.numTimes(); ++t) {
      size_t cIdx = 0;
      for (auto img : imgs) {
        std::memcpy(res.channelData(cIdx, t), img->timeData(t), img->timeByteNumber());
        cIdx += img->numChannels();
      }
    }
  } else if (dim == Dimension::Z) {
    for (size_t t = 0; t < res.numTimes(); ++t) {
      for (size_t c = 0; c < res.numChannels(); ++c) {
        size_t zIdx = 0;
        for (auto img : imgs) {
          std::memcpy(res.planeData(zIdx, c, t), img->channelData(c, t), img->channelByteNumber());
          zIdx += img->depth();
        }
      }
    }
  } else if (dim == Dimension::Y) {
    for (size_t t = 0; t < res.numTimes(); ++t) {
      for (size_t c = 0; c < res.numChannels(); ++c) {
        for (size_t z = 0; z < res.depth(); ++z) {
          size_t yIdx = 0;
          for (auto img : imgs) {
            std::memcpy(res.rowData(yIdx, z, c, t), img->planeData(z, c, t), img->planeByteNumber());
            yIdx += img->height();
          }
        }
      }
    }
  } else {
    for (size_t t = 0; t < res.numTimes(); ++t) {
      for (size_t c = 0; c < res.numChannels(); ++c) {
        for (size_t z = 0; z < res.depth(); ++z) {
          for (size_t y = 0; y < res.height(); ++y) {
            size_t xIdx = 0;
            for (auto img : imgs) {
              std::memcpy(res.data(xIdx, y, z, c, t), img->rowData(y, z, c, t), img->rowByteNumber());
              xIdx += img->width();
            }
          }
        }
      }
    }
  }

  return res;
}

ZImg ZImg::combine(const std::vector<ZImg>& imgsIn, ImgMergeMode mode)
{
  std::vector<const ZImg*> imgs;
  for (const auto& img : imgsIn) {
    imgs.push_back(&img);
  }
  return combine(imgs, mode);
}

ZImg ZImg::combine(const std::vector<ZImg*>& imgsIn, ImgMergeMode mode)
{
  std::vector<const ZImg*> imgs;
  imgs.insert(imgs.end(), imgsIn.begin(), imgsIn.end());
  return combine(imgs, mode);
}

ZImg ZImg::combine(const std::vector<const ZImg*>& imgsIn, ImgMergeMode mode)
{
  // remove empty img
  std::vector<const ZImg*> imgs;
  for (auto img : imgsIn) {
    if (img && !img->isEmpty()) {
      imgs.push_back(img);
    }
  }

  if (imgs.empty()) {
    return ZImg();
  }
  if (imgs.size() == 1) {
    return *(imgs[0]);
  }

  // check dimensions size
  const ZImg* firstImg = imgs[0];
  ZImgInfo firstInfo = firstImg->info();
  for (size_t idx = 1; idx < imgs.size(); ++idx) {
    ZImgInfo info = imgs[idx]->info();
    if (!info.isSameType(firstInfo) || !info.isSameSize(firstInfo)) {
      throw ZImgException(
        QString("Can not combine img <%1> and img <%2>").arg(info.toQString()).arg(firstInfo.toQString()));
    }
  }

  IMG_RETURN_TYPED_CALL(combine_Impl, firstInfo, imgs, mode)
}

ZImg ZImg::projectAlongDim(Dimension dim, ImgMergeMode mode, int startIn, int endIn) const
{
  if (isEmpty() || m_info.size(dim) == 1) {
    return *this;
  }

  size_t dstart = 0;
  size_t dend = m_info.size(dim);
  if (startIn >= 0 && endIn >= 0) {
    CHECK(startIn <= endIn);
    CHECK(size_t(endIn) < m_info.size(dim));
    dstart = startIn;
    dend = endIn + 1;
  }

  ZImg res;
  if (mode == ImgMergeMode::Max) {
    for (size_t i = dstart; i < dend; ++i) {
      ZImg subImg;
      switch (dim) {
        case Dimension::T:
          subImg = extractTime(i);
          break;
        case Dimension::C:
          subImg = extractChannel(i, -1);
          break;
        case Dimension::Z:
          subImg = extractPlane(i, -1, -1);
          break;
        case Dimension::Y:
          subImg = extractRow(i, -1, -1, -1);
          break;
        case Dimension::X:
          subImg = extractCol(i, -1, -1, -1);
          break;
        default:
          break;
      }
      if (i == dstart) {
        res.swap(subImg);
      } else {
        res.binaryOperation(subImg, MaxOp());
      }
    }
  } else if (mode == ImgMergeMode::Min) {
    for (size_t i = dstart; i < dend; ++i) {
      ZImg subImg;
      switch (dim) {
        case Dimension::T:
          subImg = extractTime(i);
          break;
        case Dimension::C:
          subImg = extractChannel(i, -1);
          break;
        case Dimension::Z:
          subImg = extractPlane(i, -1, -1);
          break;
        case Dimension::Y:
          subImg = extractRow(i, -1, -1, -1);
          break;
        case Dimension::X:
          subImg = extractCol(i, -1, -1, -1);
          break;
        default:
          break;
      }
      if (i == dstart) {
        res.swap(subImg);
      } else {
        res.binaryOperation(subImg, MinOp());
      }
    }
  } else if (mode == ImgMergeMode::First) {
    for (size_t i = dstart; i < dend; ++i) {
      ZImg subImg;
      switch (dim) {
        case Dimension::T:
          subImg = extractTime(i);
          break;
        case Dimension::C:
          subImg = extractChannel(i, -1);
          break;
        case Dimension::Z:
          subImg = extractPlane(i, -1, -1);
          break;
        case Dimension::Y:
          subImg = extractRow(i, -1, -1, -1);
          break;
        case Dimension::X:
          subImg = extractCol(i, -1, -1, -1);
          break;
        default:
          break;
      }
      if (i == dstart) {
        res.swap(subImg);
        break;
      }
    }
  } else {
    std::vector<ZImg> subImgs(dend - dstart);
    for (size_t i = dstart; i < dend; ++i) {
      switch (dim) {
        case Dimension::T:
          subImgs[i - dstart] = extractTime(i);
          break;
        case Dimension::C:
          subImgs[i - dstart] = extractChannel(i, -1);
          break;
        case Dimension::Z:
          subImgs[i - dstart] = extractPlane(i, -1, -1);
          break;
        case Dimension::Y:
          subImgs[i - dstart] = extractRow(i, -1, -1, -1);
          break;
        case Dimension::X:
          subImgs[i - dstart] = extractCol(i, -1, -1, -1);
          break;
        default:
          break;
      }
    }

    std::vector<const ZImg*> imgs;
    for (const auto& subImg : subImgs) {
      imgs.push_back(&subImg);
    }
    res = combine(imgs, mode);
  }
  return res;
}

ZImg ZImg::maximumZProjection(int start, int end) const
{
  return projectAlongDim(Dimension::Z, ImgMergeMode::Max, start, end);
}

ZImg ZImg::normalized() const
{
  ZImg res(*this);
  res.normalize();
  return res;
}

ZImg& ZImg::normalize()
{
  if (isEmpty()) {
    return *this;
  }
  IMG_RETURN_TYPED_CALL(normalize_Impl, m_info)
}

template<typename TDesVoxel>
ZImg ZImg::castTo() const
{
  if (isType<TDesVoxel>()) {
    return *this;
  }

  ZImgInfo info = m_info;
  info.setVoxelFormat<TDesVoxel>();
  ZImg res(info);

  IMG_TYPED_CALL_FIX2NDTYPE(cast_Impl, m_info, TDesVoxel, res)

  return res;
}

template ZImg ZImg::castTo<uint8_t>() const;

template ZImg ZImg::castTo<uint16_t>() const;

template ZImg ZImg::castTo<uint32_t>() const;

template ZImg ZImg::castTo<uint64_t>() const;

template ZImg ZImg::castTo<int8_t>() const;

template ZImg ZImg::castTo<int16_t>() const;

template ZImg ZImg::castTo<int32_t>() const;

template ZImg ZImg::castTo<int64_t>() const;

template ZImg ZImg::castTo<float>() const;

template ZImg ZImg::castTo<double>() const;

[[nodiscard]]
ZImg ZImg::castTo(VoxelFormat vf, size_t bytePerVoxel)
{
  if (voxelFormat() == vf && bytesPerVoxel() == bytePerVoxel) {
    return *this;
  }
  if ((vf == VoxelFormat::Float && bytePerVoxel != 4 && bytePerVoxel != 8) ||
      (vf == VoxelFormat::Signed && bytePerVoxel != 1 && bytePerVoxel != 2 && bytePerVoxel != 4
       && bytePerVoxel != 8) ||
      ((vf == VoxelFormat::Unsigned) && bytePerVoxel != 1 && bytePerVoxel != 2 && bytePerVoxel != 4
       && bytePerVoxel != 8)) {
    throw ZImgException(QString("Invalid combination of voxel format %1 and bytesPerVoxel %2")
                          .arg(enumToString(vf)).arg(bytePerVoxel));
  }
  ZImgInfo info = m_info;
  info.voxelFormat = vf;
  info.bytesPerVoxel = bytePerVoxel;
  info.validBitCount = 0;
  ZImg res(info);

  IMG_TYPED_CALL_2TYPE(cast_Impl, m_info, info, res)

  return res;
}

ZImg ZImg::resized(size_t desWidth, size_t desHeight, size_t desDepth,
                   Interpolant interpolant, bool antialiasing, bool antialiasingForNearest) const
{
  CHECK(desWidth > 0 && desHeight > 0 && desDepth > 0);

  ZImg res;

  if (desWidth == width() && desHeight == height() && desDepth == depth()) {
    res = *this;
    return res;
  }

  ZImgInfo info = m_info;
  info.voxelSizeX *= double(info.width) / desWidth;
  info.voxelSizeY *= double(info.height) / desHeight;
  info.voxelSizeZ *= double(info.depth) / desDepth;
  info.width = desWidth;
  info.height = desHeight;
  info.depth = desDepth;

  res = ZImg(info);
  IMG_TYPED_CALL(resize_Impl, m_info, res, interpolant, antialiasing, antialiasingForNearest)

  return res;
}

ZImg ZImg::zoomed(double scaleX, double scaleY, double scaleZ, Interpolant interpolant,
                  bool antialiasing, bool antialiasingForNearest) const
{
  size_t desWidth = std::ceil(width() * scaleX);
  size_t desHeight = std::ceil(height() * scaleY);
  size_t desDepth = std::ceil(depth() * scaleZ);
  return resized(desWidth, desHeight, desDepth, interpolant, antialiasing, antialiasingForNearest);
}

ZImg ZImg::blockDownsampled(size_t blockWidth, size_t blockHeight, size_t blockDepth, ImgMergeMode mode) const
{
  ZImgInfo info = m_info;
  info.voxelSizeX *= blockWidth;
  info.voxelSizeY *= blockHeight;
  info.voxelSizeZ *= blockDepth;
  info.width = (m_info.width + blockWidth - 1) / blockWidth;
  info.height = (m_info.height + blockHeight - 1) / blockHeight;
  info.depth = (m_info.depth + blockDepth - 1) / blockDepth;

  if (mode == ImgMergeMode::Interpolation) {
    return resized(info.width, info.height, info.depth);
  } else {
    ZImg res(info);

    if (res.isEmpty()) {
      return res;
    }

    IMG_TYPED_CALL(blockDownsampled_Impl, m_info, res, blockWidth, blockHeight, blockDepth, mode)

    return res;
  }
}

ZImg& ZImg::resize(size_t desWidth, size_t desHeight, size_t desDepth, Interpolant interpolant,
                   bool antialiasing, bool antialiasingForNearest)
{
  if (width() == desWidth && height() == desHeight && depth() == desDepth) {
    return *this;
  }
  ZImg res = resized(desWidth, desHeight, desDepth, interpolant, antialiasing, antialiasingForNearest);
  swap(res);
  return *this;
}

ZImg& ZImg::zoom(double scaleX, double scaleY, double scaleZ, Interpolant interpolant,
                 bool antialiasing, bool antialiasingForNearest)
{
  if (scaleX == 1 && scaleY == 1 && scaleZ == 1) {
    return *this;
  }
  ZImg res = zoomed(scaleX, scaleY, scaleZ, interpolant, antialiasing, antialiasingForNearest);
  swap(res);
  return *this;
}

ZImg& ZImg::blockDownsample(size_t blockWidth, size_t blockHeight, size_t blockDepth, ImgMergeMode mode)
{
  if (blockWidth == 1 && blockHeight == 1 && blockDepth == 1) {
    return *this;
  }
  ZImg res = blockDownsampled(blockWidth, blockHeight, blockDepth, mode);
  swap(res);
  return *this;
}

ZImg& ZImg::flip(Dimension dim)
{
  if (isEmpty()) {
    return *this;
  }
  /*if (dim == Dimension::L) { // flip locations
    size_t j = m_data.size() - 1;
    for (size_t i=0; i<m_data.size()/2; ++i, --j) {
      m_data[i].swap(m_data[j]);
    }
  } else */
  if (dim == Dimension::T) { // flip times
    std::reverse(m_data.begin(), m_data.end());
  } else if (dim == Dimension::C) { // flip channels
    if (numChannels() > 1) {
      std::vector<uint8_t, boost::alignment::aligned_allocator<uint8_t, 64>> buf(channelByteNumber());
      for (size_t t = 0; t < numTimes(); ++t) {
        size_t j = numChannels() - 1;
        for (size_t i = 0; i < numChannels() / 2; ++i, --j) {
          // swap channel i,j
          std::memcpy(buf.data(), channelData(i, t), channelByteNumber());
          std::memcpy(channelData(i, t), channelData(j, t), channelByteNumber());
          std::memcpy(channelData(j, t), buf.data(), channelByteNumber());
        }
      }
    }
  } else if (dim < Dimension::C) {
    IMG_TYPED_CALL(flip_Impl, m_info, dim)
  }
  return *this;
}

ZImg& ZImg::reflect()
{
  if (isEmpty()) {
    return *this;
  }
  // reflect time
  flip(Dimension::T);
  // reflect others
  IMG_TYPED_CALL(reflect_Impl, m_info)
  return *this;
}

ZImg ZImg::cumulativeSum(Dimension dim) const
{
  ZImg res = *this;
  if (dim == Dimension::T) {
    for (size_t t = 1; t < numTimes(); ++t) {
      ZImg currentTime = res.createView(-1, t);
      ZImg lastTime = res.createView(-1, t - 1);
      currentTime += lastTime;
    }
  } else if (dim == Dimension::C) {
    for (size_t c = 1; c < numChannels(); ++c) {
      ZImg currentCh = res.createView(c, -1);
      ZImg lastCh = res.createView(c - 1, -1);
      currentCh += lastCh;
    }
  } else if (dim < Dimension::C) {
    IMG_TYPED_CALL(cumulativeSum_Impl, m_info, res, dim)
  }
  return res;
}

ZImg ZImg::blockSum(size_t twidth, size_t theight, size_t tdepth) const
{
  if (twidth == 0 || theight == 0 || tdepth == 0) {
    throw ZImgException(
      QString("wrong template size input for blockSum: %1, %2, %3)").arg(twidth).arg(theight).arg(tdepth));
  }
  ZImg res;
  if (isEmpty()) {
    return res;
  }
  ZImgInfo info = m_info;
  info.width += twidth - 1;
  info.height += theight - 1;
  info.depth += tdepth - 1;
  res = ZImg(info);
  if (res.voxelFormat() == VoxelFormat::Signed) { // default signed image voxel is negative
    res.fill(0);
  }

  IMG_TYPED_CALL(blockSum_Impl, m_info, res, twidth, theight, tdepth)

  return res;
}

ZImg ZImg::blockSumPart(size_t twidth, size_t theight, size_t tdepth, size_t xStart, size_t xEnd,
                        size_t yStart, size_t yEnd, size_t zStart, size_t zEnd) const
{
  if (twidth == 0 || theight == 0 || tdepth == 0) {
    throw ZImgException(QString("wrong template size input for blockSumPart: %1, %2, %3)").
      arg(twidth).arg(theight).arg(tdepth));
  }
  if (xEnd <= xStart || xEnd > (m_info.width + twidth - 1) ||
      yEnd <= yStart || yEnd > (m_info.height + theight - 1) ||
      zEnd <= zStart || zEnd > (m_info.depth + tdepth - 1)) {
    throw ZImgException(QString("wrong region for blockSumPart: %1:%2, %3:%4, %5:%6")
                          .arg(xStart).arg(xEnd).arg(yStart).arg(yEnd).arg(zStart).arg(zEnd));
  }
  ZImg res;
  if (isEmpty()) {
    return res;
  }
  ZImgInfo info = m_info;
  info.width = xEnd - xStart;
  info.height = yEnd - yStart;
  info.depth = zEnd - zStart;
  res = ZImg(info);
  if (res.voxelFormat() == VoxelFormat::Signed) { // default signed image voxel is negative
    res.fill(0);
  }

  IMG_TYPED_CALL(blockSumPart_Impl, m_info, res, twidth, theight, tdepth, xStart,
                 yStart, zStart)

  return res;
}

ZImg& ZImg::operator+=(const ZImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZImgException(QString("img addition requires same size img as input: this <%1>, other <%2>")
                          .arg(m_info.toQString()).arg(rhs.info().toQString()));
  }
  IMG_TYPED_CALL_2TYPE(addImg_Impl, m_info, rhs.info(), rhs)
  return *this;
}

ZImg& ZImg::operator-=(const ZImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZImgException(QString("img subtraction requires same size img as input: this <%1>, other <%2>")
                          .arg(m_info.toQString()).arg(rhs.info().toQString()));
  }
  IMG_TYPED_CALL_2TYPE(subImg_Impl, m_info, rhs.info(), rhs)
  return *this;
}

ZImg& ZImg::operator*=(const ZImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZImgException(QString("img multiplies requires same size img as input: this <%1>, other <%2>")
                          .arg(m_info.toQString()).arg(rhs.info().toQString()));
  }
  IMG_TYPED_CALL_2TYPE(mulImg_Impl, m_info, rhs.info(), rhs)
  return *this;
}

ZImg& ZImg::operator/=(const ZImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZImgException(QString("img divides requires same size img as input: this <%1>, other <%2>")
                          .arg(m_info.toQString()).arg(rhs.info().toQString()));
  }
  IMG_TYPED_CALL_2TYPE(divImg_Impl, m_info, rhs.info(), rhs)
  return *this;
}

ZImg& ZImg::secureDivideBy(const ZImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZImgException(QString("img divides requires same size img as input: this <%1>, other <%2>")
                          .arg(m_info.toQString()).arg(rhs.info().toQString()));
  }
  IMG_TYPED_CALL_2TYPE(secureDivImg_Impl, m_info, rhs.info(), rhs)
  return *this;
}

bool ZImg::operator==(const ZImg& other) const
{
  if (isSameType(other) && isSameSize(other)) {
    for (size_t t = 0; t < numTimes(); ++t) {
      const uint8_t* data = timeData(t);
      const uint8_t* otherData = other.timeData(t);
      if (data != otherData && memcmp(data, otherData, timeByteNumber()) != 0) {
        return false;
      }
    }
    return true;
  }
  return false;
}

ZVoxelCoordinate ZImg::indexToCoord(int64_t idx, const ZImgInfo& info)
{
  if (info.isEmpty()) {
    throw ZImgException(QString("Can not convert index to coord with empty img info <%1>").arg(info.toQString()));
  }
  ZVoxelCoordinate res;
  //  res.l = idx >= 0 ? (idx / info.locationVoxelNumber()) : (- 1 - ((-idx-1) / info.locationVoxelNumber()));
  //  idx -= res.l * (int64_t)info.locationVoxelNumber();  //idx is positive now
  res.t = idx >= 0 ? (idx / info.timeVoxelNumber()) : (-1 - ((-idx - 1) / info.timeVoxelNumber()));
  idx -= res.t * static_cast<int64_t>(info.timeVoxelNumber());  //idx is positive now
  //  res.t = idx / info.timeVoxelNumber();
  //  idx -= res.t * info.timeVoxelNumber();
  res.c = idx / info.channelVoxelNumber();
  idx -= res.c * info.channelVoxelNumber();
  res.z = idx / info.planeVoxelNumber();
  idx -= res.z * info.planeVoxelNumber();
  res.y = idx / info.rowVoxelNumber();
  idx -= res.y * info.rowVoxelNumber();
  res.x = idx;
  return res;
}

int64_t ZImg::coordToIndex(const ZVoxelCoordinate& coord, const ZImgInfo& info)
{
  return coord.t * static_cast<int64_t>(info.timeVoxelNumber()) +
         coord.c * static_cast<int64_t>(info.channelVoxelNumber()) +
         coord.z * static_cast<int64_t>(info.planeVoxelNumber()) +
         coord.y * static_cast<int64_t>(info.rowVoxelNumber()) +
         coord.x;
}

ZImg& ZImg::correctPreMultipliedColor()
{
  if (numChannels() > 1) {
    if (voxelFormat() == VoxelFormat::Float) {
      ZImg divImg = createView(numChannels() - 1);
      for (size_t c = 0; c < numChannels() - 1; ++c) {
        ZImg chImg = createView(c);
        chImg.secureDivideBy(divImg);
      }
    } else {
      ZImg divImg = createView(numChannels() - 1).convertTo<double>();
      for (size_t c = 0; c < numChannels() - 1; ++c) {
        ZImg chImg = createView(c);
        chImg.secureDivideBy(divImg);
      }
    }
  }
  return *this;
}

void ZImg::clearData()
{
  if (!m_ownData) {
    m_data.clear();
    return;
  }

  for (auto d : m_data) {
    boost::alignment::aligned_free(d);
  }
  m_data.clear();
}

void ZImg::wrapCoord(ZVoxelCoordinate& coord, PadOption padOption) const
{
  wrapCoordToImage(&coord.x, &m_info.width, ZImgInfo::numDimensions(), padOption);
}

void ZImg::checkConnInput(size_t& conn) const
{
  if (conn != 4 && conn != 8 && conn != 6 && conn != 18 && conn != 26) {
    throw ZImgException(QString("invalid conn input: %1").arg(conn));
  }
  if (is2DImg() && conn != 4 && conn != 8) {
    if (conn == 6) {
      conn = 4;
    } else {
      conn = 8;
    }
  }
}

template<typename TVoxel>
void ZImg::cropWithPad_Impl(ZImg& res, const ZVoxelCoordinate& startCoord, const ZVoxelCoordinate& endCoord,
                            PadOption padOption, TVoxel padValue) const
{
  ZVoxelCoordinate coord;
  for (coord.t = startCoord.t; coord.t < endCoord.t; ++coord.t) {
    for (coord.c = startCoord.c; coord.c < endCoord.c; ++coord.c) {
      for (coord.z = startCoord.z; coord.z < endCoord.z; ++coord.z) {
        for (coord.y = startCoord.y; coord.y < endCoord.y; ++coord.y) {
          for (coord.x = startCoord.x; coord.x < endCoord.x; ++coord.x) {
            *(res.data<TVoxel>(coord.x - startCoord.x, coord.y - startCoord.y, coord.z - startCoord.z,
                               coord.c - startCoord.c, coord.t - startCoord.t)) =
              valueWithPad_Impl<TVoxel>(coord, padOption, padValue);
          }
        }
      }
    }
  }
}

template void
ZImg::cropWithPad_Impl<uint8_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, uint8_t) const;

template void
ZImg::cropWithPad_Impl<uint16_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, uint16_t) const;

template void
ZImg::cropWithPad_Impl<uint32_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, uint32_t) const;

template void
ZImg::cropWithPad_Impl<uint64_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, uint64_t) const;

template void
ZImg::cropWithPad_Impl<int8_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, int8_t) const;

template void
ZImg::cropWithPad_Impl<int16_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, int16_t) const;

template void
ZImg::cropWithPad_Impl<int32_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, int32_t) const;

template void
ZImg::cropWithPad_Impl<int64_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, int64_t) const;

template void
ZImg::cropWithPad_Impl<float>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, float) const;

template void
ZImg::cropWithPad_Impl<double>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, double) const;

template<typename TVoxel>
void ZImg::fill_Impl(TVoxel value)
{
  for (size_t t = 0; t < m_info.numTimes; ++t) {
    auto data = timeData<TVoxel>(t);
    std::fill(data, data + m_info.timeVoxelNumber(), value);
  }
}

template void ZImg::fill_Impl<uint8_t>(uint8_t);

template void ZImg::fill_Impl<uint16_t>(uint16_t);

template void ZImg::fill_Impl<uint32_t>(uint32_t);

template void ZImg::fill_Impl<uint64_t>(uint64_t);

template void ZImg::fill_Impl<int8_t>(int8_t);

template void ZImg::fill_Impl<int16_t>(int16_t);

template void ZImg::fill_Impl<int32_t>(int32_t);

template void ZImg::fill_Impl<int64_t>(int64_t);

template void ZImg::fill_Impl<float>(float);

template void ZImg::fill_Impl<double>(double);

template<typename TVoxel, typename TVoxelImg>
void ZImg::pasteImg_Impl(const ZImg& img, const ZVoxelCoordinate& start)
{
  using TCoordinate = ZVoxelCoordinate::value_type;
  size_t desX = std::max(start.x, TCoordinate(0));
  size_t srcX = desX - start.x;
  size_t desXEnd = std::min(start.x + static_cast<TCoordinate>(img.width()), static_cast<TCoordinate>(width()));
  size_t rowVoxelNumber = desXEnd - desX;

  for (TCoordinate desT = std::max(start.t, TCoordinate(0));
       desT < std::min(start.t + static_cast<TCoordinate>(img.numTimes()),
                       static_cast<TCoordinate>(numTimes()));
       ++desT) {
    size_t srcT = desT - start.t;
    for (TCoordinate desC = std::max(start.c, TCoordinate(0));
         desC < std::min(start.c + static_cast<TCoordinate>(img.numChannels()),
                         static_cast<TCoordinate>(numChannels()));
         ++desC) {
      size_t srcC = desC - start.c;
      for (TCoordinate desZ = std::max(start.z, TCoordinate(0));
           desZ < std::min(start.z + static_cast<TCoordinate>(img.depth()),
                           static_cast<TCoordinate>(depth()));
           ++desZ) {
        size_t srcZ = desZ - start.z;
        for (TCoordinate desY = std::max(start.y, TCoordinate(0));
             desY < std::min(start.y + static_cast<TCoordinate>(img.height()),
                             static_cast<TCoordinate>(height()));
             ++desY) {
          size_t srcY = desY - start.y;

          auto* desData = data<TVoxel>(desX, desY, desZ, desC, desT);
          auto srcData = img.data<TVoxelImg>(srcX, srcY, srcZ, srcC, srcT);
          for (size_t v = 0; v < rowVoxelNumber; ++v) {
            desData[v] = static_cast<TVoxel>(srcData[v]);
          }
        }
      }
    }
  }
}

template<typename TVoxel, typename TVoxelImg>
void ZImg::pasteImgMax_Impl(const ZImg& img, const ZVoxelCoordinate& start)
{
  using TCoordinate = ZVoxelCoordinate::value_type;
  size_t desX = std::max(start.x, TCoordinate(0));
  size_t srcX = desX - start.x;
  size_t desXEnd = std::min(start.x + static_cast<TCoordinate>(img.width()), static_cast<TCoordinate>(width()));
  size_t rowVoxelNumber = desXEnd - desX;

  for (TCoordinate desT = std::max(start.t, TCoordinate(0));
       desT <
       std::min(start.t + static_cast<TCoordinate>(img.numTimes()), static_cast<TCoordinate>(numTimes())); ++desT) {
    size_t srcT = desT - start.t;
    for (TCoordinate desC = std::max(start.c, TCoordinate(0));
         desC < std::min(start.c + static_cast<TCoordinate>(img.numChannels()),
                         static_cast<TCoordinate>(numChannels())); ++desC) {
      size_t srcC = desC - start.c;
      for (TCoordinate desZ = std::max(start.z, TCoordinate(0));
           desZ <
           std::min(start.z + static_cast<TCoordinate>(img.depth()), static_cast<TCoordinate>(depth())); ++desZ) {
        size_t srcZ = desZ - start.z;
        for (TCoordinate desY = std::max(start.y, TCoordinate(0));
             desY <
             std::min(start.y + static_cast<TCoordinate>(img.height()), static_cast<TCoordinate>(height())); ++desY) {
          size_t srcY = desY - start.y;

          auto desData = data<TVoxel>(desX, desY, desZ, desC, desT);
          auto srcData = img.data<TVoxelImg>(srcX, srcY, srcZ, srcC, srcT);
          for (size_t v = 0; v < rowVoxelNumber; ++v) {
            desData[v] = std::max(static_cast<TVoxel>(srcData[v]), desData[v]);
          }
        }
      }
    }
  }
}

template<typename TVoxel>
ZImg ZImg::combine_Impl(const std::vector<const ZImg*>& imgs, ImgMergeMode mode)
{
  if (mode == ImgMergeMode::First) {
    ZImg res(*imgs[0]);
    return res;
  }

  if (mode == ImgMergeMode::Min) {
    ZImg res(*imgs[0]);
    for (size_t i = 1; i < imgs.size(); ++i) {
      const ZImg* img = imgs[i];
      for (size_t t = 0; t < res.numTimes(); ++t) {
        auto resData = res.timeData<TVoxel>(t);
        auto srcData = img->timeData<TVoxel>(t);
        for (size_t v = 0; v < res.timeVoxelNumber(); ++v) {
          resData[v] = std::min(resData[v], srcData[v]);
        }
      }
    }
    return res;
  }

  if (mode == ImgMergeMode::Max) {
    ZImg res(*imgs[0]);
    for (size_t i = 1; i < imgs.size(); ++i) {
      const ZImg* img = imgs[i];
      for (size_t t = 0; t < res.numTimes(); ++t) {
        auto resData = res.timeData<TVoxel>(t);
        auto srcData = img->timeData<TVoxel>(t);
        for (size_t v = 0; v < res.timeVoxelNumber(); ++v) {
          resData[v] = std::max(resData[v], srcData[v]);
        }
      }
    }
    return res;
  }

  if (mode == ImgMergeMode::Mean) {
    ZImg res(imgs[0]->info());
    std::vector<TVoxel> buf(imgs.size());

    for (size_t t = 0; t < res.numTimes(); ++t) {
      auto resData = res.timeData<TVoxel>(t);
      for (size_t v = 0; v < res.timeVoxelNumber(); ++v) {
        for (size_t i = 0; i < imgs.size(); ++i) {
          auto srcData = imgs[i]->timeData<TVoxel>(t);
          buf[i] = srcData[v];
        }
        resData[v] = static_cast<TVoxel>(mean(buf.begin(), buf.end()));
      }
    }
    return res;
  }

  if (mode == ImgMergeMode::Median) {
    ZImg res(imgs[0]->info());
    std::vector<TVoxel> buf(imgs.size());

    for (size_t t = 0; t < res.numTimes(); ++t) {
      auto resData = res.timeData<TVoxel>(t);
      for (size_t v = 0; v < res.timeVoxelNumber(); ++v) {
        for (size_t i = 0; i < imgs.size(); ++i) {
          auto srcData = imgs[i]->timeData<TVoxel>(t);
          buf[i] = srcData[v];
        }
        resData[v] = static_cast<TVoxel>(medianInPlace(buf.begin(), buf.end()));
      }
    }
    return res;
  }

  return ZImg();
}

template ZImg ZImg::combine_Impl<uint8_t>(const std::vector<const ZImg*>&, ImgMergeMode);

template ZImg ZImg::combine_Impl<uint16_t>(const std::vector<const ZImg*>&, ImgMergeMode);

template ZImg ZImg::combine_Impl<uint32_t>(const std::vector<const ZImg*>&, ImgMergeMode);

template ZImg ZImg::combine_Impl<uint64_t>(const std::vector<const ZImg*>&, ImgMergeMode);

template ZImg ZImg::combine_Impl<int8_t>(const std::vector<const ZImg*>&, ImgMergeMode);

template ZImg ZImg::combine_Impl<int16_t>(const std::vector<const ZImg*>&, ImgMergeMode);

template ZImg ZImg::combine_Impl<int32_t>(const std::vector<const ZImg*>&, ImgMergeMode);

template ZImg ZImg::combine_Impl<int64_t>(const std::vector<const ZImg*>&, ImgMergeMode);

template ZImg ZImg::combine_Impl<float>(const std::vector<const ZImg*>&, ImgMergeMode);

template ZImg ZImg::combine_Impl<double>(const std::vector<const ZImg*>&, ImgMergeMode);

template<typename TVoxel>
ZImg& ZImg::normalize_Impl()
{
  TVoxel minV = 0;
  TVoxel maxV = 0;
  computeMinMax(minV, maxV);
  return normalize(minV, maxV);
}

template<typename TVoxel, typename TDesVoxel>
void ZImg::cast_Impl(ZImg& res) const
{
  for (size_t t = 0; t < numTimes(); ++t) {
    auto srcData = timeData<TVoxel>(t);
    auto desData = res.timeData<TDesVoxel>(t);
    for (size_t v = 0; v < timeVoxelNumber(); ++v) {
      desData[v] = static_cast<TDesVoxel>(srcData[v]);
    }
  }
}

template<typename TVoxel>
void ZImg::resize_Impl(ZImg& res, Interpolant interpolant, bool antialiasing, bool antialiasingForNearest) const
{
  for (size_t t = 0; t < numTimes(); ++t) {
    for (size_t c = 0; c < numChannels(); ++c) {
      if (res.depth() == depth()) {
        for (size_t z = 0; z < depth(); ++z) {
          //ZBenchTimer bt;
          //bt.start();
          image2DResize(planeData<TVoxel>(z, c, t), width(), height(),
                        res.planeData<TVoxel>(z, c, t), res.width(), res.height(),
                        interpolant, antialiasing, antialiasingForNearest);
          //bt.stopAndPrint();
          //          bt.reset();
          //          bt.start();
          //          image2DResize_Old(planeData<TVoxel>(z,c,t), width(), height(),
          //                            res.planeData<TVoxel>(z,c,t), res.width(), res.height(),
          //                            interpolant);
          //          bt.stopAndPrint();
        }
      } else {
        image3DResize(channelData<TVoxel>(c, t), width(), height(), depth(),
                      res.channelData<TVoxel>(c, t), res.width(), res.height(), res.depth(),
                      interpolant, antialiasing, antialiasingForNearest);
      }
    }
  }
}

template<typename TVoxel>
void
ZImg::blockDownsampled_Impl(ZImg& res, size_t blockWidth, size_t blockHeight, size_t blockDepth,
                            ImgMergeMode mode) const
{
  if (mode == ImgMergeMode::Mean) {
    for (size_t t = 0; t < res.numTimes(); ++t) {
      for (size_t c = 0; c < res.numChannels(); ++c) {
        auto resData = res.channelData<TVoxel>(c, t);
        size_t resOffset = 0;
        for (size_t z = 0; z < res.depth(); ++z) {
          size_t zStart = z * blockDepth;
          size_t zEnd = std::min((z + 1) * blockDepth, depth());
          size_t zSpan = zEnd - zStart;
          for (size_t y = 0; y < res.height(); ++y) {
            size_t yStart = y * blockHeight;
            size_t yEnd = std::min((y + 1) * blockHeight, height());
            size_t yzSpan = (yEnd - yStart) * zSpan;
            for (size_t x = 0; x < res.width(); ++x) {
              size_t xStart = x * blockWidth;
              size_t xEnd = std::min((x + 1) * blockWidth, width());
              size_t numVoxel = (xEnd - xStart) * yzSpan;
              auto srcData = data<TVoxel>(xStart, yStart, zStart, c, t);
              size_t srcOffset = 0;
              double sum = 0;
              for (size_t mz = zStart; mz < zEnd; ++mz) {
                for (size_t my = yStart; my < yEnd; ++my) {
                  for (size_t mx = xStart; mx < xEnd; ++mx) {
                    sum += srcData[srcOffset++];
                  }
                  srcOffset += rowVoxelNumber() - (xEnd - xStart);
                }
                srcOffset += planeVoxelNumber() - (yEnd - yStart) * rowVoxelNumber();
              }
              resData[resOffset++] = static_cast<TVoxel>(sum / numVoxel);
            }
          }
        }
      }
    }
  } else if (mode == ImgMergeMode::Median) {
    std::vector<TVoxel> buf(blockWidth * blockHeight * blockDepth);
    for (size_t t = 0; t < res.numTimes(); ++t) {
      for (size_t c = 0; c < res.numChannels(); ++c) {
        auto resData = res.channelData<TVoxel>(c, t);
        size_t resOffset = 0;
        for (size_t z = 0; z < res.depth(); ++z) {
          size_t zStart = z * blockDepth;
          size_t zEnd = std::min((z + 1) * blockDepth, depth());
          for (size_t y = 0; y < res.height(); ++y) {
            size_t yStart = y * blockHeight;
            size_t yEnd = std::min((y + 1) * blockHeight, height());
            for (size_t x = 0; x < res.width(); ++x) {
              size_t xStart = x * blockWidth;
              size_t xEnd = std::min((x + 1) * blockWidth, width());
              auto srcData = data<TVoxel>(xStart, yStart, zStart, c, t);
              size_t srcOffset = 0;
              size_t bufIdx = 0;
              for (size_t mz = zStart; mz < zEnd; ++mz) {
                for (size_t my = yStart; my < yEnd; ++my) {
                  for (size_t mx = xStart; mx < xEnd; ++mx) {
                    buf[bufIdx++] = srcData[srcOffset++];
                  }
                  srcOffset += rowVoxelNumber() - (xEnd - xStart);
                }
                srcOffset += planeVoxelNumber() - (yEnd - yStart) * rowVoxelNumber();
              }
              resData[resOffset++] = static_cast<TVoxel>(medianInPlace(buf.begin(), buf.begin() + bufIdx));
            }
          }
        }
      }
    }
  } else if (mode == ImgMergeMode::Min) {
    for (size_t t = 0; t < res.numTimes(); ++t) {
      for (size_t c = 0; c < res.numChannels(); ++c) {
        auto resData = res.channelData<TVoxel>(c, t);
        size_t resOffset = 0;
        for (size_t z = 0; z < res.depth(); ++z) {
          size_t zStart = z * blockDepth;
          size_t zEnd = std::min((z + 1) * blockDepth, depth());
          for (size_t y = 0; y < res.height(); ++y) {
            size_t yStart = y * blockHeight;
            size_t yEnd = std::min((y + 1) * blockHeight, height());
            for (size_t x = 0; x < res.width(); ++x) {
              size_t xStart = x * blockWidth;
              size_t xEnd = std::min((x + 1) * blockWidth, width());
              auto srcData = data<TVoxel>(xStart, yStart, zStart, c, t);
              size_t srcOffset = 0;
              for (size_t mz = zStart; mz < zEnd; ++mz) {
                for (size_t my = yStart; my < yEnd; ++my) {
                  for (size_t mx = xStart; mx < xEnd; ++mx) {
                    resData[resOffset] = std::min(srcData[srcOffset++], resData[resOffset]);
                  }
                  srcOffset += rowVoxelNumber() - (xEnd - xStart);
                }
                srcOffset += planeVoxelNumber() - (yEnd - yStart) * rowVoxelNumber();
              }
              ++resOffset;
            }
          }
        }
      }
    }
  } else if (mode == ImgMergeMode::Max) {
    for (size_t t = 0; t < res.numTimes(); ++t) {
      for (size_t c = 0; c < res.numChannels(); ++c) {
        auto resData = res.channelData<TVoxel>(c, t);
        size_t resOffset = 0;
        for (size_t z = 0; z < res.depth(); ++z) {
          size_t zStart = z * blockDepth;
          size_t zEnd = std::min((z + 1) * blockDepth, depth());
          for (size_t y = 0; y < res.height(); ++y) {
            size_t yStart = y * blockHeight;
            size_t yEnd = std::min((y + 1) * blockHeight, height());
            for (size_t x = 0; x < res.width(); ++x) {
              size_t xStart = x * blockWidth;
              size_t xEnd = std::min((x + 1) * blockWidth, width());
              auto srcData = data<TVoxel>(xStart, yStart, zStart, c, t);
              size_t srcOffset = 0;
              for (size_t mz = zStart; mz < zEnd; ++mz) {
                for (size_t my = yStart; my < yEnd; ++my) {
                  for (size_t mx = xStart; mx < xEnd; ++mx) {
                    resData[resOffset] = std::max(srcData[srcOffset++], resData[resOffset]);
                  }
                  srcOffset += rowVoxelNumber() - (xEnd - xStart);
                }
                srcOffset += planeVoxelNumber() - (yEnd - yStart) * rowVoxelNumber();
              }
              ++resOffset;
            }
          }
        }
      }
    }
  } else if (mode == ImgMergeMode::First) {
    for (size_t t = 0; t < res.numTimes(); ++t) {
      for (size_t c = 0; c < res.numChannels(); ++c) {
        auto resData = res.channelData<TVoxel>(c, t);
        size_t resOffset = 0;
        for (size_t z = 0; z < res.depth(); ++z) {
          size_t zStart = z * blockDepth;
          for (size_t y = 0; y < res.height(); ++y) {
            size_t yStart = y * blockHeight;
            for (size_t x = 0; x < res.width(); ++x) {
              size_t xStart = x * blockWidth;
              auto srcData = data<TVoxel>(xStart, yStart, zStart, c, t);
              resData[resOffset] = srcData[0];
              ++resOffset;
            }
          }
        }
      }
    }
  }
}

template<typename TVoxel, typename TValue>
void ZImg::computeMinMax_Impl(TValue& minV, TValue& maxV) const
{
  if (isEmpty()) {
    minV = 0;
    maxV = 0;
    return;
  }
  for (size_t t = 0; t < numTimes(); ++t) {
    auto data = timeData<TVoxel>(t);
    std::pair<const TVoxel*, const TVoxel*> res = minMaxElement(data, data + timeVoxelNumber(), true);
    if (t == 0) {
      minV = static_cast<TValue>(*res.first);
      maxV = static_cast<TValue>(*res.second);
    } else {
      minV = std::min(minV, static_cast<TValue>(*res.first));
      maxV = std::max(maxV, static_cast<TValue>(*res.second));
    }
  }
}

template<typename TVoxel, typename TVoxelRhs>
void ZImg::addImg_Impl(const ZImg& rhs)
{
  for (size_t t = 0; t < numTimes(); ++t) {
    auto data = timeData<TVoxel>(t);
    auto rhsData = rhs.timeData<TVoxelRhs>(t);
    saturate_add(data, rhsData, timeVoxelNumber(), data);
  }
}

template<typename TVoxel, typename TVoxelRhs>
void ZImg::subImg_Impl(const ZImg& rhs)
{
  for (size_t t = 0; t < numTimes(); ++t) {
    auto data = timeData<TVoxel>(t);
    auto rhsData = rhs.timeData<TVoxelRhs>(t);
    saturate_sub(data, rhsData, timeVoxelNumber(), data);
  }
}

template<typename TVoxel, typename TVoxelRhs>
void ZImg::mulImg_Impl(const ZImg& rhs)
{
  for (size_t t = 0; t < numTimes(); ++t) {
    auto data = timeData<TVoxel>(t);
    auto rhsData = rhs.timeData<TVoxelRhs>(t);
    saturate_mul(data, rhsData, timeVoxelNumber(), data);
  }
}

template<typename TVoxel, typename TVoxelRhs>
void ZImg::divImg_Impl(const ZImg& rhs)
{
  for (size_t t = 0; t < numTimes(); ++t) {
    auto data = timeData<TVoxel>(t);
    auto rhsData = rhs.timeData<TVoxelRhs>(t);
    saturate_div(data, rhsData, timeVoxelNumber(), data);
  }
}

template<typename TVoxel, typename TVoxelRhs>
void ZImg::secureDivImg_Impl(const ZImg& rhs)
{
  for (size_t t = 0; t < numTimes(); ++t) {
    auto data = timeData<TVoxel>(t);
    auto rhsData = rhs.timeData<TVoxelRhs>(t);
    saturate_div_secure(data, rhsData, timeVoxelNumber(), data);
  }
}

template<typename TVoxel>
void ZImg::histogram_Impl(std::vector<size_t>& res, TVoxel minData, TVoxel maxData) const
{
  if (maxData <= minData) {
    throw ZImgException(QString("Invalid histogram range %1:%2").arg(minData).arg(maxData));
  }

  if constexpr (std::is_floating_point_v<std::remove_reference_t<TVoxel>>) {
    double scale = res.size() / (maxData - minData);
    for (size_t t = 0; t < numTimes(); ++t) {
      const auto* data = timeData<TVoxel>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        if (data[v] >= minData && data[v] <= maxData) {
          size_t idx = (data[v] - minData) * scale;
          if (idx == res.size()) { idx = res.size() - 1; }  // only maxData map to index that out of bound
          res[idx] += 1;
        }
      }
    }
  } else {
    size_t numData = maxData - minData + 1_usize;
    if (numData == res.size()) {
      if (minData == dataRangeMin<TVoxel>() && maxData == dataRangeMax<TVoxel>()) {
        for (size_t t = 0; t < numTimes(); ++t) {
          const auto* data = timeData<TVoxel>(t);
          for (size_t v = 0; v < timeVoxelNumber(); ++v) {
            res[data[v] - minData] += 1;
          }
        }
      } else {
        for (size_t t = 0; t < numTimes(); ++t) {
          const auto* data = timeData<TVoxel>(t);
          for (size_t v = 0; v < timeVoxelNumber(); ++v) {
            if (data[v] >= minData && data[v] <= maxData) {
              res[data[v] - minData] += 1;
            }
          }
        }
      }
    } else {
      double scale = res.size() / (maxData + 1. - minData);
      for (size_t t = 0; t < numTimes(); ++t) {
        const auto* data = timeData<TVoxel>(t);
        for (size_t v = 0; v < timeVoxelNumber(); ++v) {
          if (data[v] >= minData && data[v] <= maxData) {
            size_t idx = (data[v] - minData) * scale;
            res[idx] += 1;
          }
        }
      }
    }
  }
}

//template void ZImg::histogram_Impl<uint8_t>(std::vector<size_t>&, uint8_t, uint8_t) const;
//template void ZImg::histogram_Impl<uint16_t>(std::vector<size_t>&, uint16_t, uint16_t) const;
//template void ZImg::histogram_Impl<uint32_t>(std::vector<size_t>&, uint32_t, uint32_t) const;
//template void ZImg::histogram_Impl<uint64_t>(std::vector<size_t>&, uint64_t, uint64_t) const;
//template void ZImg::histogram_Impl<int8_t>(std::vector<size_t>&, int8_t, int8_t) const;
//template void ZImg::histogram_Impl<int16_t>(std::vector<size_t>&, int16_t, int16_t) const;
//template void ZImg::histogram_Impl<int32_t>(std::vector<size_t>&, int32_t, int32_t) const;
//template void ZImg::histogram_Impl<int64_t>(std::vector<size_t>&, int64_t, int64_t) const;
//template void ZImg::histogram_Impl<float>(std::vector<size_t>&, float, float) const;
//template void ZImg::histogram_Impl<double>(std::vector<size_t>&, double, double) const;

template<typename TVoxel, typename TMaskVoxel>
void ZImg::histogramMask_Impl(std::vector<size_t>& res, TVoxel minData, TVoxel maxData, const ZImg& mask) const
{
  if (maxData < minData) {
    throw ZImgException(QString("Invalid histogram range %1:%2").arg(minData).arg(maxData));
  }

  if constexpr (std::is_floating_point_v<std::remove_reference_t<TVoxel>>) {
    double scale = res.size() / (maxData - minData);
    for (size_t t = 0; t < numTimes(); ++t) {
      const auto* data = timeData<TVoxel>(t);
      const auto* maskData = mask.timeData<TMaskVoxel>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        if (maskData[v] && data[v] >= minData && data[v] <= maxData) {
          size_t idx = (data[v] - minData) * scale;
          if (idx == res.size()) { idx = res.size() - 1; }  // only maxData map to index that out of bound
          res[idx] += 1;
        }
      }
    }
  } else {
    size_t numData = maxData - minData + 1_usize;
    if (numData == res.size()) {
      if (minData == dataRangeMin<TVoxel>() && maxData == dataRangeMax<TVoxel>()) {
        for (size_t t = 0; t < numTimes(); ++t) {
          const auto* data = timeData<TVoxel>(t);
          const auto* maskData = mask.timeData<TMaskVoxel>(t);
          for (size_t v = 0; v < timeVoxelNumber(); ++v) {
            if (maskData[v]) {
              res[data[v] - minData] += 1;
            }
          }
        }
      } else {
        for (size_t t = 0; t < numTimes(); ++t) {
          const auto* data = timeData<TVoxel>(t);
          const auto* maskData = mask.timeData<TMaskVoxel>(t);
          for (size_t v = 0; v < timeVoxelNumber(); ++v) {
            if (maskData[v] && data[v] >= minData && data[v] <= maxData) {
              res[data[v] - minData] += 1;
            }
          }
        }
      }
    } else {
      double scale = res.size() / (maxData + 1. - minData);
      for (size_t t = 0; t < numTimes(); ++t) {
        const auto* data = timeData<TVoxel>(t);
        const auto* maskData = mask.timeData<TMaskVoxel>(t);
        for (size_t v = 0; v < timeVoxelNumber(); ++v) {
          if (maskData[v] && data[v] >= minData && data[v] <= maxData) {
            size_t idx = (data[v] - minData) * scale;
            res[idx] += 1;
          }
        }
      }
    }
  }
}

// only for dim 0, 1, 2
template<typename TVoxel>
void ZImg::flip_Impl(Dimension dim)
{
  if (dim == Dimension::X || dim == Dimension::Y || dim == Dimension::Z) {
    for (size_t t = 0; t < numTimes(); ++t) {
      for (size_t c = 0; c < numChannels(); ++c) {
        auto data = channelData<TVoxel>(c, t);
        image3DFlip(data, width(), height(), depth(), dim);
      }
    }
  }
}

template<typename TVoxel>
void ZImg::reflect_Impl()
{
  for (size_t t = 0; t < numTimes(); ++t) {
    auto data = timeData<TVoxel>(t);
    std::reverse(data, data + timeVoxelNumber());
  }
}

// only for dim 0, 1, 2
template<typename TVoxel>
void ZImg::cumulativeSum_Impl(ZImg& res, Dimension dim) const
{
  if (dim == Dimension::Z) {
    for (size_t t = 0; t < numTimes(); ++t) {
      for (size_t c = 0; c < numChannels(); ++c) {
        for (size_t z = 1; z < depth(); ++z) {
          auto data = res.planeData<TVoxel>(z, c, t);
          auto prevData = res.planeData<TVoxel>(z - 1, c, t);
          saturate_add(data, prevData, planeVoxelNumber(), data);
        }
      }
    }
  } else if (dim == Dimension::Y) {
    for (size_t t = 0; t < numTimes(); ++t) {
      for (size_t c = 0; c < numChannels(); ++c) {
        for (size_t z = 0; z < depth(); ++z) {
          for (size_t y = 1; y < height(); ++y) {
            auto data = res.rowData<TVoxel>(y, z, c, t);
            auto prevData = res.rowData<TVoxel>(y - 1, z, c, t);
            saturate_add(data, prevData, rowVoxelNumber(), data);
          }
        }
      }
    }
  } else if (dim == Dimension::X) {
    for (size_t t = 0; t < numTimes(); ++t) {
      for (size_t c = 0; c < numChannels(); ++c) {
        for (size_t z = 0; z < depth(); ++z) {
          for (size_t y = 0; y < height(); ++y) {
            auto data = res.data<TVoxel>(1, y, z, c, t);
            for (size_t x = 1; x < width(); ++x, ++data) {
              *data = saturate_add(*data, *(data - 1));
            }
          }
        }
      }
    }
  }
}

template<typename TVoxel>
void ZImg::blockSum_Impl(ZImg& res, size_t twidth, size_t theight, size_t tdepth) const
{
  if (twidth == 1) {
    for (size_t t = 0; t < res.numTimes(); ++t) {
      for (size_t c = 0; c < res.numChannels(); ++c) {
        for (size_t z = tdepth - 1; z < res.depth(); ++z) {
          for (size_t y = theight - 1; y < res.height(); ++y) {
            std::memcpy(res.rowData(y, z, c, t),
                        rowData(y - theight + 1, z - tdepth + 1, c, t),
                        res.rowByteNumber());
          }
        }
      }
    }
  } else if (twidth > 1) {  // first dim
    if (twidth <= width()) {
      for (size_t t = 0; t < res.numTimes(); ++t) {
        for (size_t c = 0; c < res.numChannels(); ++c) {
          for (size_t z = tdepth - 1; z < res.depth(); ++z) {
            for (size_t y = theight - 1; y < res.height(); ++y) {
              auto resData = res.rowData<TVoxel>(y, z, c, t);
              auto origData = rowData<TVoxel>(y - theight + 1, z - tdepth + 1, c, t);
              resData[0] = origData[0];
              for (size_t x = 1; x < twidth; ++x) {
                resData[x] = saturate_add(origData[x], resData[x - 1]);
              }
              for (size_t x = twidth; x < width(); ++x) {
                resData[x] = saturate_sub(saturate_add(origData[x], resData[x - 1]), origData[x - twidth]);
              }
              for (size_t x = width(); x < res.width(); ++x) {
                resData[x] = saturate_sub(resData[x - 1], origData[x - twidth]);
              }
            }
          }
        }
      }
    } else {
      for (size_t t = 0; t < res.numTimes(); ++t) {
        for (size_t c = 0; c < res.numChannels(); ++c) {
          for (size_t z = tdepth - 1; z < res.depth(); ++z) {
            for (size_t y = theight - 1; y < res.height(); ++y) {
              auto resData = res.rowData<TVoxel>(y, z, c, t);
              auto origData = rowData<TVoxel>(y - theight + 1, z - tdepth + 1, c, t);
              resData[0] = origData[0];
              for (size_t x = 1; x < width(); ++x) {
                resData[x] = saturate_add(origData[x], resData[x - 1]);
              }
              for (size_t x = width(); x < twidth; ++x) {
                resData[x] = resData[x - 1];
              }
              for (size_t x = twidth; x < res.width(); ++x) {
                resData[x] = saturate_sub(resData[x - 1], origData[x - twidth]);
              }
            }
          }
        }
      }
    }
  }

  if (theight > 1) { // second dim
    size_t rowByteNum = res.rowByteNumber();
    size_t rowVoxelNum = res.rowVoxelNumber();
    size_t dataOffset = (theight - 1) * rowVoxelNum;
    std::vector<TVoxel> buf(rowVoxelNum);
    std::vector<TVoxel> bufRow(rowVoxelNum);
    if (theight <= height()) {
      for (size_t t = 0; t < res.numTimes(); ++t) {
        for (size_t c = 0; c < res.numChannels(); ++c) {
          for (size_t z = tdepth - 1; z < res.depth(); ++z) {
            // first row
            std::memcpy(res.rowData(0, z, c, t),
                        res.rowData(theight - 1, z, c, t),
                        rowByteNum);
            // save to subtract
            std::memcpy(bufRow.data(), res.rowData(0, z, c, t), rowByteNum);
            // other
            for (size_t y = 1; y < theight; ++y) {
              auto resData = res.rowData<TVoxel>(y, z, c, t);
              for (size_t v = 0; v < rowVoxelNum; ++v) {
                resData[v] = saturate_add(resData[v - rowVoxelNum],
                                          resData[v + dataOffset]);
              }
            }
            for (size_t y = theight; y < height(); ++y) {
              auto resData = res.rowData<TVoxel>(y, z, c, t);
              std::memcpy(buf.data(), resData, rowByteNum);
              for (size_t v = 0; v < rowVoxelNum; ++v) {
                resData[v] = saturate_sub(saturate_add(resData[v - rowVoxelNum],
                                                       resData[v + dataOffset]), bufRow[v]);
              }
              std::memcpy(bufRow.data(), buf.data(), rowByteNum);
            }
            for (size_t y = height(); y < res.height(); ++y) {
              auto resData = res.rowData<TVoxel>(y, z, c, t);
              std::memcpy(buf.data(), resData, rowByteNum);
              for (size_t v = 0; v < rowVoxelNum; ++v) {
                resData[v] = saturate_sub(resData[v - rowVoxelNum], bufRow[v]);
              }
              std::memcpy(bufRow.data(), buf.data(), rowByteNum);
            }
          }
        }
      }
    } else {
      for (size_t t = 0; t < res.numTimes(); ++t) {
        for (size_t c = 0; c < res.numChannels(); ++c) {
          for (size_t z = tdepth - 1; z < res.depth(); ++z) {
            // first row
            std::memcpy(res.rowData(0, z, c, t),
                        res.rowData(theight - 1, z, c, t),
                        rowByteNum);
            // save to subtract
            std::memcpy(bufRow.data(), res.rowData(0, z, c, t), rowByteNum);
            // other
            for (size_t y = 1; y < height(); ++y) {
              auto resData = res.rowData<TVoxel>(y, z, c, t);
              for (size_t v = 0; v < rowVoxelNum; ++v) {
                resData[v] = saturate_add(resData[v - rowVoxelNum],
                                          resData[v + dataOffset]);
              }
            }
            for (size_t y = height(); y < theight; ++y) {
              auto resData = res.rowData<TVoxel>(y, z, c, t);
              std::memcpy(resData, resData - rowVoxelNum, rowByteNum);
            }
            for (size_t y = theight; y < res.height(); ++y) {
              auto resData = res.rowData<TVoxel>(y, z, c, t);
              std::memcpy(buf.data(), resData, rowByteNum);
              for (size_t v = 0; v < rowVoxelNum; ++v) {
                resData[v] = saturate_sub(resData[v - rowVoxelNum], bufRow[v]);
              }
              std::memcpy(bufRow.data(), buf.data(), rowByteNum);
            }
          }
        }
      }
    }
  }

  if (tdepth > 1) {
    size_t planeByteNum = res.planeByteNumber();
    size_t planeVoxelNum = res.planeVoxelNumber();
    size_t dataOffset = (tdepth - 1) * planeVoxelNum;
    std::vector<TVoxel> buf(planeVoxelNum);
    std::vector<TVoxel> bufPlane(planeVoxelNum);
    if (tdepth <= depth()) {
      for (size_t t = 0; t < res.numTimes(); ++t) {
        for (size_t c = 0; c < res.numChannels(); ++c) {
          // first plane
          std::memcpy(res.planeData(0, c, t),
                      res.planeData(tdepth - 1, c, t),
                      planeByteNum);
          // save to subtract
          std::memcpy(bufPlane.data(), res.planeData(0, c, t), planeByteNum);
          // other
          for (size_t z = 1; z < tdepth; ++z) {
            auto resData = res.planeData<TVoxel>(z, c, t);
            for (size_t v = 0; v < planeVoxelNum; ++v) {
              resData[v] = saturate_add(resData[v - planeVoxelNum],
                                        resData[v + dataOffset]);
            }
          }
          for (size_t z = tdepth; z < depth(); ++z) {
            auto resData = res.planeData<TVoxel>(z, c, t);
            std::memcpy(buf.data(), resData, planeByteNum);
            for (size_t v = 0; v < planeVoxelNum; ++v) {
              resData[v] = saturate_sub(saturate_add(resData[v - planeVoxelNum],
                                                     resData[v + dataOffset]), bufPlane[v]);
            }
            std::memcpy(bufPlane.data(), buf.data(), planeByteNum);
          }
          for (size_t z = depth(); z < res.depth(); ++z) {
            auto resData = res.planeData<TVoxel>(z, c, t);
            std::memcpy(buf.data(), resData, planeByteNum);
            for (size_t v = 0; v < planeVoxelNum; ++v) {
              resData[v] = saturate_sub(resData[v - planeVoxelNum], bufPlane[v]);
            }
            std::memcpy(bufPlane.data(), buf.data(), planeByteNum);
          }
        }
      }
    } else {
      for (size_t t = 0; t < res.numTimes(); ++t) {
        for (size_t c = 0; c < res.numChannels(); ++c) {
          // first plane
          std::memcpy(res.planeData(0, c, t),
                      res.planeData(tdepth - 1, c, t),
                      planeByteNum);
          // save to subtract
          std::memcpy(bufPlane.data(), res.planeData(0, c, t), planeByteNum);
          // other
          for (size_t z = 1; z < depth(); ++z) {
            auto resData = res.planeData<TVoxel>(z, c, t);
            for (size_t v = 0; v < planeVoxelNum; ++v) {
              resData[v] = saturate_add(resData[v - planeVoxelNum],
                                        resData[v + dataOffset]);
            }
          }
          for (size_t z = depth(); z < tdepth; ++z) {
            auto resData = res.planeData<TVoxel>(z, c, t);
            std::memcpy(resData, resData - planeVoxelNum, planeByteNum);
          }
          for (size_t z = tdepth; z < res.depth(); ++z) {
            auto resData = res.planeData<TVoxel>(z, c, t);
            std::memcpy(buf.data(), resData, planeByteNum);
            for (size_t v = 0; v < planeVoxelNum; ++v) {
              resData[v] = saturate_sub(resData[v - planeVoxelNum], bufPlane[v]);
            }
            std::memcpy(bufPlane.data(), buf.data(), planeByteNum);
          }
        }
      }
    }
  }
}

template<typename TVoxel>
void ZImg::blockSumPart_Impl(ZImg& res, size_t twidth, size_t theight, size_t tdepth, size_t xStart,
                             size_t yStart, size_t zStart) const
{
  size_t srcRowNum = rowVoxelNumber();
  size_t srcPlaneNum = planeVoxelNumber();
  for (size_t t = 0; t < res.numTimes(); ++t) {
    for (size_t c = 0; c < res.numChannels(); ++c) {
      auto desData = res.channelData<TVoxel>(c, t);
      size_t desOffset = 0;
      for (size_t z = 0; z < res.depth(); ++z) {
        size_t blockZStart = std::max(0, static_cast<int>(zStart + z) - static_cast<int>(tdepth) + 1);
        size_t blockZEnd = std::min(depth(), zStart + z + 1);
        for (size_t y = 0; y < res.height(); ++y) {
          size_t blockYStart = std::max(0, static_cast<int>(yStart + y) - static_cast<int>(theight) + 1);
          size_t blockYEnd = std::min(height(), yStart + y + 1);
          TVoxel inc = 0;
          TVoxel dec = 0;
          for (size_t x = 0; x < res.width(); ++x) {
            int tleft = static_cast<int>(xStart + x) - static_cast<int>(twidth) + 1;
            size_t tright = xStart + x + 1;
            size_t blockXStart = std::max(0, tleft);
            size_t blockXEnd = std::min(width(), tright);
            auto srcData = data<TVoxel>(blockXStart, blockYStart, blockZStart, c, t);
            size_t srcOffset = 0;
            if (x == 0) {
              inc = 0;
              dec = 0;
              for (size_t mz = blockZStart; mz < blockZEnd; ++mz) {
                for (size_t my = blockYStart; my < blockYEnd; ++my) {
                  if (tleft >= 0) {
                    dec = saturate_add(dec, srcData[srcOffset]);
                  }
                  if (tright < width()) {
                    inc = saturate_add(inc, srcData[srcOffset + blockXEnd - blockXStart]);
                  }
                  for (size_t mx = blockXStart; mx < blockXEnd; ++mx) {
                    desData[desOffset] = saturate_add(desData[desOffset], srcData[srcOffset]);
                    ++srcOffset;
                  }
                  srcOffset += srcRowNum - (blockXEnd - blockXStart);
                }
                srcOffset += srcPlaneNum - (blockYEnd - blockYStart) * srcRowNum;
              }
            } else {
              desData[desOffset] = saturate_sub(saturate_add(desData[desOffset - 1], inc), dec);
              inc = 0;
              dec = 0;
              for (size_t mz = blockZStart; mz < blockZEnd; ++mz) {
                for (size_t my = blockYStart; my < blockYEnd; ++my) {
                  if (tleft >= 0) {
                    dec = saturate_add(dec, srcData[srcOffset]);
                  }
                  if (tright < width()) {
                    inc = saturate_add(inc, srcData[srcOffset + blockXEnd - blockXStart]);
                  }
                  srcOffset += srcRowNum;
                }
                srcOffset += srcPlaneNum - (blockYEnd - blockYStart) * srcRowNum;
              }
            }
            ++desOffset;
          }
        }
      }
    }
  }
}

template<typename TVoxel>
void ZImg::thresholdAbove_Impl(TVoxel threshold, ThresholdMode threMode, TVoxel outsideValue)
{
  if (threMode == ThresholdMode::IncludeThreshold) {
    for (size_t t = 0; t < numTimes(); ++t) {
      auto data = timeData<TVoxel>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        if (data[v] >= threshold) {
          data[v] = outsideValue;
        }
      }
    }
  } else if (threMode == ThresholdMode::ExcludeThreshold) {
    for (size_t t = 0; t < numTimes(); ++t) {
      auto data = timeData<TVoxel>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        if (data[v] > threshold) {
          data[v] = outsideValue;
        }
      }
    }
  }
}

template void ZImg::thresholdAbove_Impl<uint8_t>(uint8_t, ThresholdMode, uint8_t);

template void ZImg::thresholdAbove_Impl<uint16_t>(uint16_t, ThresholdMode, uint16_t);

template void ZImg::thresholdAbove_Impl<uint32_t>(uint32_t, ThresholdMode, uint32_t);

template void ZImg::thresholdAbove_Impl<uint64_t>(uint64_t, ThresholdMode, uint64_t);

template void ZImg::thresholdAbove_Impl<int8_t>(int8_t, ThresholdMode, int8_t);

template void ZImg::thresholdAbove_Impl<int16_t>(int16_t, ThresholdMode, int16_t);

template void ZImg::thresholdAbove_Impl<int32_t>(int32_t, ThresholdMode, int32_t);

template void ZImg::thresholdAbove_Impl<int64_t>(int64_t, ThresholdMode, int64_t);

template void ZImg::thresholdAbove_Impl<float>(float, ThresholdMode, float);

template void ZImg::thresholdAbove_Impl<double>(double, ThresholdMode, double);

template<typename TVoxel>
void ZImg::thresholdBelow_Impl(TVoxel threshold, ThresholdMode threMode, TVoxel outsideValue)
{
  if (threMode == ThresholdMode::IncludeThreshold) {
    for (size_t t = 0; t < numTimes(); ++t) {
      auto data = timeData<TVoxel>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        if (data[v] <= threshold) {
          data[v] = outsideValue;
        }
      }
    }
  } else if (threMode == ThresholdMode::ExcludeThreshold) {
    for (size_t t = 0; t < numTimes(); ++t) {
      auto data = timeData<TVoxel>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        if (data[v] < threshold) {
          data[v] = outsideValue;
        }
      }
    }
  }
}

template void ZImg::thresholdBelow_Impl<uint8_t>(uint8_t, ThresholdMode, uint8_t);

template void ZImg::thresholdBelow_Impl<uint16_t>(uint16_t, ThresholdMode, uint16_t);

template void ZImg::thresholdBelow_Impl<uint32_t>(uint32_t, ThresholdMode, uint32_t);

template void ZImg::thresholdBelow_Impl<uint64_t>(uint64_t, ThresholdMode, uint64_t);

template void ZImg::thresholdBelow_Impl<int8_t>(int8_t, ThresholdMode, int8_t);

template void ZImg::thresholdBelow_Impl<int16_t>(int16_t, ThresholdMode, int16_t);

template void ZImg::thresholdBelow_Impl<int32_t>(int32_t, ThresholdMode, int32_t);

template void ZImg::thresholdBelow_Impl<int64_t>(int64_t, ThresholdMode, int64_t);

template void ZImg::thresholdBelow_Impl<float>(float, ThresholdMode, float);

template void ZImg::thresholdBelow_Impl<double>(double, ThresholdMode, double);

template<typename TVoxel>
void ZImg::binarized_Impl(ZImg& res, TVoxel threshold, ThresholdMode threMode) const
{
  if (threMode == ThresholdMode::IncludeThreshold) {
    for (size_t t = 0; t < numTimes(); ++t) {
      auto data = timeData<TVoxel>(t);
      auto resData = res.timeData<uint8_t>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        if (data[v] >= threshold) {
          resData[v] = 1;
        }
      }
    }
  } else if (threMode == ThresholdMode::ExcludeThreshold) {
    for (size_t t = 0; t < numTimes(); ++t) {
      auto data = timeData<TVoxel>(t);
      auto resData = res.timeData<uint8_t>(t);
      for (size_t v = 0; v < timeVoxelNumber(); ++v) {
        if (data[v] > threshold) {
          resData[v] = 1;
        }
      }
    }
  }
}

template void ZImg::binarized_Impl<uint8_t>(ZImg&, uint8_t, ThresholdMode) const;

template void ZImg::binarized_Impl<uint16_t>(ZImg&, uint16_t, ThresholdMode) const;

template void ZImg::binarized_Impl<uint32_t>(ZImg&, uint32_t, ThresholdMode) const;

template void ZImg::binarized_Impl<uint64_t>(ZImg&, uint64_t, ThresholdMode) const;

template void ZImg::binarized_Impl<int8_t>(ZImg&, int8_t, ThresholdMode) const;

template void ZImg::binarized_Impl<int16_t>(ZImg&, int16_t, ThresholdMode) const;

template void ZImg::binarized_Impl<int32_t>(ZImg&, int32_t, ThresholdMode) const;

template void ZImg::binarized_Impl<int64_t>(ZImg&, int64_t, ThresholdMode) const;

template void ZImg::binarized_Impl<float>(ZImg&, float, ThresholdMode) const;

template void ZImg::binarized_Impl<double>(ZImg&, double, ThresholdMode) const;

template<typename TVoxel>
void ZImg::showContentAsQString_Impl(QString& res) const
{
  QTextStream os(&res, QIODevice::WriteOnly);
  os << "start img\n";
  for (size_t t = 0; t < numTimes(); ++t) {
    for (size_t c = 0; c < numChannels(); ++c) {
      for (size_t z = 0; z < depth(); ++z) {
        for (size_t y = 0; y < height(); ++y) {
          os << t << ":" << c << ":" << z << ":" << y << ": ";
          auto data = rowData<TVoxel>(y, z, c, t);
          for (size_t x = 0; x < width(); ++x) {
            os << data[x] << " ";
          }
          os << "\n";
        }
        os << "\n";
      }
      os << "\n";
    }
    os << "\n";
  }
  os << "\n";
  os << "end img\n";
}

void tag_invoke(const json::value_from_tag&, json::value& jv, const ZImg& img)
{
  IMG_TYPED_CALL(tag_invoke_img_Impl, img.info(), jv, img)
}

template<typename TVoxel>
void tag_invoke_img_Impl(json::value& jv, const ZImg& img)
{
  auto& jo = jv.emplace_object();
  jo["info"] = json::value_from(img.info());
  json::array ja(img.voxelNumber());
  size_t i = 0;
  for (size_t t = 0; t < img.numTimes(); ++t) {
    auto* data = img.timeData<TVoxel>(t);
    for (size_t v = 0; v < img.timeVoxelNumber(); ++v) {
      ja[i++] = data[v];
    }
  }
  jo["data"] = ja;
}

ZImg tag_invoke(const json::value_to_tag<ZImg>&, const json::value& jv)
{
  auto info = json::value_to<ZImgInfo>(jv.at("info"));
  ZImg res(info);

  IMG_RETURN_TYPED_CALL(tag_invoke_img_Impl, info, res, jv)
}

template<typename TVoxel>
ZImg tag_invoke_img_Impl(ZImg& img, const json::value& jv)
{
  auto data = json::value_to<std::vector<TVoxel>>(jv.at("data"));
  ZImg tmp;
  tmp.wrapData(data.data(), img.info());
  img.pasteImg(tmp);
  return img;
}

}  // namespace nim
