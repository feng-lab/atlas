#include "zimgmetaimage.h"

#include "zimgsliceprovider.h"
#include "zlog.h"
#include <metaImage.h>
#include <QFile>
#include <QStringList>
#include <QFileInfo>
#include <QDir>

namespace nim {

QString ZImgMetaImage::shortName() const
{
  return "MetaImage";
}

QString ZImgMetaImage::fullName() const
{
  return "Kitware MetaImage";
}

QStringList ZImgMetaImage::extensions() const
{
  QStringList res;
  res << "mhd" << "mha";
  return res;
}

void ZImgMetaImage::readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                             std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                             std::vector<std::set<std::array<size_t, 3>>>* pyramidalRatios)
{
  MetaImage metaImage;
  if (!metaImage.Read(QFile::encodeName(filename).constData(), false, nullptr)) {
    throw ZIOException("Can not read file");
  }
  infos.resize(1);
  parseInfo(metaImage, infos[0]);
  LOG(INFO) << infos[0].toQString();

  createDefaultSubBlocks(filename, infos, subBlocks, pyramidalRatios);
}

void ZImgMetaImage::readMetadata(const QString& /*filename*/, ZImgMetadata& /*meta*/, size_t /*scene*/)
{
}

void ZImgMetaImage::readThumbnail(const QString& /*filename*/, ZImgThumbernail& /*thumbnail*/,
                                  const ZImgRegion& /*region*/, size_t /*scene*/)
{
}

void ZImgMetaImage::readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }
  MetaImage metaImage;
  if (!metaImage.Read(QFile::encodeName(filename).constData(), false, nullptr)) {
    throw ZIOException("Can not read metaImage");
  }
  //metaImage.PrintInfo();
  ZImgInfo imgInfo;
  parseInfo(metaImage, imgInfo);
  if (imgInfo.isEmpty()) {
    throw ZIOException("Empty Image");
  }

  if (region.isEmpty() || !region.isValid(imgInfo)) {
    throw ZIOException(
      QString("Invalid image region. Image info: '%1', region: '%2'").arg(imgInfo.toQString()).arg(region.toQString()));
  }

  if (region.containsWholeImg(imgInfo)) {
    img = ZImg(imgInfo);
    metaImage.Clear();
    metaImage.Read(QFile::encodeName(filename).constData(), true, img.channelData<uint8_t>(0));
    if (imgInfo.numChannels > 1) {
      ZImg tpImg(imgInfo);
      CXYZtoXYZC(img, tpImg);
      img.swap(tpImg);
    }
  } else {
    ZImgInfo clipInfo = region.clip(imgInfo);
    bool clipChannel = clipInfo.numChannels != imgInfo.numChannels;
    if (clipChannel) {
      clipInfo.numChannels = imgInfo.numChannels;
    }

    ZImg tmpImg(clipInfo);
    metaImage.Clear();
    ZImgRegion rgn = region;
    rgn.resolveRegionEnd(imgInfo);
    int indexMin[3];
    indexMin[0] = rgn.start.x;
    indexMin[1] = rgn.start.y;
    indexMin[2] = rgn.start.z;
    int indexMax[3];
    indexMax[0] = rgn.end.x - 1;
    indexMax[1] = rgn.end.y - 1;
    indexMax[2] = rgn.end.z - 1;
    metaImage.ReadROI(indexMin, indexMax, QFile::encodeName(filename).constData(), true,
                      tmpImg.channelData<uint8_t>(0));
    if (clipInfo.numChannels > 1) {
      ZImg tpImg(clipInfo);
      CXYZtoXYZC(tmpImg, tpImg);
      tmpImg.swap(tpImg);
    }

    if (clipChannel) {
      ZImgRegion crgn;
      crgn.start.c = rgn.start.c;
      crgn.end.c = rgn.end.c;
      img = tmpImg.crop(rgn);
    } else {
      img.swap(tmpImg);
    }
  }
}

void ZImgMetaImage::checkImgBeforeWriting(const QString& filename, const ZImgInfo& info,
                                          const ZImgWriteParameters& paras)
{
  ZImgFormat::checkImgBeforeWriting(filename, info, paras);
  if (!(paras.compression == Compression::AUTO ||
        paras.compression == Compression::NONE ||
        paras.compression == Compression::DEFLATE)) {
    throw ZIOException(QString("compression %1 is not supported").arg(enumToString(paras.compression)));
  }
  if (info.numTimes != 1) {
    throw ZIOException("time sequence image is not supported");
  }
}

