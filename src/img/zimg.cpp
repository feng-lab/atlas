#include "zimg.h"
#include "zimgio.h"
#include "zlog.h"
#include "zimage3dutils.h"
#include "zstatisticsutils.h"
#include <algorithm>
#include "zrandom.h"
#include <QTextStream>
#include <type_traits>
#include "zbenchtimer.h"
#include <QFileInfo>

namespace {

struct MinOp {
  template<typename TVoxel, typename TVoxelOther>
  TVoxel operator()(TVoxel voxelRef, TVoxelOther otherVoxel) const
  {
    return std::min(voxelRef, static_cast<TVoxel>(otherVoxel));
  }
};

struct MaxOp {
  template<typename TVoxel, typename TVoxelOther>
  TVoxel operator()(TVoxel voxelRef, TVoxelOther otherVoxel) const
  {
    return std::max(voxelRef, static_cast<TVoxel>(otherVoxel));
  }
};

}

namespace nim {

ZImgMetadata::ZImgMetadata()
  : ZImgMetadataBase<ZImgMetatag>()
{
}

QString ZImgMetadata::toQString() const
{
  QString res;

  for (std::map<_AttachPoint, std::vector<ZImgMetatag>>::const_iterator it = m_data.begin();
       it != m_data.end(); ++it) {
    if (!it->second.empty()) {
      res = res % QString("Attach Point: z: %1, c: %2, t: %3\n").arg(it->first.z)
          .arg(it->first.c).arg(it->first.t);
      for (size_t i=0; i<it->second.size(); ++i)
        res = res % "  " % it->second[i].toQString() % "\n";
    }
  }

  return res;
}

ZImgThumbernail::ZImgThumbernail()
  : ZImgMetadataBase<ZImg>()
{
}

QString ZImgThumbernail::toQString() const
{
  QString res;

  for (std::map<_AttachPoint, std::vector<ZImg>>::const_iterator it = m_data.begin();
       it != m_data.end(); ++it) {
    if (!it->second.empty()) {
      res = res % QString("Attach Point: z: %1, c: %2, t: %3, Number of Thumbnails: %4\n").arg(it->first.z)
          .arg(it->first.c).arg(it->first.t).arg(it->second.size());
      for (size_t i=0; i<it->second.size(); ++i)
        res = res % "  thumb <" % it->second[i].info().toQString() % ">\n";
    }
  }

  return res;
}

ZImgSource::ZImgSource()
{
}

ZImgSource::ZImgSource(const QString &fn, const ZImgRegion &rgn, size_t scene, FileFormat format)
  : region(rgn), scene(scene), format(format)
{
  QFileInfo fi(fn);
  if (fi.exists()) {
    filenames << fi.canonicalFilePath();
    totalFileSize += fi.size();
  } else {
    throw ZIOException(QString("file %1 does not exist").arg(fn));
  }
}

ZImgSource::ZImgSource(const QStringList &fns, Dimension catDim, const ZImgRegion &rgn, size_t scene, FileFormat format,
                       bool expandXY, bool expandWithMaxValue)
  : catDim(catDim), region(rgn), scene(scene), format(format), expandXY(expandXY)
  , expandWithMaxValue(expandWithMaxValue)
{
  for (int i=0; i<fns.size(); ++i) {
    QFileInfo fi(fns[i]);
    if (fi.exists()) {
      filenames << fi.canonicalFilePath();
      totalFileSize += fi.size();
    } else {
      throw ZIOException(QString("file %1 does not exist").arg(fns[i]));
    }
  }
}

QString ZImgSource::toQString() const
{
  QString res;
  if (filenames.size() > 1) {
    res = filenames[0] + QString(" %1 Sequence Scene %2").arg(enumToString(catDim)).arg(scene);
    if (!region.isDefault()) {
      res += QString(" Region %1").arg(region.toQString());
    }
  } else if (filenames.size() == 1) {
    res = filenames[0] + QString(" Scene %2").arg(scene);
    if (!region.isDefault()) {
      res += QString(" Region %1").arg(region.toQString());
    }
  }
  return res;
}

//-----------------------------------------------------------------------------------

ZImg::ZImg()
  : m_ownData(true)
{
}

ZImg::ZImg(const ZImgInfo &info)
  : m_info(info)
  , m_ownData(true)
{
  allocate();
}

ZImg::ZImg(const ZImg &other)
{
  m_info = other.m_info;
  m_metadata = other.m_metadata;
  m_thumbnail = other.m_thumbnail;
  //m_ownData = other.m_ownData;
  m_ownData = true;
  if (m_ownData) { // deep copy
    allocate();
    for (size_t t=0; t<numTimes(); ++t)
      memcpy(timeData<uint8_t>(t), other.timeData<uint8_t>(t), timeByteNumber());
  } else { // shallow copy
    m_data.resize(numTimes());
    for (size_t t=0; t<numTimes(); ++t)
      m_data[t] = other.m_data[t];
  }
}

ZImg::ZImg(ZImg &&other) noexcept
{
  swap(other);
}

ZImg::ZImg(const QString &filename, ZImgRegion region, size_t scene, FileFormat format)
{
  load(filename, region, scene, format);
}

ZImg::ZImg(const ZImgSource &imgSource)
{
  load(imgSource);
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

void ZImg::swap(ZImg &other) noexcept
{
  m_data.swap(other.m_data);
  m_thumbnail.swap(other.m_thumbnail);
  m_info.swap(other.m_info);
  m_metadata.swap(other.m_metadata);
  std::swap(m_ownData, other.m_ownData);
}

void ZImg::getQtReadNameFilter(QStringList &filters, QList<FileFormat> &formats)
{
  ZImgIOInstance.getQtReadNameFilter(filters, formats);
}

void ZImg::getQtWriteNameFilter(QStringList &filters, QList<FileFormat> &formats, QList<Compression> &comps)
{
  ZImgIOInstance.getQtWriteNameFilter(filters, formats, comps);
}

bool ZImg::fileExtensionReadSupported(const QString &filename)
{
  return ZImgIOInstance.fileExtensionReadSupported(filename);
}

bool ZImg::fileExtensionWriteSupported(const QString &filename)
{
  return ZImgIOInstance.fileExtensionWriteSupported(filename);
}

void ZImg::load(const QString &filename, size_t scene, FileFormat format)
{
  clear();
  ZImgIOInstance.readImg(filename, *this, ZImgRegion(), scene, 1, format);
}

void ZImg::load(const QString &filename, ZImgRegion region, size_t scene, FileFormat format)
{
  clear();
  ZImgIOInstance.readImg(filename, *this, region, scene, 1, format);
}

void ZImg::load(const ZImgSource &imgSource)
{
  clear();
  ZImgIOInstance.readImg(imgSource, *this);
}

void ZImg::save(const QString &filename, FileFormat format, Compression comp) const
{
  ZImgIOInstance.writeImg(filename, *this, format, comp);
}

void ZImg::load(const QStringList &fileList, Dimension catDim, size_t scene, FileFormat format,
                bool expandXY, bool expandWithMaxValue)
{
  clear();
  ZImgIOInstance.readImg(fileList, catDim, *this, scene, format, expandXY, expandWithMaxValue);
}

void ZImg::load(const QStringList &fileList, Dimension catDim, const ZImgRegion &region, size_t scene, FileFormat format,
                bool expandXY, bool expandWithMaxValue)
{
  clear();
  ZImgIOInstance.readImg(fileList, catDim, region, *this, scene, format, expandXY, expandWithMaxValue);
}

std::vector<ZImgInfo> ZImg::readImgInfo(const QString &filename, std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> *subBlocks, FileFormat format)
{
  std::vector<ZImgInfo> res;
  ZImgIOInstance.readInfo(filename, res, subBlocks, nullptr, format);
  return res;
}

std::vector<ZImgInfo> ZImg::readImgInfo(const QStringList &fileList, Dimension catDim, std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> *subBlocks,
                                        FileFormat format, bool expandXY)
{
  std::vector<ZImgInfo> res;
  ZImgIOInstance.readInfo(fileList, catDim, res, subBlocks, format, expandXY);
  return res;
}

void ZImg::wrapData(void *data, const ZImgInfo &info)
{
  clear();

  m_info = info;

  m_ownData = false;

  m_data.resize(m_info.numTimes);

  for (size_t i=0; i<m_info.numTimes; ++i) {
    // reinterpret_cast allowed (AliasedType is char or unsigned char: this permits
    // examination of the object representation of any object as an array of unsigned char.)
    m_data[i] = reinterpret_cast<uint8_t*>(data) + i * info.timeVoxelNumber();
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

  for (size_t i=0; i<m_info.numTimes; ++i) {
    m_data[i] = reinterpret_cast<uint8_t*>(data + i * width * height * depth * numChannels);
  }
}

template void ZImg::wrapData(uint8_t*,size_t,size_t,size_t,size_t,size_t);
template void ZImg::wrapData(uint16_t*,size_t,size_t,size_t,size_t,size_t);
template void ZImg::wrapData(uint32_t*,size_t,size_t,size_t,size_t,size_t);
template void ZImg::wrapData(uint64_t*,size_t,size_t,size_t,size_t,size_t);
template void ZImg::wrapData(int8_t*,size_t,size_t,size_t,size_t,size_t);
template void ZImg::wrapData(int16_t*,size_t,size_t,size_t,size_t,size_t);
template void ZImg::wrapData(int32_t*,size_t,size_t,size_t,size_t,size_t);
template void ZImg::wrapData(int64_t*,size_t,size_t,size_t,size_t,size_t);
template void ZImg::wrapData(float*,size_t,size_t,size_t,size_t,size_t);
template void ZImg::wrapData(double*,size_t,size_t,size_t,size_t,size_t);

template<typename TVoxel>
bool ZImg::isType() const
{
  if (m_info.voxelByteNumber() == sizeof(TVoxel)) {
  switch (m_info.voxelFormat) {
  case VoxelFormat::Unsigned:
    if (std::is_integral<TVoxel>::value && std::is_unsigned<TVoxel>::value)
      return true;
    break;
  case VoxelFormat::Signed:
    if (std::is_integral<TVoxel>::value && std::is_signed<TVoxel>::value)
      return true;
    break;
  case VoxelFormat::Float:
    if (std::is_floating_point<TVoxel>::value)
      return true;
    break;
  default:
    break;
  }
  }
  return false;
}

template bool ZImg::isType<uint8_t>() const;
template bool ZImg::isType<uint16_t>() const;
template bool ZImg::isType<uint32_t>() const;
template bool ZImg::isType<uint64_t>() const;
template bool ZImg::isType<int8_t>() const;
template bool ZImg::isType<int16_t>() const;
template bool ZImg::isType<int32_t>() const;
template bool ZImg::isType<int64_t>() const;
template bool ZImg::isType<float>() const;
template bool ZImg::isType<double>() const;
template bool ZImg::isType<const uint8_t>() const;
template bool ZImg::isType<const uint16_t>() const;
template bool ZImg::isType<const uint32_t>() const;
template bool ZImg::isType<const uint64_t>() const;
template bool ZImg::isType<const int8_t>() const;
template bool ZImg::isType<const int16_t>() const;
template bool ZImg::isType<const int32_t>() const;
template bool ZImg::isType<const int64_t>() const;
template bool ZImg::isType<const float>() const;
template bool ZImg::isType<const double>() const;

void ZImg::allocate()
{
  clearData();
  if (m_info.isEmpty())
    return;
  m_data.resize(m_info.numTimes);

  for (size_t t=0; t<m_info.numTimes; ++t) {
    m_data[t] = static_cast<uint8_t*>(boost::alignment::aligned_alloc(32, timeByteNumber()));

    if (m_data[t] == nullptr) {
      clearData();
      ZImgInfo info = m_info;
      m_info.clear();
      throw ZImgException(QString("Can not allocate memory for img <%1>").arg(info.toQString()));
    }
  }

  int64_t defaultValue = 0;
  if (m_info.voxelFormat == VoxelFormat::Signed) {
    switch (m_info.bytesPerVoxel) {
    case 1:
      defaultValue = std::numeric_limits<int8_t>::min();
      break;
    case 2:
      defaultValue = std::numeric_limits<int16_t>::min();
      break;
    case 4:
      defaultValue = std::numeric_limits<int32_t>::min();
      break;
    case 8:
      defaultValue = std::numeric_limits<int64_t>::min();
      break;
    default:
      break;
    }
  }
  fill(defaultValue);
}

QString ZImg::toQString() const
{
  QString res;
  if (isEmpty())
    return res;
  IMG_TYPED_CALL(showContentAsQString_Impl, (*this), res);
  return res;
}

ZImg ZImg::createView(int c, int t)
{
  ZImgRegion rgn;
  if (c >= 0) {
    rgn.start.c = c;
    rgn.end.c = c+1;
  }
  if (t >= 0) {
    rgn.start.t = t;
    rgn.end.t = t+1;
  }
  if (!rgn.isValid(m_info)) {
    throw ZImgException(QString("Invalid view of img, c:%1, t:%2, rgn:%3, img:%4")
                        .arg(c).arg(t).arg(rgn.toQString()).arg(m_info.toQString()));
  }
  ZImg res;
  res.m_info = rgn.clip(m_info);
  res.m_data.resize(res.numTimes());
  for (size_t t=0; t<res.numTimes(); ++t)
    res.m_data[t] = channelData<uint8_t>(rgn.start.c, t+rgn.start.t);
  res.m_ownData = false;
  return res;
}

const ZImg ZImg::createView(int c, int t) const
{
  ZImgRegion rgn;
  if (c >= 0) {
    rgn.start.c = c;
    rgn.end.c = c+1;
  }
  if (t >= 0) {
    rgn.start.t = t;
    rgn.end.t = t+1;
  }
  if (!rgn.isValid(m_info)) {
    throw ZImgException(QString("Invalid view of img, c:%1, t:%2, rgn:%3, img:%4")
                        .arg(c).arg(t).arg(rgn.toQString()).arg(m_info.toQString()));
  }
  ZImg res;
  res.m_info = rgn.clip(m_info);
  res.m_data.resize(res.numTimes());
  for (size_t t=0; t<res.numTimes(); ++t)
    res.m_data[t] = &(m_data[t+rgn.start.t][0]) + rgn.start.c * m_info.channelByteNumber();
  res.m_ownData = false;
  return res;
}

ZImg ZImg::createView(size_t z, size_t c, size_t t)
{
  ZImgRegion rgn;
  rgn.start.z = z;
  rgn.end.z = z+1;
  rgn.start.c = c;
  rgn.end.c = c+1;
  rgn.start.t = t;
  rgn.end.t = t+1;
  if (!rgn.isValid(m_info)) {
    throw ZImgException(QString("Invalid view of img, z: %1, c:%2, t:%3, rgn:%4, img:%5")
                        .arg(z).arg(c).arg(t).arg(rgn.toQString()).arg(m_info.toQString()));
  }
  ZImg res;
  res.m_info = rgn.clip(m_info);
  res.m_data.resize(res.numTimes());
  for (size_t t=0; t<res.numTimes(); ++t)
    res.m_data[t] = planeData<uint8_t>(rgn.start.z, rgn.start.c, t+rgn.start.t);
  res.m_ownData = false;
  return res;
}

const ZImg ZImg::createView(size_t z, size_t c, size_t t) const
{
  ZImgRegion rgn;
  rgn.start.z = z;
  rgn.end.z = z+1;
  rgn.start.c = c;
  rgn.end.c = c+1;
  rgn.start.t = t;
  rgn.end.t = t+1;
  if (!rgn.isValid(m_info)) {
    throw ZImgException(QString("Invalid view of img, z: %1, c:%2, t:%3, rgn:%4, img:%5")
                        .arg(z).arg(c).arg(t).arg(rgn.toQString()).arg(m_info.toQString()));
  }
  ZImg res;
  res.m_info = rgn.clip(m_info);
  res.m_data.resize(res.numTimes());
  for (size_t t=0; t<res.numTimes(); ++t)
    res.m_data[t] = &(m_data[t+rgn.start.t][0]) +
        rgn.start.c * m_info.channelByteNumber() + rgn.start.z * m_info.planeByteNumber();
  res.m_ownData = false;
  return res;
}

template<typename TValue>
void ZImg::computeMinMax(TValue &min, TValue &max) const
{
  IMG_TYPED_CALL(computeMinMax_Impl, (*this), min, max);
}

template void ZImg::computeMinMax(uint8_t&,uint8_t&) const;
template void ZImg::computeMinMax(uint16_t&,uint16_t&) const;
template void ZImg::computeMinMax(uint32_t&,uint32_t&) const;
template void ZImg::computeMinMax(uint64_t&,uint64_t&) const;
template void ZImg::computeMinMax(int8_t&,int8_t&) const;
template void ZImg::computeMinMax(int16_t&,int16_t&) const;
template void ZImg::computeMinMax(int32_t&,int32_t&) const;
template void ZImg::computeMinMax(int64_t&,int64_t&) const;
template void ZImg::computeMinMax(float&,float&) const;
template void ZImg::computeMinMax(double&,double&) const;

std::vector<size_t> ZImg::histogram(size_t nbins, const ZImg &mask) const
{
  if (nbins == 0) {
    nbins = bytesPerVoxel() > 1 ? 65536 : 256;
  }
  std::vector<size_t> res(nbins, 0);

  if (mask.isEmpty()) {
    IMG_TYPED_CALL(histogram_Impl, (*this), res);
  } else if (isSameSize(mask)) {
    IMG_TYPED_CALL_2TYPE(histogramMask_Impl, (*this), mask, res, mask);
  } else {
    throw ZImgException(QString("histogram mask has different size <%1> than current img <%2>")
                        .arg(mask.info().toQString()).arg(m_info.toQString()));
  }

  return res;
}

std::pair<double, double> ZImg::binRange(size_t binIdx, size_t nbins) const
{
  if (voxelFormat() == VoxelFormat::Float) {
    return binRange(binIdx, 0.0, 1.0, nbins);
  } else if (voxelFormat() == VoxelFormat::Signed) {
    return binRange(binIdx, dataRangeMin<int64_t>(), dataRangeMax<int64_t>(), nbins);
  } else {
    return binRange(binIdx, dataRangeMin<uint64_t>(), dataRangeMax<uint64_t>(), nbins);
  }
}

ZImg ZImg::crop(const ZImgRegion &region) const
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
  for (size_t t=rgn.start.t; t < static_cast<size_t>(rgn.end.t); ++t) {
    if (rgn.containsWholeChannel(m_info)) {
      // copy continues channel blocks
      memcpy(res.timeData(t-rgn.start.t),
             channelData(rgn.start.c, t), res.timeByteNumber());
    } else if (rgn.containsWholePlane(m_info)) {
      // copy channel by channel
      for (size_t c=rgn.start.c; c < static_cast<size_t>(rgn.end.c); ++c) {
        memcpy(res.channelData(c-rgn.start.c, t-rgn.start.t),
               planeData(rgn.start.z, c, t), res.channelByteNumber());

      }
    } else if (rgn.containsWholeRow(m_info)) {
      // copy plane by plane
      for (size_t c=rgn.start.c; c < static_cast<size_t>(rgn.end.c); ++c) {
        for (size_t z=rgn.start.z; z < static_cast<size_t>(rgn.end.z); ++z) {
          memcpy(res.planeData(z-rgn.start.z, c-rgn.start.c, t-rgn.start.t),
                 rowData(rgn.start.y, z, c, t), res.planeByteNumber());
        }
      }
    } else {
      // copy row by row
      for (size_t c=rgn.start.c; c < static_cast<size_t>(rgn.end.c); ++c) {
        for (size_t z=rgn.start.z; z < static_cast<size_t>(rgn.end.z); ++z) {
          for (size_t y=rgn.start.y; y < static_cast<size_t>(rgn.end.y); ++y) {
            memcpy(res.rowData(y-rgn.start.y, z-rgn.start.z, c-rgn.start.c, t-rgn.start.t),
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
  rgn.end.x = x+1;
  rgn.start.y = y;
  rgn.end.y = y+1;
  if (z >= 0) {
    rgn.start.z = z;
    rgn.end.z = z+1;
  }
  if (c >= 0) {
    rgn.start.c = c;
    rgn.end.c = c+1;
  }
  if (t >= 0) {
    rgn.start.t = t;
    rgn.end.t = t+1;
  }
  return crop(rgn);
}

ZImg ZImg::extractCol(size_t x, int z, int c, int t) const
{
  ZImgRegion rgn;
  rgn.start.x = x;
  rgn.end.x = x+1;
  if (z >= 0) {
    rgn.start.z = z;
    rgn.end.z = z+1;
  }
  if (c >= 0) {
    rgn.start.c = c;
    rgn.end.c = c+1;
  }
  if (t >= 0) {
    rgn.start.t = t;
    rgn.end.t = t+1;
  }
  return crop(rgn);
}

ZImg ZImg::extractRow(size_t y, int z, int c, int t) const
{
  ZImgRegion rgn;
  rgn.start.y = y;
  rgn.end.y = y+1;
  if (z >= 0) {
    rgn.start.z = z;
    rgn.end.z = z+1;
  }
  if (c >= 0) {
    rgn.start.c = c;
    rgn.end.c = c+1;
  }
  if (t >= 0) {
    rgn.start.t = t;
    rgn.end.t = t+1;
  }
  return crop(rgn);
}

ZImg ZImg::extractPlane(size_t z, int c, int t) const
{
  ZImgRegion rgn;
  rgn.start.z = z;
  rgn.end.z = z+1;
  if (c >= 0) {
    rgn.start.c = c;
    rgn.end.c = c+1;
  }
  if (t >= 0) {
    rgn.start.t = t;
    rgn.end.t = t+1;
  }
  return crop(rgn);
}

ZImg ZImg::extractChannel(size_t c, int t) const
{
  ZImgRegion rgn;
  rgn.start.c = c;
  rgn.end.c = c+1;
  if (t >= 0) {
    rgn.start.t = t;
    rgn.end.t = t+1;
  }
  return crop(rgn);
}

ZImg ZImg::extractTime(size_t t) const
{
  ZImgRegion rgn;
  rgn.start.t = t;
  rgn.end.t = t+1;
  return crop(rgn);
}

template<typename TVoxel>
void ZImg::fillRandom_Impl()
{
  std::uniform_int_distribution<TVoxel> dist(dataRangeMin<TVoxel>(), dataRangeMax<TVoxel>());
  for (size_t t=0; t<numTimes(); ++t) {
    TVoxel* data = timeData<TVoxel>(t);
    for (size_t v=0; v<timeVoxelNumber(); ++v) {
      data[v] = dist(ZRandomInstance.engine());
    }
  }
}

template<>
void ZImg::fillRandom_Impl<uint8_t>()
{
  std::uniform_int_distribution<uint32_t> dist(dataRangeMin<uint32_t>(), dataRangeMax<uint32_t>());
  for (size_t t=0; t<numTimes(); ++t) {
    uint8_t* data = timeData<uint8_t>(t);
    for (size_t v=0; v<timeVoxelNumber(); ++v) {
      data[v] = dist(ZRandomInstance.engine());
    }
  }
}

template<>
void ZImg::fillRandom_Impl<int8_t>()
{
  std::uniform_int_distribution<int32_t> dist(dataRangeMin<int32_t>(), dataRangeMax<int32_t>());
  for (size_t t=0; t<numTimes(); ++t) {
    int8_t* data = timeData<int8_t>(t);
    for (size_t v=0; v<timeVoxelNumber(); ++v) {
      data[v] = dist(ZRandomInstance.engine());
    }
  }
}

template<>
void ZImg::fillRandom_Impl<float>()
{
  std::uniform_real_distribution<float> dist(dataRangeMin<float>(), dataRangeMax<float>());
  for (size_t t=0; t<numTimes(); ++t) {
    float* data = timeData<float>(t);
    for (size_t v=0; v<timeVoxelNumber(); ++v) {
      data[v] = dist(ZRandomInstance.engine());
    }
  }
}

template<>
void ZImg::fillRandom_Impl<double>()
{
  std::uniform_real_distribution<double> dist(dataRangeMin<double>(), dataRangeMax<double>());
  for (size_t t=0; t<numTimes(); ++t) {
    double* data = timeData<double>(t);
    for (size_t v=0; v<timeVoxelNumber(); ++v) {
      data[v] = dist(ZRandomInstance.engine());
    }
  }
}

ZImg &ZImg::fillRandom()
{
  IMG_TYPED_CALL(fillRandom_Impl, (*this));
  return *this;
}

ZImg& ZImg::pasteImg(const ZImg& img, const ZVoxelCoordinate &start)
{
  typedef ZVoxelCoordinate::value_type TCoordinate;

  if (isEmpty() || img.isEmpty()) {
    LOG(WARNING) << "Trying to paste empty img, abort";
    return *this;
  }

  if ((start.x < 0 && start.x + static_cast<TCoordinate>(img.width()) <= 0) || start.x >= static_cast<TCoordinate>(width()) ||
      (start.y < 0 && start.y + static_cast<TCoordinate>(img.height()) <= 0) || start.y >= static_cast<TCoordinate>(height()) ||
      (start.z < 0 && start.z + static_cast<TCoordinate>(img.depth()) <= 0) || start.z >= static_cast<TCoordinate>(depth()) ||
      (start.c < 0 && start.c + static_cast<TCoordinate>(img.numChannels()) <= 0) || start.c >= static_cast<TCoordinate>(numChannels()) ||
      (start.t < 0 && start.t + static_cast<TCoordinate>(img.numTimes()) <= 0) || start.t >= static_cast<TCoordinate>(numTimes())) {
    LOG(WARNING) << "Trying to paste img with no overlap region, abort";
    return *this;
  }

  if (isSameType(img)) {
    size_t desX = std::max(start.x, TCoordinate(0));
    size_t srcX = desX - start.x;
    size_t desXEnd = std::min(start.x + static_cast<TCoordinate>(img.width()), static_cast<TCoordinate>(width()));
    size_t rowByteNumber = (desXEnd - desX) * m_info.bytesPerVoxel;

    for (TCoordinate desT = std::max(start.t, TCoordinate(0));
         desT < std::min(start.t + static_cast<TCoordinate>(img.numTimes()), static_cast<TCoordinate>(numTimes())); ++desT) {
      size_t srcT = desT - start.t;
      for (TCoordinate desC = std::max(start.c, TCoordinate(0));
           desC < std::min(start.c + static_cast<TCoordinate>(img.numChannels()), static_cast<TCoordinate>(numChannels())); ++desC) {
        size_t srcC = desC - start.c;
        for (TCoordinate desZ = std::max(start.z, TCoordinate(0));
             desZ < std::min(start.z + static_cast<TCoordinate>(img.depth()), static_cast<TCoordinate>(depth())); ++desZ) {
          size_t srcZ = desZ - start.z;
          for (TCoordinate desY = std::max(start.y, TCoordinate(0));
               desY < std::min(start.y + static_cast<TCoordinate>(img.height()), static_cast<TCoordinate>(height())); ++desY) {
            size_t srcY = desY - start.y;

            memcpy(data(desX, desY, desZ, desC, desT),
                   img.data(srcX, srcY, srcZ, srcC, srcT),
                   rowByteNumber);
          }
        }
      }
    }
  } else {
    IMG_TYPED_CALL_2TYPE(pasteImg_Impl, (*this), img, img, start);
  }

  return *this;
}

ZImg& ZImg::pasteImgMax(const ZImg &img, const ZVoxelCoordinate &start)
{
  typedef ZVoxelCoordinate::value_type TCoordinate;

  if (isEmpty() || img.isEmpty()) {
    LOG(WARNING) << "Trying to paste empty img, abort";
    return *this;
  }

  if ((start.x < 0 && start.x + static_cast<TCoordinate>(img.width()) <= 0) || start.x >= static_cast<TCoordinate>(width()) ||
      (start.y < 0 && start.y + static_cast<TCoordinate>(img.height()) <= 0) || start.y >= static_cast<TCoordinate>(height()) ||
      (start.z < 0 && start.z + static_cast<TCoordinate>(img.depth()) <= 0) || start.z >= static_cast<TCoordinate>(depth()) ||
      (start.c < 0 && start.c + static_cast<TCoordinate>(img.numChannels()) <= 0) || start.c >= static_cast<TCoordinate>(numChannels()) ||
      (start.t < 0 && start.t + static_cast<TCoordinate>(img.numTimes()) <= 0) || start.t >= static_cast<TCoordinate>(numTimes())) {
    LOG(WARNING) << "Trying to paste img with no overlap region, abort";
    return *this;
  }

  IMG_TYPED_CALL_2TYPE(pasteImgMax_Impl, (*this), img, img, start);

  return *this;
}

ZImg ZImg::cat(const std::vector<ZImg> &imgsIn, Dimension dim)
{
  std::vector<const ZImg*> imgs;
  for (size_t i=0; i<imgsIn.size(); ++i)
    imgs.push_back(&imgsIn[i]);
  return cat(imgs, dim);
}

ZImg ZImg::cat(const std::vector<ZImg *> &imgsIn, Dimension dim)
{
  std::vector<const ZImg*> imgs;
  for (size_t i=0; i<imgsIn.size(); ++i)
    imgs.push_back(imgsIn[i]);
  return cat(imgs, dim);
}

ZImg ZImg::cat(const std::vector<const ZImg *> &imgsIn, Dimension dim)
{
  // remove empty img
  std::vector<const ZImg*> imgs;
  for (size_t i=0; i<imgsIn.size(); ++i)
    if (imgsIn[i] && !imgsIn[i]->isEmpty())
      imgs.push_back(imgsIn[i]);

  if (imgs.empty())
    return ZImg();
  if (imgs.size() == 1)
    return *(imgs[0]);

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
      throw ZImgException(QString("Can not concat img <%1> and img <%2>").arg(info.toQString()).arg(firstInfo.toQString()));
    }
  }

  // create result img
  ZImg res(resInfo);

  if (dim == Dimension::T) {
    size_t tIdx = 0;
    for (size_t idx = 0; idx < imgs.size(); ++idx) {
      for (size_t t=0; t<imgs[idx]->numTimes(); ++t) {
        size_t desT = tIdx++;
        memcpy(res.timeData(desT), imgs[idx]->timeData(t), res.timeByteNumber());
      }
    }
  } else if (dim == Dimension::C) {
    for (size_t t=0; t<res.numTimes(); ++t) {
      size_t cIdx = 0;
      for (size_t idx = 0; idx < imgs.size(); ++idx) {
        memcpy(res.channelData(cIdx, t),
               imgs[idx]->timeData(t), imgs[idx]->timeByteNumber());
        cIdx += imgs[idx]->numChannels();
      }
    }
  } else if (dim == Dimension::Z) {
    for (size_t t=0; t<res.numTimes(); ++t) {
      for (size_t c=0; c<res.numChannels(); ++c) {
        size_t zIdx = 0;
        for (size_t idx = 0; idx < imgs.size(); ++idx) {
          memcpy(res.planeData(zIdx, c, t),
                 imgs[idx]->channelData(c,t), imgs[idx]->channelByteNumber());
          zIdx += imgs[idx]->depth();
        }
      }
    }
  } else if (dim == Dimension::Y) {
    for (size_t t=0; t<res.numTimes(); ++t) {
      for (size_t c=0; c<res.numChannels(); ++c) {
        for (size_t z=0; z<res.depth(); ++z) {
          size_t yIdx = 0;
          for (size_t idx = 0; idx < imgs.size(); ++idx) {
            memcpy(res.rowData(yIdx, z, c, t),
                   imgs[idx]->planeData(z, c, t), imgs[idx]->planeByteNumber());
            yIdx += imgs[idx]->height();
          }
        }
      }
    }
  } else {
    for (size_t t=0; t<res.numTimes(); ++t) {
      for (size_t c=0; c<res.numChannels(); ++c) {
        for (size_t z=0; z<res.depth(); ++z) {
          for (size_t y=0; y<res.height(); ++y) {
            size_t xIdx = 0;
            for (size_t idx = 0; idx < imgs.size(); ++idx) {
              memcpy(res.data(xIdx, y, z, c, t),
                     imgs[idx]->rowData(y, z, c, t), imgs[idx]->rowByteNumber());
              xIdx += imgs[idx]->width();
            }
          }
        }
      }
    }
  }

  return res;
}

ZImg ZImg::cat(const ZImg &img1, const ZImg &img2, Dimension dim)
{
  std::vector<const ZImg*> imgs;
  imgs.push_back(&img1);
  imgs.push_back(&img2);
  return cat(imgs, dim);
}

ZImg ZImg::cat(const ZImg &img1, const ZImg &img2, const ZImg &img3, Dimension dim)
{
  std::vector<const ZImg*> imgs;
  imgs.push_back(&img1);
  imgs.push_back(&img2);
  imgs.push_back(&img3);
  return cat(imgs, dim);
}

ZImg ZImg::cat(const ZImg &img1, const ZImg &img2, const ZImg &img3, const ZImg &img4, Dimension dim)
{
  std::vector<const ZImg*> imgs;
  imgs.push_back(&img1);
  imgs.push_back(&img2);
  imgs.push_back(&img3);
  imgs.push_back(&img4);
  return cat(imgs, dim);
}

ZImg ZImg::combine(const std::vector<ZImg> &imgsIn, ZImg::CombineMode mode)
{
  std::vector<const ZImg*> imgs;
  for (size_t i=0; i<imgsIn.size(); ++i)
    imgs.push_back(&imgsIn[i]);
  return combine(imgs, mode);
}

ZImg ZImg::combine(const std::vector<ZImg*> &imgsIn, ZImg::CombineMode mode)
{
  std::vector<const ZImg*> imgs;
  for (size_t i=0; i<imgsIn.size(); ++i)
    imgs.push_back(imgsIn[i]);
  return combine(imgs, mode);
}

ZImg ZImg::combine(const std::vector<const ZImg*> &imgsIn, ZImg::CombineMode mode)
{
  // remove empty img
  std::vector<const ZImg*> imgs;
  for (size_t i=0; i<imgsIn.size(); ++i)
    if (imgsIn[i] && !imgsIn[i]->isEmpty())
      imgs.push_back(imgsIn[i]);

  if (imgs.empty())
    return ZImg();
  if (imgs.size() == 1)
    return *(imgs[0]);

  // check dimensions size
  const ZImg* firstImg = imgs[0];
  ZImgInfo firstInfo = firstImg->info();
  for (size_t idx = 1; idx < imgs.size(); ++idx) {
    ZImgInfo info = imgs[idx]->info();
    if (!info.isSameType(firstInfo) || !info.isSameSize(firstInfo)) {
      throw ZImgException(QString("Can not combine img <%1> and img <%2>").arg(info.toQString()).arg(firstInfo.toQString()));
    }
  }

  IMG_RETURN_TYPED_CALL(combine_Impl, (*firstImg), imgs, mode);

  return ZImg();
}

ZImg ZImg::combine(const ZImg &img1, const ZImg &img2, ZImg::CombineMode mode)
{
  std::vector<const ZImg*> imgs;
  imgs.push_back(&img1);
  imgs.push_back(&img2);
  return combine(imgs, mode);
}

ZImg ZImg::combine(const ZImg &img1, const ZImg &img2, const ZImg &img3, ZImg::CombineMode mode)
{
  std::vector<const ZImg*> imgs;
  imgs.push_back(&img1);
  imgs.push_back(&img2);
  imgs.push_back(&img3);
  return combine(imgs, mode);
}

ZImg ZImg::combine(const ZImg &img1, const ZImg &img2, const ZImg &img3, const ZImg &img4, ZImg::CombineMode mode)
{
  std::vector<const ZImg*> imgs;
  imgs.push_back(&img1);
  imgs.push_back(&img2);
  imgs.push_back(&img3);
  imgs.push_back(&img4);
  return combine(imgs, mode);
}

ZImg ZImg::projectAlongDim(Dimension dim, CombineMode mode) const
{
  if (isEmpty() || m_info.size(dim) == 1)
    return *this;

  ZImg res;
  if (mode == CombineMode::Max) {
    for (size_t i=0; i<m_info.size(dim); ++i) {
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
      if (i == 0) {
        res.swap(subImg);
      } else {
        res.binaryOperation(subImg, MaxOp());
      }
    }
  } else if (mode == CombineMode::Min) {
    for (size_t i=0; i<m_info.size(dim); ++i) {
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
      if (i == 0) {
        res.swap(subImg);
      } else {
        res.binaryOperation(subImg, MinOp());
      }
    }
  } else {
    std::vector<ZImg> subImgs(m_info.size(dim));
    for (size_t i=0; i<subImgs.size(); ++i) {
      switch (dim) {
      case Dimension::T:
        subImgs[i] = extractTime(i);
        break;
      case Dimension::C:
        subImgs[i] = extractChannel(i, -1);
        break;
      case Dimension::Z:
        subImgs[i] = extractPlane(i, -1, -1);
        break;
      case Dimension::Y:
        subImgs[i] = extractRow(i, -1, -1, -1);
        break;
      case Dimension::X:
        subImgs[i] = extractCol(i, -1, -1, -1);
        break;
      default:
        break;
      }
    }

    std::vector<const ZImg*> imgs;
    for (size_t i=0; i<subImgs.size(); ++i)
      imgs.push_back(&subImgs[i]);
    res = combine(imgs, mode);
  }
  return res;
}

ZImg ZImg::maximumZProjection() const
{
  return projectAlongDim(Dimension::Z, CombineMode::Max);
}

ZImg ZImg::normalized() const
{
  ZImg res(*this);
  res.normalize();
  return res;
}

ZImg &ZImg::normalize()
{
  if (isEmpty())
    return *this;
  IMG_RETURN_TYPED_CALL(normalize_Impl, (*this));
  return *this;
}

template<typename TDesVoxel>
ZImg ZImg::castTo() const
{
  if (isType<TDesVoxel>())
    return *this;

  ZImgInfo info = m_info;
  info.setVoxelFormat<TDesVoxel>();
  ZImg res(info);

  IMG_TYPED_CALL_FIX2NDTYPE(cast_Impl, (*this), TDesVoxel, res);

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

  IMG_TYPED_CALL_2TYPE(cast_Impl, (*this), res, res);

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
  IMG_TYPED_CALL(resize_Impl, (*this), res, interpolant, antialiasing, antialiasingForNearest);

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

ZImg ZImg::blockDownsampled(size_t blockWidth, size_t blockHeight, size_t blockDepth, ZImg::CombineMode mode) const
{
  ZImgInfo info = m_info;
  info.voxelSizeX *= blockWidth;
  info.voxelSizeY *= blockHeight;
  info.voxelSizeZ *= blockDepth;
  info.width = m_info.width / blockWidth + m_info.width % blockWidth;
  info.height = m_info.height / blockHeight + m_info.height % blockHeight;
  info.depth = m_info.depth / blockDepth + m_info.depth % blockDepth;
  ZImg res(info);

  if (res.isEmpty())
    return res;

  IMG_TYPED_CALL(blockDownsampled_Impl, (*this), res, blockWidth, blockHeight, blockDepth, mode);
  return res;
}

ZImg &ZImg::resize(size_t desWidth, size_t desHeight, size_t desDepth, Interpolant interpolant,
                   bool antialiasing, bool antialiasingForNearest)
{
  if (width() == desWidth && height() == desHeight && depth() == desDepth)
    return *this;
  ZImg res = resized(desWidth, desHeight, desDepth, interpolant, antialiasing, antialiasingForNearest);
  swap(res);
  return *this;
}

ZImg &ZImg::zoom(double scaleX, double scaleY, double scaleZ, Interpolant interpolant,
                 bool antialiasing, bool antialiasingForNearest)
{
  if (scaleX == 1 && scaleY == 1 && scaleZ == 1)
    return *this;
  ZImg res = zoomed(scaleX, scaleY, scaleZ, interpolant, antialiasing, antialiasingForNearest);
  swap(res);
  return *this;
}

ZImg &ZImg::blockDownsample(size_t blockWidth, size_t blockHeight, size_t blockDepth, ZImg::CombineMode mode)
{
  if (blockWidth == 1 && blockHeight == 1 && blockDepth == 1)
    return *this;
  ZImg res = blockDownsampled(blockWidth, blockHeight, blockDepth, mode);
  swap(res);
  return *this;
}

ZImg &ZImg::flip(Dimension dim)
{
  if (isEmpty())
    return *this;
  /*if (dim == Dimension::L) { // flip locations
    size_t j = m_data.size() - 1;
    for (size_t i=0; i<m_data.size()/2; ++i, --j) {
      m_data[i].swap(m_data[j]);
    }
  } else */
  if (dim == Dimension::T) { // flip times
    size_t k = m_data.size() - 1;
    for (size_t j=0; j<m_data.size()/2; ++j, --k) {
      std::swap(m_data[j], m_data[k]);
    }
  } else if (dim == Dimension::C) { // flip channels
    if (numChannels() > 1) {
      std::vector<int8_t> buf(channelByteNumber());
      for (size_t t=0; t<numTimes(); ++t) {
        size_t j = numChannels() - 1;
        for (size_t i=0; i<numChannels()/2; ++i,--j) {
          // swap channel i,j
          memcpy(buf.data(), channelData(i,t), channelByteNumber());
          memcpy(channelData(i,t), channelData(j,t), channelByteNumber());
          memcpy(channelData(j,t), buf.data(), channelByteNumber());
        }
      }
    }
  } else if (dim < Dimension::C) {
    IMG_TYPED_CALL(flip_Impl, (*this), dim);
  }
  return *this;
}

ZImg &ZImg::reflect()
{
  if (isEmpty())
    return *this;
  // reflect time
  flip(Dimension::T);
  // reflect others
  IMG_TYPED_CALL(reflect_Impl, (*this));
  return *this;
}

ZImg ZImg::cumulativeSum(Dimension dim) const
{
  ZImg res = *this;
  if (dim == Dimension::T) {
    for (size_t t=1; t<numTimes(); ++t) {
      ZImg currentTime = res.createView(-1, t);
      ZImg lastTime = res.createView(-1, t-1);
      currentTime += lastTime;
    }
  } else if (dim == Dimension::C) {
    for (size_t c=1; c<numChannels(); ++c) {
      ZImg currentCh = res.createView(c, -1);
      ZImg lastCh = res.createView(c-1, -1);
      currentCh += lastCh;
    }
  } else if (dim < Dimension::C) {
    IMG_TYPED_CALL(cumulativeSum_Impl, (*this), res, dim);
  }
  return res;
}

ZImg ZImg::blockSum(size_t twidth, size_t theight, size_t tdepth) const
{
  if (twidth == 0 || theight == 0 || tdepth == 0) {
    throw ZImgException(QString("wrong template size input for blockSum: %1, %2, %3)").arg(twidth).arg(theight).arg(tdepth));
  }
  ZImg res;
  if (isEmpty())
    return res;
  ZImgInfo info = m_info;
  info.width += twidth-1;
  info.height += theight-1;
  info.depth += tdepth-1;
  res = ZImg(info);
  if (res.voxelFormat() == VoxelFormat::Signed) // default signed image voxel is negative
    res.fill(0);

  IMG_TYPED_CALL(blockSum_Impl, (*this), res, twidth, theight, tdepth);

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
  if (isEmpty())
    return res;
  ZImgInfo info = m_info;
  info.width = xEnd - xStart;
  info.height = yEnd - yStart;
  info.depth = zEnd - zStart;
  res = ZImg(info);
  if (res.voxelFormat() == VoxelFormat::Signed) // default signed image voxel is negative
    res.fill(0);

  IMG_TYPED_CALL(blockSumPart_Impl, (*this), res, twidth, theight, tdepth, xStart,
                 yStart, zStart);

  return res;
}

ZImg &ZImg::operator+=(const ZImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZImgException(QString("img addition requires same size img as input: this <%1>, other <%2>")
                        .arg(m_info.toQString()).arg(rhs.info().toQString()));
  }
  IMG_TYPED_CALL_2TYPE(addImg_Impl, (*this), rhs, rhs);
  return *this;
}

ZImg &ZImg::operator-=(const ZImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZImgException(QString("img subtraction requires same size img as input: this <%1>, other <%2>")
                        .arg(m_info.toQString()).arg(rhs.info().toQString()));
  }
  IMG_TYPED_CALL_2TYPE(subImg_Impl, (*this), rhs, rhs);
  return *this;
}

ZImg &ZImg::operator*=(const ZImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZImgException(QString("img multiplies requires same size img as input: this <%1>, other <%2>")
                        .arg(m_info.toQString()).arg(rhs.info().toQString()));
  }
  IMG_TYPED_CALL_2TYPE(mulImg_Impl, (*this), rhs, rhs);
  return *this;
}

ZImg &ZImg::operator/=(const ZImg& rhs)
{
  if (!isSameSize(rhs)) {
    throw ZImgException(QString("img divides requires same size img as input: this <%1>, other <%2>")
                        .arg(m_info.toQString()).arg(rhs.info().toQString()));
  }
  IMG_TYPED_CALL_2TYPE(divImg_Impl, (*this), rhs, rhs);
  return *this;
}

ZImg &ZImg::secureDivideBy(const ZImg &rhs)
{
  if (!isSameSize(rhs)) {
    throw ZImgException(QString("img divides requires same size img as input: this <%1>, other <%2>")
                        .arg(m_info.toQString()).arg(rhs.info().toQString()));
  }
  IMG_TYPED_CALL_2TYPE(secureDivImg_Impl, (*this), rhs, rhs);
  return *this;
}

bool ZImg::operator==(const ZImg& other) const
{
  if (isSameType(other) && isSameSize(other)) {
    for (size_t t=0; t<numTimes(); ++t) {
      const uint8_t* data = timeData(t);
      const uint8_t* otherData = other.timeData(t);
      if (data != otherData && memcmp(data, otherData, timeByteNumber()) != 0)
        return false;
    }
    return true;
  }
  return false;
}

ZVoxelCoordinate ZImg::indexToCoord(int64_t idx, const ZImgInfo &info)
{
  if (info.isEmpty())
    throw ZImgException(QString("Can not convert index to coord with empty img info <%1>").arg(info.toQString()));
  ZVoxelCoordinate res;
  //  res.l = idx >= 0 ? (idx / info.locationVoxelNumber()) : (- 1 - ((-idx-1) / info.locationVoxelNumber()));
  //  idx -= res.l * (int64_t)info.locationVoxelNumber();  //idx is positive now
  res.t = idx >= 0 ? (idx / info.timeVoxelNumber()) : (- 1 - ((-idx-1) / info.timeVoxelNumber()));
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

int64_t ZImg::coordToIndex(const ZVoxelCoordinate &coord, const ZImgInfo &info)
{
  return coord.t * static_cast<int64_t>(info.timeVoxelNumber()) +
      coord.c * static_cast<int64_t>(info.channelVoxelNumber()) +
      coord.z * static_cast<int64_t>(info.planeVoxelNumber()) +
      coord.y * static_cast<int64_t>(info.rowVoxelNumber()) +
      coord.x;
}

ZImg &ZImg::correctPreMultipliedColor()
{
  if (numChannels() > 1) {
    if (voxelFormat() == VoxelFormat::Float) {
      ZImg divImg = createView(numChannels()-1);
      for (size_t c=0; c<numChannels()-1; ++c) {
        ZImg chImg = createView(c);
        chImg.secureDivideBy(divImg);
      }
    } else {
      ZImg divImg = createView(numChannels()-1).convertTo<double>();
      for (size_t c=0; c<numChannels()-1; ++c) {
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

  for (size_t i=0; i<m_data.size(); ++i) {
    //for (size_t j=0; j<m_data[i].size(); ++j) {
    boost::alignment::aligned_free(m_data[i]);
    //}
  }
  m_data.clear();
}

void ZImg::wrapCoord(ZVoxelCoordinate &coord, PadOption padOption) const
{
  wrapCoordToImage(&coord.x, &m_info.width, m_info.numDimensions(), padOption);
}

void ZImg::checkConnInput(size_t &conn) const
{
  if (conn != 4 && conn != 8 && conn != 6 && conn != 18 && conn != 26) {
    throw ZImgException(QString("invalid conn input: %1").arg(conn));
  }
  if (is2DImg() && conn != 4 && conn != 8) {
    if (conn == 6)
      conn = 4;
    else
      conn = 8;
  }
}

template<typename TVoxel>
void ZImg::cropWithPad_Impl(ZImg &res, const ZVoxelCoordinate &startCoord, const ZVoxelCoordinate &endCoord,
                            PadOption padOption, TVoxel padValue) const
{
  ZVoxelCoordinate coord;
  for (coord.t=startCoord.t; coord.t<endCoord.t; ++coord.t) {
    for (coord.c=startCoord.c; coord.c<endCoord.c; ++coord.c) {
      for (coord.z=startCoord.z; coord.z<endCoord.z; ++coord.z) {
        for (coord.y=startCoord.y; coord.y<endCoord.y; ++coord.y) {
          for (coord.x=startCoord.x; coord.x<endCoord.x; ++coord.x) {
            *(res.data<TVoxel>(coord.x-startCoord.x, coord.y-startCoord.y, coord.z-startCoord.z,
                               coord.c-startCoord.c, coord.t-startCoord.t)) =
                valueWithPad_Impl<TVoxel>(coord, padOption, padValue);
          }
        }
      }
    }
  }
}

template void ZImg::cropWithPad_Impl<uint8_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, uint8_t) const;
template void ZImg::cropWithPad_Impl<uint16_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, uint16_t) const;
template void ZImg::cropWithPad_Impl<uint32_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, uint32_t) const;
template void ZImg::cropWithPad_Impl<uint64_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, uint64_t) const;
template void ZImg::cropWithPad_Impl<int8_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, int8_t) const;
template void ZImg::cropWithPad_Impl<int16_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, int16_t) const;
template void ZImg::cropWithPad_Impl<int32_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, int32_t) const;
template void ZImg::cropWithPad_Impl<int64_t>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, int64_t) const;
template void ZImg::cropWithPad_Impl<float>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, float) const;
template void ZImg::cropWithPad_Impl<double>(ZImg&, const ZVoxelCoordinate&, const ZVoxelCoordinate&, PadOption, double) const;

template<typename TVoxel>
void ZImg::fill_Impl(TVoxel value)
{
  for (size_t t=0; t<m_info.numTimes; ++t) {
    TVoxel* data = timeData<TVoxel>(t);
    std::fill(data, data+m_info.timeVoxelNumber(), value);
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
void ZImg::pasteImg_Impl(const ZImg& img, const ZVoxelCoordinate &start)
{
  typedef ZVoxelCoordinate::value_type TCoordinate;
  size_t desX = std::max(start.x, TCoordinate(0));
  size_t srcX = desX - start.x;
  size_t desXEnd = std::min(start.x + static_cast<TCoordinate>(img.width()), static_cast<TCoordinate>(width()));
  size_t rowVoxelNumber = desXEnd - desX;

  for (TCoordinate desT = std::max(start.t, TCoordinate(0));
       desT < std::min(start.t + static_cast<TCoordinate>(img.numTimes()), static_cast<TCoordinate>(numTimes())); ++desT) {
    size_t srcT = desT - start.t;
    for (TCoordinate desC = std::max(start.c, TCoordinate(0));
         desC < std::min(start.c + static_cast<TCoordinate>(img.numChannels()), static_cast<TCoordinate>(numChannels())); ++desC) {
      size_t srcC = desC - start.c;
      for (TCoordinate desZ = std::max(start.z, TCoordinate(0));
           desZ < std::min(start.z + static_cast<TCoordinate>(img.depth()), static_cast<TCoordinate>(depth())); ++desZ) {
        size_t srcZ = desZ - start.z;
        for (TCoordinate desY = std::max(start.y, TCoordinate(0));
             desY < std::min(start.y + static_cast<TCoordinate>(img.height()), static_cast<TCoordinate>(height())); ++desY) {
          size_t srcY = desY - start.y;

          TVoxel* desData = data<TVoxel>(desX, desY, desZ, desC, desT);
          const TVoxelImg* srcData = img.data<TVoxelImg>(srcX, srcY, srcZ, srcC, srcT);
          for (size_t v=0; v<rowVoxelNumber; ++v) {
            desData[v] = static_cast<TVoxel>(srcData[v]);
          }
        }
      }
    }
  }
}

template<typename TVoxel, typename TVoxelImg>
void ZImg::pasteImgMax_Impl(const ZImg& img, const ZVoxelCoordinate &start)
{
  typedef ZVoxelCoordinate::value_type TCoordinate;
  size_t desX = std::max(start.x, TCoordinate(0));
  size_t srcX = desX - start.x;
  size_t desXEnd = std::min(start.x + static_cast<TCoordinate>(img.width()), static_cast<TCoordinate>(width()));
  size_t rowVoxelNumber = desXEnd - desX;

  for (TCoordinate desT = std::max(start.t, TCoordinate(0));
       desT < std::min(start.t + static_cast<TCoordinate>(img.numTimes()), static_cast<TCoordinate>(numTimes())); ++desT) {
    size_t srcT = desT - start.t;
    for (TCoordinate desC = std::max(start.c, TCoordinate(0));
         desC < std::min(start.c + static_cast<TCoordinate>(img.numChannels()), static_cast<TCoordinate>(numChannels())); ++desC) {
      size_t srcC = desC - start.c;
      for (TCoordinate desZ = std::max(start.z, TCoordinate(0));
           desZ < std::min(start.z + static_cast<TCoordinate>(img.depth()), static_cast<TCoordinate>(depth())); ++desZ) {
        size_t srcZ = desZ - start.z;
        for (TCoordinate desY = std::max(start.y, TCoordinate(0));
             desY < std::min(start.y + static_cast<TCoordinate>(img.height()), static_cast<TCoordinate>(height())); ++desY) {
          size_t srcY = desY - start.y;

          TVoxel* desData = data<TVoxel>(desX, desY, desZ, desC, desT);
          const TVoxelImg* srcData = img.data<TVoxelImg>(srcX, srcY, srcZ, srcC, srcT);
          for (size_t v=0; v<rowVoxelNumber; ++v) {
            desData[v] = std::max(static_cast<TVoxel>(srcData[v]), desData[v]);
          }
        }
      }
    }
  }
}

template<typename TVoxel>
ZImg ZImg::combine_Impl(const std::vector<const ZImg*>& imgs, CombineMode mode)
{
  if (mode == CombineMode::Min) {
    ZImg res(*imgs[0]);
    for (size_t i=1; i<imgs.size(); ++i) {
      const ZImg* img = imgs[i];
      for (size_t t=0; t<res.numTimes(); ++t) {
        TVoxel* resData = res.timeData<TVoxel>(t);
        const TVoxel* srcData = img->timeData<TVoxel>(t);
        for (size_t v=0; v<res.timeVoxelNumber(); ++v) {
          resData[v] = std::min(resData[v], srcData[v]);
        }
      }
    }
    return res;
  } else if (mode == CombineMode::Max) {
    ZImg res(*imgs[0]);
    for (size_t i=1; i<imgs.size(); ++i) {
      const ZImg* img = imgs[i];
      for (size_t t=0; t<res.numTimes(); ++t) {
        TVoxel* resData = res.timeData<TVoxel>(t);
        const TVoxel* srcData = img->timeData<TVoxel>(t);
        for (size_t v=0; v<res.timeVoxelNumber(); ++v) {
          resData[v] = std::max(resData[v], srcData[v]);
        }
      }
    }
    return res;
  } else if (mode == CombineMode::Mean) {
    ZImg res(imgs[0]->info());
    std::vector<TVoxel> buf(imgs.size());

    for (size_t t=0; t<res.numTimes(); ++t) {
      TVoxel* resData = res.timeData<TVoxel>(t);
      for (size_t v=0; v<res.timeVoxelNumber(); ++v) {
        for (size_t i=0; i<imgs.size(); ++i) {
          const TVoxel* srcData = imgs[i]->timeData<TVoxel>(t);
          buf[i] = srcData[v];
        }
        resData[v] = static_cast<TVoxel>(mean(buf.begin(), buf.end()));
      }
    }
    return res;
  } else if (mode == CombineMode::Median) {
    ZImg res(imgs[0]->info());
    std::vector<TVoxel> buf(imgs.size());

    for (size_t t=0; t<res.numTimes(); ++t) {
      TVoxel* resData = res.timeData<TVoxel>(t);
      for (size_t v=0; v<res.timeVoxelNumber(); ++v) {
        for (size_t i=0; i<imgs.size(); ++i) {
          const TVoxel* srcData = imgs[i]->timeData<TVoxel>(t);
          buf[i] = srcData[v];
        }
        resData[v] = static_cast<TVoxel>(medianInPlace(buf.begin(), buf.end()));
      }
    }
    return res;
  }
  return ZImg();
}

template ZImg ZImg::combine_Impl<uint8_t>(const std::vector<const ZImg*>&, CombineMode);
template ZImg ZImg::combine_Impl<uint16_t>(const std::vector<const ZImg*>&, CombineMode);
template ZImg ZImg::combine_Impl<uint32_t>(const std::vector<const ZImg*>&, CombineMode);
template ZImg ZImg::combine_Impl<uint64_t>(const std::vector<const ZImg*>&, CombineMode);
template ZImg ZImg::combine_Impl<int8_t>(const std::vector<const ZImg*>&, CombineMode);
template ZImg ZImg::combine_Impl<int16_t>(const std::vector<const ZImg*>&, CombineMode);
template ZImg ZImg::combine_Impl<int32_t>(const std::vector<const ZImg*>&, CombineMode);
template ZImg ZImg::combine_Impl<int64_t>(const std::vector<const ZImg*>&, CombineMode);
template ZImg ZImg::combine_Impl<float>(const std::vector<const ZImg*>&, CombineMode);
template ZImg ZImg::combine_Impl<double>(const std::vector<const ZImg*>&, CombineMode);

template<typename TVoxel>
ZImg& ZImg::normalize_Impl()
{
  TVoxel minV = 0;
  TVoxel maxV = 0;
  computeMinMax(minV, maxV);
  return normalize(minV, maxV);
}

template<typename TVoxel, typename TDesVoxel>
void ZImg::cast_Impl(ZImg &res) const
{
  for (size_t t=0; t<numTimes(); ++t) {
    const TVoxel *srcData = timeData<TVoxel>(t);
    TDesVoxel *desData = res.timeData<TDesVoxel>(t);
    for (size_t v=0; v<timeVoxelNumber(); ++v) {
      desData[v] = static_cast<TDesVoxel>(srcData[v]);
    }
  }
}

template<typename TVoxel>
void ZImg::resize_Impl(ZImg &res, Interpolant interpolant, bool antialiasing, bool antialiasingForNearest) const
{
  for (size_t t=0; t<numTimes(); ++t) {
    for (size_t c=0; c<numChannels(); ++c) {
      if (res.depth() == depth()) {
        for (size_t z=0; z<depth(); ++z) {
                    //ZBenchTimer bt;
                    //bt.start();
          image2DResize(planeData<TVoxel>(z,c,t), width(), height(),
                        res.planeData<TVoxel>(z,c,t), res.width(), res.height(),
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
        image3DResize(channelData<TVoxel>(c,t), width(), height(), depth(),
                      res.channelData<TVoxel>(c,t), res.width(), res.height(), res.depth(),
                      interpolant, antialiasing, antialiasingForNearest);
      }
    }
  }
}

template<typename TVoxel>
void ZImg::blockDownsampled_Impl(ZImg &res, size_t blockWidth, size_t blockHeight, size_t blockDepth, CombineMode mode) const
{
  if (mode == CombineMode::Mean) {
    for (size_t t=0; t<res.numTimes(); ++t) {
      for (size_t c=0; c<res.numChannels(); ++c) {
        TVoxel *resData = res.channelData<TVoxel>(c,t);
        size_t resOffset = 0;
        for (size_t z=0; z<res.depth(); ++z) {
          size_t zStart = z * blockDepth;
          size_t zEnd = std::min((z+1) * blockDepth, depth());
          size_t zSpan = zEnd - zStart;
          for (size_t y=0; y<res.height(); ++y) {
            size_t yStart = y * blockHeight;
            size_t yEnd = std::min((y+1) * blockHeight, height());
            size_t yzSpan = (yEnd - yStart) * zSpan;
            for (size_t x=0; x<res.width(); ++x) {
              size_t xStart = x * blockWidth;
              size_t xEnd = std::min((x+1) * blockWidth, width());
              size_t numVoxel = (xEnd - xStart) * yzSpan;
              const TVoxel *srcData = data<TVoxel>(xStart, yStart, zStart,c,t);
              size_t srcOffset = 0;
              double sum = 0;
              for (size_t mz=zStart; mz<zEnd; ++mz) {
                for (size_t my=yStart; my<yEnd; ++my) {
                  for (size_t mx=xStart; mx<xEnd; ++mx) {
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
  } else if (mode == CombineMode::Median) {
    std::vector<TVoxel> buf(blockWidth * blockHeight * blockDepth);
    for (size_t t=0; t<res.numTimes(); ++t) {
      for (size_t c=0; c<res.numChannels(); ++c) {
        TVoxel *resData = res.channelData<TVoxel>(c,t);
        size_t resOffset = 0;
        for (size_t z=0; z<res.depth(); ++z) {
          size_t zStart = z * blockDepth;
          size_t zEnd = std::min((z+1) * blockDepth, depth());
          for (size_t y=0; y<res.height(); ++y) {
            size_t yStart = y * blockHeight;
            size_t yEnd = std::min((y+1) * blockHeight, height());
            for (size_t x=0; x<res.width(); ++x) {
              size_t xStart = x * blockWidth;
              size_t xEnd = std::min((x+1) * blockWidth, width());
              const TVoxel *srcData = data<TVoxel>(xStart, yStart, zStart,c,t);
              size_t srcOffset = 0;
              size_t bufIdx = 0;
              for (size_t mz=zStart; mz<zEnd; ++mz) {
                for (size_t my=yStart; my<yEnd; ++my) {
                  for (size_t mx=xStart; mx<xEnd; ++mx) {
                    buf[bufIdx++] = srcData[srcOffset++];
                  }
                  srcOffset += rowVoxelNumber() - (xEnd - xStart);
                }
                srcOffset += planeVoxelNumber() - (yEnd - yStart) * rowVoxelNumber();
              }
              resData[resOffset++] = static_cast<TVoxel>(medianInPlace(buf.begin(), buf.begin()+bufIdx));
            }
          }
        }
      }
    }
  } else if (mode == CombineMode::Min) {
    for (size_t t=0; t<res.numTimes(); ++t) {
      for (size_t c=0; c<res.numChannels(); ++c) {
        TVoxel *resData = res.channelData<TVoxel>(c,t);
        size_t resOffset = 0;
        for (size_t z=0; z<res.depth(); ++z) {
          size_t zStart = z * blockDepth;
          size_t zEnd = std::min((z+1) * blockDepth, depth());
          for (size_t y=0; y<res.height(); ++y) {
            size_t yStart = y * blockHeight;
            size_t yEnd = std::min((y+1) * blockHeight, height());
            for (size_t x=0; x<res.width(); ++x) {
              size_t xStart = x * blockWidth;
              size_t xEnd = std::min((x+1) * blockWidth, width());
              const TVoxel *srcData = data<TVoxel>(xStart, yStart, zStart,c,t);
              size_t srcOffset = 0;
              for (size_t mz=zStart; mz<zEnd; ++mz) {
                for (size_t my=yStart; my<yEnd; ++my) {
                  for (size_t mx=xStart; mx<xEnd; ++mx) {
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
  } else if (mode == CombineMode::Max) {
    for (size_t t=0; t<res.numTimes(); ++t) {
      for (size_t c=0; c<res.numChannels(); ++c) {
        TVoxel *resData = res.channelData<TVoxel>(c,t);
        size_t resOffset = 0;
        for (size_t z=0; z<res.depth(); ++z) {
          size_t zStart = z * blockDepth;
          size_t zEnd = std::min((z+1) * blockDepth, depth());
          for (size_t y=0; y<res.height(); ++y) {
            size_t yStart = y * blockHeight;
            size_t yEnd = std::min((y+1) * blockHeight, height());
            for (size_t x=0; x<res.width(); ++x) {
              size_t xStart = x * blockWidth;
              size_t xEnd = std::min((x+1) * blockWidth, width());
              const TVoxel *srcData = data<TVoxel>(xStart, yStart, zStart,c,t);
              size_t srcOffset = 0;
              for (size_t mz=zStart; mz<zEnd; ++mz) {
                for (size_t my=yStart; my<yEnd; ++my) {
                  for (size_t mx=xStart; mx<xEnd; ++mx) {
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
  }
}

template<typename TVoxel, typename TValue>
void ZImg::computeMinMax_Impl(TValue &minV, TValue &maxV) const
{
  if (isEmpty()) {
    minV = 0;
    maxV = 0;
    return;
  }
  for (size_t t=0; t<numTimes(); ++t) {
    const TVoxel* data = timeData<TVoxel>(t);
    std::pair<const TVoxel*,const TVoxel*> res = minMaxElement(data, data+timeVoxelNumber(), true);
    if (t==0) {
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
  for (size_t t=0; t<numTimes(); ++t) {
    TVoxel* data = timeData<TVoxel>(t);
    const TVoxelRhs* rhsData = rhs.timeData<TVoxelRhs>(t);
    saturate_add(data, rhsData, timeVoxelNumber(), data);
  }
}

template<typename TVoxel, typename TVoxelRhs>
void ZImg::subImg_Impl(const ZImg& rhs)
{
  for (size_t t=0; t<numTimes(); ++t) {
    TVoxel* data = timeData<TVoxel>(t);
    const TVoxelRhs* rhsData = rhs.timeData<TVoxelRhs>(t);
    saturate_sub(data, rhsData, timeVoxelNumber(), data);
  }
}

template<typename TVoxel, typename TVoxelRhs>
void ZImg::mulImg_Impl(const ZImg& rhs)
{
  for (size_t t=0; t<numTimes(); ++t) {
    TVoxel* data = timeData<TVoxel>(t);
    const TVoxelRhs* rhsData = rhs.timeData<TVoxelRhs>(t);
    saturate_mul(data, rhsData, timeVoxelNumber(), data);
  }
}

template<typename TVoxel, typename TVoxelRhs>
void ZImg::divImg_Impl(const ZImg& rhs)
{
  for (size_t t=0; t<numTimes(); ++t) {
    TVoxel* data = timeData<TVoxel>(t);
    const TVoxelRhs* rhsData = rhs.timeData<TVoxelRhs>(t);
    saturate_div(data, rhsData, timeVoxelNumber(), data);
  }
}

template<typename TVoxel, typename TVoxelRhs>
void ZImg::secureDivImg_Impl(const ZImg& rhs)
{
  for (size_t t=0; t<numTimes(); ++t) {
    TVoxel* data = timeData<TVoxel>(t);
    const TVoxelRhs* rhsData = rhs.timeData<TVoxelRhs>(t);
    saturate_div_secure(data, rhsData, timeVoxelNumber(), data);
  }
}

template<typename TVoxel>
void ZImg::histogram_Impl(std::vector<size_t>& res, TVoxel minData, TVoxel maxData) const
{
  if ((voxelFormat() == VoxelFormat::Float && maxData <= minData) ||
      (voxelFormat() != VoxelFormat::Float && maxData < minData)) {
    throw ZImgException(QString("Invalid histogram range %1:%2").arg(minData).arg(maxData));
  }

  // special case
  if (voxelFormat() != VoxelFormat::Float) {
    size_t numData = maxData - minData + 1_usize;
    if (numData == res.size()) {
      if (minData == dataRangeMin<TVoxel>() && maxData == dataRangeMax<TVoxel>()) {
        for (size_t t=0; t<numTimes(); ++t) {
          const TVoxel* data = timeData<TVoxel>(t);
          for (size_t v=0; v<timeVoxelNumber(); ++v) {
            res[data[v] - minData] += 1;
          }
        }
      } else {
        for (size_t t=0; t<numTimes(); ++t) {
          const TVoxel* data = timeData<TVoxel>(t);
          for (size_t v=0; v<timeVoxelNumber(); ++v) {
            if (data[v] >= minData && data[v] <= maxData)
              res[data[v] - minData] += 1;
          }
        }
      }
      return;
    }
  }

  if (voxelFormat() == VoxelFormat::Float) {
    double scale = res.size() / (maxData - minData);
    for (size_t t=0; t<numTimes(); ++t) {
      const TVoxel* data = timeData<TVoxel>(t);
      for (size_t v=0; v<timeVoxelNumber(); ++v) {
        if (data[v] >= minData && data[v] <= maxData) {
          size_t idx = (data[v] - minData) * scale;
          if (idx == res.size()) idx = res.size() - 1;  // only maxData map to index that out of bound
          res[idx] += 1;
        }
      }
    }
  } else {
    double scale = res.size() / (maxData + 1. - minData);
    for (size_t t=0; t<numTimes(); ++t) {
      const TVoxel* data = timeData<TVoxel>(t);
      for (size_t v=0; v<timeVoxelNumber(); ++v) {
        if (data[v] >= minData && data[v] <= maxData) {
          size_t idx = (data[v] - minData) * scale;
          res[idx] += 1;
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
  if ((voxelFormat() == VoxelFormat::Float && maxData <= minData) ||
      (voxelFormat() != VoxelFormat::Float && maxData < minData)) {
    throw ZImgException(QString("Invalid histogram range %1:%2").arg(minData).arg(maxData));
  }

  // special case
  if (voxelFormat() != VoxelFormat::Float) {
    size_t numData = maxData - minData + 1_usize;
    if (numData == res.size()) {
      if (minData == dataRangeMin<TVoxel>() && maxData == dataRangeMax<TVoxel>()) {
        for (size_t t=0; t<numTimes(); ++t) {
          const TVoxel* data = timeData<TVoxel>(t);
          const TMaskVoxel* maskData = mask.timeData<TMaskVoxel>(t);
          for (size_t v=0; v<timeVoxelNumber(); ++v) {
            if (maskData[v])
              res[data[v] - minData] += 1;
          }
        }
      } else {
        for (size_t t=0; t<numTimes(); ++t) {
          const TVoxel* data = timeData<TVoxel>(t);
          const TMaskVoxel* maskData = mask.timeData<TMaskVoxel>(t);
          for (size_t v=0; v<timeVoxelNumber(); ++v) {
            if (maskData[v] && data[v] >= minData && data[v] <= maxData)
              res[data[v] - minData] += 1;
          }
        }
      }
      return;
    }
  }

  if (voxelFormat() == VoxelFormat::Float) {
    double scale = res.size() / (maxData - minData);
    for (size_t t=0; t<numTimes(); ++t) {
      const TVoxel* data = timeData<TVoxel>(t);
      const TMaskVoxel* maskData = mask.timeData<TMaskVoxel>(t);
      for (size_t v=0; v<timeVoxelNumber(); ++v) {
        if (maskData[v] && data[v] >= minData && data[v] <= maxData) {
          size_t idx = (data[v] - minData) * scale;
          if (idx == res.size()) idx = res.size() - 1;  // only maxData map to index that out of bound
          res[idx] += 1;
        }
      }
    }
  } else {
    double scale = res.size() / (maxData + 1. - minData);
    for (size_t t=0; t<numTimes(); ++t) {
      const TVoxel* data = timeData<TVoxel>(t);
      const TMaskVoxel* maskData = mask.timeData<TMaskVoxel>(t);
      for (size_t v=0; v<timeVoxelNumber(); ++v) {
        if (maskData[v] && data[v] >= minData && data[v] <= maxData) {
          size_t idx = (data[v] - minData) * scale;
          res[idx] += 1;
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
    for (size_t t=0; t<numTimes(); ++t) {
      for (size_t c=0; c<numChannels(); ++c) {
        TVoxel* data = channelData<TVoxel>(c,t);
        image3DFlip(data, width(), height(), depth(), dim);
      }
    }
  }
}

template<typename TVoxel>
void ZImg::reflect_Impl()
{
  for (size_t t=0; t<numTimes(); ++t) {
    size_t j = timeVoxelNumber() - 1;
    TVoxel* data = timeData<TVoxel>(t);
    for (size_t i=0; i<timeVoxelNumber()/2; ++i, --j) {
      std::swap(data[i], data[j]);
    }
  }
}

// only for dim 0, 1, 2
template<typename TVoxel>
void ZImg::cumulativeSum_Impl(ZImg& res, Dimension dim) const
{
  if (dim == Dimension::Z) {
    for (size_t t=0; t<numTimes(); ++t) {
      for (size_t c=0; c<numChannels(); ++c) {
        for (size_t z=1; z<depth(); ++z) {
          TVoxel* data = res.planeData<TVoxel>(z,c,t);
          TVoxel* prevData = res.planeData<TVoxel>(z-1,c,t);
          saturate_add(data, prevData, planeVoxelNumber(), data);
        }
      }
    }
  } else if (dim == Dimension::Y) {
    for (size_t t=0; t<numTimes(); ++t) {
      for (size_t c=0; c<numChannels(); ++c) {
        for (size_t z=0; z<depth(); ++z) {
          for (size_t y=1; y<height(); ++y) {
            TVoxel* data = res.rowData<TVoxel>(y,z,c,t);
            TVoxel* prevData = res.rowData<TVoxel>(y-1,z,c,t);
            saturate_add(data, prevData, rowVoxelNumber(), data);
          }
        }
      }
    }
  } else if (dim == Dimension::X) {
    for (size_t t=0; t<numTimes(); ++t) {
      for (size_t c=0; c<numChannels(); ++c) {
        for (size_t z=0; z<depth(); ++z) {
          for (size_t y=0; y<height(); ++y) {
            TVoxel* data = res.data<TVoxel>(1,y,z,c,t);
            for (size_t x=1; x<width(); ++x, ++data) {
              *data = saturate_add(*data, *(data-1));
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
    for (size_t t=0; t<res.numTimes(); ++t) {
      for (size_t c=0; c<res.numChannels(); ++c) {
        for (size_t z=tdepth-1; z<res.depth(); ++z) {
          for (size_t y=theight-1; y<res.height(); ++y) {
            memcpy(res.rowData(y,z,c,t),
                   rowData(y-theight+1,z-tdepth+1,c,t),
                   res.rowByteNumber());
          }
        }
      }
    }
  } else if (twidth > 1) {  // first dim
    if (twidth <= width()) {
      for (size_t t=0; t<res.numTimes(); ++t) {
        for (size_t c=0; c<res.numChannels(); ++c) {
          for (size_t z=tdepth-1; z<res.depth(); ++z) {
            for (size_t y=theight-1; y<res.height(); ++y) {
              TVoxel* resData = res.rowData<TVoxel>(y,z,c,t);
              const TVoxel* origData = rowData<TVoxel>(y-theight+1,z-tdepth+1,c,t);
              resData[0] = origData[0];
              for (size_t x=1; x<twidth; ++x) {
                resData[x] = saturate_add(origData[x], resData[x-1]);
              }
              for (size_t x=twidth; x<width(); ++x) {
                resData[x] = saturate_sub(saturate_add(origData[x], resData[x-1]), origData[x-twidth]);
              }
              for (size_t x=width(); x<res.width(); ++x) {
                resData[x] = saturate_sub(resData[x-1], origData[x-twidth]);
              }
            }
          }
        }
      }
    } else {
      for (size_t t=0; t<res.numTimes(); ++t) {
        for (size_t c=0; c<res.numChannels(); ++c) {
          for (size_t z=tdepth-1; z<res.depth(); ++z) {
            for (size_t y=theight-1; y<res.height(); ++y) {
              TVoxel* resData = res.rowData<TVoxel>(y,z,c,t);
              const TVoxel* origData = rowData<TVoxel>(y-theight+1,z-tdepth+1,c,t);
              resData[0] = origData[0];
              for (size_t x=1; x<width(); ++x) {
                resData[x] = saturate_add(origData[x], resData[x-1]);
              }
              for (size_t x=width(); x<twidth; ++x) {
                resData[x] = resData[x-1];
              }
              for (size_t x=twidth; x<res.width(); ++x) {
                resData[x] = saturate_sub(resData[x-1], origData[x-twidth]);
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
      for (size_t t=0; t<res.numTimes(); ++t) {
        for (size_t c=0; c<res.numChannels(); ++c) {
          for (size_t z=tdepth-1; z<res.depth(); ++z) {
            // first row
            memcpy(res.rowData(0,z,c,t),
                   res.rowData(theight-1,z,c,t),
                   rowByteNum);
            // save to subtract
            memcpy(bufRow.data(), res.rowData(0,z,c,t), rowByteNum);
            // other
            for (size_t y=1; y<theight; ++y) {
              TVoxel* resData = res.rowData<TVoxel>(y,z,c,t);
              for (size_t v=0; v<rowVoxelNum; ++v) {
                resData[v] = saturate_add(resData[v - rowVoxelNum],
                    resData[v + dataOffset]);
              }
            }
            for (size_t y=theight; y<height(); ++y) {
              TVoxel* resData = res.rowData<TVoxel>(y,z,c,t);
              memcpy(buf.data(), resData, rowByteNum);
              for (size_t v=0; v<rowVoxelNum; ++v) {
                resData[v] = saturate_sub(saturate_add(resData[v - rowVoxelNum],
                                          resData[v + dataOffset]), bufRow[v]);
              }
              memcpy(bufRow.data(), buf.data(), rowByteNum);
            }
            for (size_t y=height(); y<res.height(); ++y) {
              TVoxel* resData = res.rowData<TVoxel>(y,z,c,t);
              memcpy(buf.data(), resData, rowByteNum);
              for (size_t v=0; v<rowVoxelNum; ++v) {
                resData[v] = saturate_sub(resData[v - rowVoxelNum], bufRow[v]);
              }
              memcpy(bufRow.data(), buf.data(), rowByteNum);
            }
          }
        }
      }
    } else {
      for (size_t t=0; t<res.numTimes(); ++t) {
        for (size_t c=0; c<res.numChannels(); ++c) {
          for (size_t z=tdepth-1; z<res.depth(); ++z) {
            // first row
            memcpy(res.rowData(0,z,c,t),
                   res.rowData(theight-1,z,c,t),
                   rowByteNum);
            // save to subtract
            memcpy(bufRow.data(), res.rowData(0,z,c,t), rowByteNum);
            // other
            for (size_t y=1; y<height(); ++y) {
              TVoxel* resData = res.rowData<TVoxel>(y,z,c,t);
              for (size_t v=0; v<rowVoxelNum; ++v) {
                resData[v] = saturate_add(resData[v - rowVoxelNum],
                    resData[v + dataOffset]);
              }
            }
            for (size_t y=height(); y<theight; ++y) {
              TVoxel* resData = res.rowData<TVoxel>(y,z,c,t);
              memcpy(resData, resData-rowVoxelNum, rowByteNum);
            }
            for (size_t y=theight; y<res.height(); ++y) {
              TVoxel* resData = res.rowData<TVoxel>(y,z,c,t);
              memcpy(buf.data(), resData, rowByteNum);
              for (size_t v=0; v<rowVoxelNum; ++v) {
                resData[v] = saturate_sub(resData[v - rowVoxelNum], bufRow[v]);
              }
              memcpy(bufRow.data(), buf.data(), rowByteNum);
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
      for (size_t t=0; t<res.numTimes(); ++t) {
        for (size_t c=0; c<res.numChannels(); ++c) {
          // first plane
          memcpy(res.planeData(0,c,t),
                 res.planeData(tdepth-1,c,t),
                 planeByteNum);
          // save to subtract
          memcpy(bufPlane.data(), res.planeData(0,c,t), planeByteNum);
          // other
          for (size_t z=1; z<tdepth; ++z) {
            TVoxel* resData = res.planeData<TVoxel>(z,c,t);
            for (size_t v=0; v<planeVoxelNum; ++v) {
              resData[v] = saturate_add(resData[v - planeVoxelNum],
                  resData[v + dataOffset]);
            }
          }
          for (size_t z=tdepth; z<depth(); ++z) {
            TVoxel* resData = res.planeData<TVoxel>(z,c,t);
            memcpy(buf.data(), resData, planeByteNum);
            for (size_t v=0; v<planeVoxelNum; ++v) {
              resData[v] = saturate_sub(saturate_add(resData[v - planeVoxelNum],
                                        resData[v + dataOffset]), bufPlane[v]);
            }
            memcpy(bufPlane.data(), buf.data(), planeByteNum);
          }
          for (size_t z=depth(); z<res.depth(); ++z) {
            TVoxel* resData = res.planeData<TVoxel>(z,c,t);
            memcpy(buf.data(), resData, planeByteNum);
            for (size_t v=0; v<planeVoxelNum; ++v) {
              resData[v] = saturate_sub(resData[v - planeVoxelNum], bufPlane[v]);
            }
            memcpy(bufPlane.data(), buf.data(), planeByteNum);
          }
        }
      }
    } else {
      for (size_t t=0; t<res.numTimes(); ++t) {
        for (size_t c=0; c<res.numChannels(); ++c) {
          // first plane
          memcpy(res.planeData(0,c,t),
                 res.planeData(tdepth-1,c,t),
                 planeByteNum);
          // save to subtract
          memcpy(bufPlane.data(), res.planeData(0,c,t), planeByteNum);
          // other
          for (size_t z=1; z<depth(); ++z) {
            TVoxel* resData = res.planeData<TVoxel>(z,c,t);
            for (size_t v=0; v<planeVoxelNum; ++v) {
              resData[v] = saturate_add(resData[v - planeVoxelNum],
                  resData[v + dataOffset]);
            }
          }
          for (size_t z=depth(); z<tdepth; ++z) {
            TVoxel* resData = res.planeData<TVoxel>(z,c,t);
            memcpy(resData, resData-planeVoxelNum, planeByteNum);
          }
          for (size_t z=tdepth; z<res.depth(); ++z) {
            TVoxel* resData = res.planeData<TVoxel>(z,c,t);
            memcpy(buf.data(), resData, planeByteNum);
            for (size_t v=0; v<planeVoxelNum; ++v) {
              resData[v] = saturate_sub(resData[v - planeVoxelNum], bufPlane[v]);
            }
            memcpy(bufPlane.data(), buf.data(), planeByteNum);
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
  for (size_t t=0; t<res.numTimes(); ++t) {
    for (size_t c=0; c<res.numChannels(); ++c) {
      TVoxel* desData = res.channelData<TVoxel>(c,t);
      size_t desOffset = 0;
      for (size_t z=0; z<res.depth(); ++z) {
        size_t blockZStart = std::max(0, static_cast<int>(zStart+z)-static_cast<int>(tdepth)+1);
        size_t blockZEnd = std::min(depth(), zStart+z+1);
        for (size_t y=0; y<res.height(); ++y) {
          size_t blockYStart = std::max(0, static_cast<int>(yStart+y)-static_cast<int>(theight)+1);
          size_t blockYEnd = std::min(height(), yStart+y+1);
          TVoxel inc = 0;
          TVoxel dec = 0;
          for (size_t x=0; x<res.width(); ++x) {
            int tleft = static_cast<int>(xStart+x)-static_cast<int>(twidth)+1;
            size_t tright = xStart + x + 1;
            size_t blockXStart = std::max(0, tleft);
            size_t blockXEnd = std::min(width(), tright);
            const TVoxel* srcData = data<TVoxel>(blockXStart, blockYStart, blockZStart, c,t);
            size_t srcOffset = 0;
            if (x == 0) {
              inc = 0;
              dec = 0;
              for (size_t mz=blockZStart; mz<blockZEnd; ++mz) {
                for (size_t my=blockYStart; my<blockYEnd; ++my) {
                  if (tleft >= 0) {
                    dec = saturate_add(dec, srcData[srcOffset]);
                  }
                  if (tright < width()) {
                    inc = saturate_add(inc, srcData[srcOffset+blockXEnd-blockXStart]);
                  }
                  for (size_t mx=blockXStart; mx<blockXEnd; ++mx) {
                    desData[desOffset] = saturate_add(desData[desOffset], srcData[srcOffset]);
                    ++srcOffset;
                  }
                  srcOffset += srcRowNum - (blockXEnd-blockXStart);
                }
                srcOffset += srcPlaneNum - (blockYEnd-blockYStart) * srcRowNum;
              }
            } else {
              desData[desOffset] = saturate_sub(saturate_add(desData[desOffset-1], inc), dec);
              inc = 0;
              dec = 0;
              for (size_t mz=blockZStart; mz<blockZEnd; ++mz) {
                for (size_t my=blockYStart; my<blockYEnd; ++my) {
                  if (tleft >= 0) {
                    dec = saturate_add(dec, srcData[srcOffset]);
                  }
                  if (tright < width()) {
                    inc = saturate_add(inc, srcData[srcOffset+blockXEnd-blockXStart]);
                  }
                  srcOffset += srcRowNum;
                }
                srcOffset += srcPlaneNum - (blockYEnd-blockYStart) * srcRowNum;
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
    for (size_t t=0; t<numTimes(); ++t) {
      TVoxel* data = timeData<TVoxel>(t);
      for (size_t v=0; v<timeVoxelNumber(); ++v) {
        if (data[v] >= threshold)
          data[v] = outsideValue;
      }
    }
  } else if (threMode == ThresholdMode::ExcludeThreshold) {
    for (size_t t=0; t<numTimes(); ++t) {
      TVoxel* data = timeData<TVoxel>(t);
      for (size_t v=0; v<timeVoxelNumber(); ++v) {
        if (data[v] > threshold)
          data[v] = outsideValue;
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
    for (size_t t=0; t<numTimes(); ++t) {
      TVoxel* data = timeData<TVoxel>(t);
      for (size_t v=0; v<timeVoxelNumber(); ++v) {
        if (data[v] <= threshold)
          data[v] = outsideValue;
      }
    }
  } else if (threMode == ThresholdMode::ExcludeThreshold) {
    for (size_t t=0; t<numTimes(); ++t) {
      TVoxel* data = timeData<TVoxel>(t);
      for (size_t v=0; v<timeVoxelNumber(); ++v) {
        if (data[v] < threshold)
          data[v] = outsideValue;
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
    for (size_t t=0; t<numTimes(); ++t) {
      const TVoxel* data = timeData<TVoxel>(t);
      uint8_t* resData = res.timeData<uint8_t>(t);
      for (size_t v=0; v<timeVoxelNumber(); ++v) {
        if (data[v] >= threshold)
          resData[v] = 1;
      }
    }
  } else if (threMode == ThresholdMode::ExcludeThreshold) {
    for (size_t t=0; t<numTimes(); ++t) {
      const TVoxel* data = timeData<TVoxel>(t);
      uint8_t* resData = res.timeData<uint8_t>(t);
      for (size_t v=0; v<timeVoxelNumber(); ++v) {
        if (data[v] > threshold)
          resData[v] = 1;
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
  for (size_t t=0; t<numTimes(); ++t) {
    for (size_t c=0; c<numChannels(); ++c) {
      for (size_t z=0; z<depth(); ++z) {
        for (size_t y=0; y<height(); ++y) {
          os << t << ":" << c << ":" << z << ":" << y << ": ";
          const TVoxel* data = rowData<TVoxel>(y,z,c,t);
          for (size_t x=0; x<width(); ++x) {
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

}  // namespace nim