void ZImgMetaImage::writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, img.info(), paras);

  bool multipleChannel = img.numChannels() > 1;

  ZImg tmpImg;
  if (multipleChannel) {
    tmpImg = ZImg(img.info());
    XYZCtoCXYZ(img, tmpImg);
  }

  int nDims = 3;
  if (img.depth() == 1) {
    nDims = 2;
  }
  int dimSize[3];
  dimSize[0] = img.width();
  dimSize[1] = img.height();
  dimSize[2] = img.depth();
  MET_DistanceUnitsEnumType distanceUnitType = MET_DISTANCE_UNITS_UNKNOWN;
  float elementSpacing[3];
  if (img.voxelSizeUnit() == VoxelSizeUnit::none) {
    elementSpacing[0] = img.voxelSizeX();
    elementSpacing[1] = img.voxelSizeY();
    elementSpacing[2] = img.voxelSizeZ();
  } else {
    distanceUnitType = MET_DISTANCE_UNITS_UM;
    elementSpacing[0] = img.info().voxelSizeXInUm();
    elementSpacing[1] = img.info().voxelSizeYInUm();
    elementSpacing[2] = img.info().voxelSizeZInUm();
  }
  MET_ValueEnumType elementType = MET_CHAR;
  switch (img.voxelFormat()) {
    case VoxelFormat::Signed:
      switch (img.bytesPerVoxel()) {
        case 1:
          elementType = multipleChannel ? MET_CHAR_ARRAY : MET_CHAR;
          break;
        case 2:
          elementType = multipleChannel ? MET_SHORT_ARRAY : MET_SHORT;
          break;
        case 4:
          elementType = multipleChannel ? MET_INT_ARRAY : MET_INT;
          break;
        case 8:
          elementType = multipleChannel ? MET_LONG_LONG_ARRAY : MET_LONG_LONG;
          break;
        default:
          CHECK(false);
          break;
      }
      break;
    case VoxelFormat::Unsigned:
      switch (img.bytesPerVoxel()) {
        case 1:
          elementType = multipleChannel ? MET_UCHAR_ARRAY : MET_UCHAR;
          break;
        case 2:
          elementType = multipleChannel ? MET_USHORT_ARRAY : MET_USHORT;
          break;
        case 4:
          elementType = multipleChannel ? MET_UINT_ARRAY : MET_UINT;
          break;
        case 8:
          elementType = multipleChannel ? MET_ULONG_LONG_ARRAY : MET_ULONG_LONG;
          break;
        default:
          CHECK(false);
          break;
      }
      break;
    case VoxelFormat::Float:
      switch (img.bytesPerVoxel()) {
        case 4:
          elementType = multipleChannel ? MET_FLOAT_ARRAY : MET_FLOAT;
          break;
        case 8:
          elementType = multipleChannel ? MET_DOUBLE_ARRAY : MET_DOUBLE;
          break;
        default:
          CHECK(false);
          break;
      }
  }

  MetaImage metaImage(nDims, dimSize, elementSpacing, elementType, img.numChannels(),
                      multipleChannel ? const_cast<uint8_t*>(tmpImg.channelData(0))
                                      : const_cast<uint8_t*>(img.channelData(0)));

  metaImage.DistanceUnits(distanceUnitType);
  metaImage.CompressedData(paras.compression != Compression::NONE);

  if (!metaImage.Write(QFile::encodeName(filename).constData())) {
    throw ZIOException("Can not write metaimage");
  }
}

bool ZImgMetaImage::supportRead() const
{
  return true;
}

bool ZImgMetaImage::supportWrite() const
{
  return true;
}

void ZImgMetaImage::parseInfo(const MetaImage& metaImage, ZImgInfo& info)
{
  int ndims = metaImage.NDims();
  if (ndims == 1) {
    info.width = metaImage.DimSize(0);
    info.height = 1;
    info.depth = 1;
  } else if (ndims == 2) {
    info.width = metaImage.DimSize(0);
    info.height = metaImage.DimSize(1);
    info.depth = 1;
  } else if (ndims == 3) {
    info.width = metaImage.DimSize(0);
    info.height = metaImage.DimSize(1);
    info.depth = metaImage.DimSize(2);
  } else {
    throw ZIOException(QString("NDims not supported: %1.").arg(ndims));
  }
  info.numChannels = metaImage.ElementNumberOfChannels();
  info.numTimes = 1;
  switch (metaImage.ElementType()) {
    case MET_CHAR:
    case MET_CHAR_ARRAY:
      info.bytesPerVoxel = 1;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case MET_UCHAR:
    case MET_UCHAR_ARRAY:
      info.bytesPerVoxel = 1;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case MET_SHORT:
    case MET_SHORT_ARRAY:
      info.bytesPerVoxel = 2;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case MET_USHORT:
    case MET_USHORT_ARRAY:
      info.bytesPerVoxel = 2;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case MET_INT:
    case MET_LONG:
    case MET_INT_ARRAY:
    case MET_LONG_ARRAY:
      info.bytesPerVoxel = 4;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case MET_UINT:
    case MET_ULONG:
    case MET_UINT_ARRAY:
    case MET_ULONG_ARRAY:
      info.bytesPerVoxel = 4;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case MET_LONG_LONG:
    case MET_LONG_LONG_ARRAY:
      info.bytesPerVoxel = 8;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case MET_ULONG_LONG:
    case MET_ULONG_LONG_ARRAY:
      info.bytesPerVoxel = 8;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case MET_FLOAT:
    case MET_FLOAT_ARRAY:
      info.bytesPerVoxel = 4;
      info.voxelFormat = VoxelFormat::Float;
      break;
    case MET_DOUBLE:
    case MET_DOUBLE_ARRAY:
      info.bytesPerVoxel = 8;
      info.voxelFormat = VoxelFormat::Float;
      break;
    default:
      throw ZIOException("Not supported ElementType");
      break;
  }

  if (true || metaImage.ElementSizeValid()) { //todo: why?
    switch (metaImage.DistanceUnits()) {
      case MET_DISTANCE_UNITS_CM:
        info.voxelSizeUnit = VoxelSizeUnit::cm;
        break;
      case MET_DISTANCE_UNITS_MM:
        info.voxelSizeUnit = VoxelSizeUnit::mm;
        break;
      default:   // todo: should we default to um ?
        info.voxelSizeUnit = VoxelSizeUnit::um;
        break;
    }
    if (ndims == 1) {
      info.voxelSizeX = metaImage.ElementSize(0);
    } else if (ndims == 2) {
      info.voxelSizeX = metaImage.ElementSize(0);
      info.voxelSizeY = metaImage.ElementSize(1);
    } else if (ndims == 3) {
      info.voxelSizeX = metaImage.ElementSize(0);
      info.voxelSizeY = metaImage.ElementSize(1);
      info.voxelSizeZ = metaImage.ElementSize(2);
    }
  }
  info.createDefaultDescriptions();
}

} // namespace nim

