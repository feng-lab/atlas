#include "zimgitkimage.h"

#include "zimgsliceprovider.h"
#include "zglobal.h"
#include <itkImageIOBase.h>
#include <itkNiftiImageIOFactory.h>
#include <itkNrrdImageIOFactory.h>
#include <itkImageIOFactory.h>
#include <itkSCIFIOImageIOFactory.h>
#include <itkMetaDataObject.h>
//#define ATLAS_SUPPORT_DICOM
#ifdef ATLAS_SUPPORT_DICOM
#include <itkGDCMImageIOFactory.h>
#endif

#include <QFile>
#include <QStringList>
#include <QFileInfo>
#include <QDir>

namespace nim {

ZImgITKImage::ZImgITKImage()
{
  try {
    if (itk::ObjectFactoryBase::GetRegisteredFactories().empty()) {
      itk::NiftiImageIOFactory::RegisterOneFactory();
      itk::NrrdImageIOFactory::RegisterOneFactory();
      if (hasSCIFIOSupport()) {
        itk::SCIFIOImageIOFactory::RegisterOneFactory();
      }
#ifdef ATLAS_SUPPORT_DICOM
      itk::GDCMImageIOFactory::RegisterOneFactory();
#endif
    }
  }
  catch (itk::ExceptionObject& err) {
    throw ZIOException(err.what());
  }
}

QString ZImgITKImage::shortName() const
{
  return "ITKImage";
}

QString ZImgITKImage::fullName() const
{
  return "ITKImage";
}

QStringList ZImgITKImage::extensions() const
{
  QStringList res;

  try {
    for (const auto& pt : itk::ObjectFactoryBase::CreateAllInstance("itkImageIOBase")) {
      if (auto io = dynamic_cast<const itk::ImageIOBase*>(pt.GetPointer())) {
        //LOG(INFO) << "ImageIO: " << io->GetNameOfClass();
        for (const auto& ext : io->GetSupportedReadExtensions()) {
          res.push_back(ext.c_str());
          res.last().remove(0, 1); // remove '.'
        }
      }
    }
    if (hasSCIFIOSupport()) {
      res.push_back("nd2");
    }
#ifdef ATLAS_SUPPORT_DICOM
    res.push_back("dcm");
#endif
  }
  catch (itk::ExceptionObject& err) {
    throw ZIOException(err.what());
  }

  return res;
}

void ZImgITKImage::readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                            std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                            std::vector<std::set<size_t>>* pyramidalRatios)
{
  try {
    itk::ImageIOBase::Pointer imageIO = itk::ImageIOFactory::CreateImageIO(QFile::encodeName(filename).constData(),
                                                                           itk::ImageIOFactory::FileModeType::ReadMode);

    if (imageIO.IsNull())
      throw ZIOException("can not create reader");

    imageIO->SetFileName(QFile::encodeName(filename).constData());
    imageIO->ReadImageInformation();

    bool isNrrd = QString(imageIO->GetNameOfClass()).contains("Nrrd");

    bool isNd2 = filename.endsWith(".nd2", Qt::CaseInsensitive);

    infos.resize(1);
    parseInfo(imageIO.GetPointer(), infos[0], isNd2);

    if (isNrrd) {
      createEmptySubBlocks(infos, subBlocks, pyramidalRatios);
    } else {
      createDefaultSubBlocks(filename, infos, subBlocks, pyramidalRatios);
    }
  }
  catch (itk::ExceptionObject& err) {
    throw ZIOException(err.what());
  }
}

void ZImgITKImage::readMetadata(const QString& filename, ZImgMetadata& meta, size_t /*scene*/)
{
  try {
    itk::ImageIOBase::Pointer imageIO = itk::ImageIOFactory::CreateImageIO(QFile::encodeName(filename).constData(),
                                                                           itk::ImageIOFactory::FileModeType::ReadMode);

    if (imageIO.IsNull())
      throw ZIOException("can not create reader");

    imageIO->SetFileName(QFile::encodeName(filename).constData());
    imageIO->ReadImageInformation();

    parseMetadata(imageIO.GetPointer(), meta);
  }
  catch (itk::ExceptionObject& err) {
    throw ZIOException(err.what());
  }
}

void ZImgITKImage::readThumbnail(const QString& /*filename*/, ZImgThumbernail& /*thumbnail*/,
                                 const ZImgRegion& /*region*/, size_t /*scene*/)
{
  try {
  }
  catch (itk::ExceptionObject& err) {
    throw ZIOException(err.what());
  }
}

void ZImgITKImage::readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene, size_t ratio)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }

  try {
    itk::ImageIOBase::Pointer imageIO = itk::ImageIOFactory::CreateImageIO(QFile::encodeName(filename).constData(),
                                                                           itk::ImageIOFactory::FileModeType::ReadMode);

    if (imageIO.IsNull())
      throw ZIOException("can not create reader");

    imageIO->SetFileName(QFile::encodeName(filename).constData());
    imageIO->ReadImageInformation();

    bool isNrrd = QString(imageIO->GetNameOfClass()).contains("Nrrd");

    bool isNd2 = filename.endsWith(".nd2", Qt::CaseInsensitive);

    ZImgInfo imgInfo;
    parseInfo(imageIO.GetPointer(), imgInfo, isNd2);

    if (region.isEmpty() || !region.isValid(imgInfo)) {
      throw ZIOException(QString("Invalid image region. Image info: '%1', region: '%2'").arg(imgInfo.toQString()).arg(
        region.toQString()));
    }

    if (region.containsWholeImg(imgInfo) || isNrrd) {
      img = ZImg(imgInfo);
      itk::ImageIORegion ioRegion(4);
      ioRegion.SetIndex(0, 0);
      ioRegion.SetIndex(1, 0);
      ioRegion.SetIndex(2, 0);
      ioRegion.SetIndex(3, 0);
      ioRegion.SetSize(0, img.width());
      ioRegion.SetSize(1, img.height());
      ioRegion.SetSize(2, img.depth());
      ioRegion.SetSize(3, img.numTimes());
      imageIO->SetIORegion(ioRegion);
      if (imgInfo.numTimes > 1) {
        std::vector<uint8_t> buf(img.byteNumber());
        imageIO->Read(buf.data());
        if (isNd2) {
          fixDimensionOrder(buf.data(), "XYZTC", img);
        } else {
          fixDimensionOrder(buf.data(), "CXYZT", img);
        }
      } else {
        imageIO->Read(img.channelData(0));
      }
      if (imgInfo.numChannels > 1 && imgInfo.numTimes == 1 &&
          !isNd2) { // if numTimes > 1 or isNd2 then dimension order is already fixed
        ZImg tpImg(imgInfo);
        CXYZtoXYZC(img, tpImg);
        img.swap(tpImg);
      }

      if (isNrrd && !region.containsWholeImg(imgInfo))
        img = img.crop(region);
    } else {
      ZImgRegion rgn = region;
      rgn.resolveRegionEnd(imgInfo);

      bool clipChannel = rgn.start.c != 0 || rgn.end.c != int(imgInfo.numChannels);
      if (clipChannel) {
        rgn.start.c = 0;
        rgn.end.c = imgInfo.numChannels;
      }

      ZImgInfo clipInfo = rgn.clip(imgInfo);
      ZImg tmpImg(clipInfo);

      itk::ImageIORegion ioRegion(4);
      ioRegion.SetIndex(0, rgn.start.x);
      ioRegion.SetIndex(1, rgn.start.y);
      ioRegion.SetIndex(2, rgn.start.z);
      ioRegion.SetIndex(3, rgn.start.t);
      ioRegion.SetSize(0, rgn.end.x - rgn.start.x);
      ioRegion.SetSize(1, rgn.end.y - rgn.start.y);
      ioRegion.SetSize(2, rgn.end.z - rgn.start.z);
      ioRegion.SetSize(3, rgn.end.t - rgn.start.t);
      imageIO->SetIORegion(ioRegion);
      if (clipInfo.numTimes > 1) {
        std::vector<uint8_t> buf(tmpImg.byteNumber());
        imageIO->Read(buf.data());
        if (isNd2) {
          fixDimensionOrder(buf.data(), "XYZTC", tmpImg);
        } else {
          fixDimensionOrder(buf.data(), "CXYZT", tmpImg);
        }
      } else {
        imageIO->Read(tmpImg.channelData(0));
      }
      if (clipInfo.numChannels > 1 && clipInfo.numTimes == 1 &&
          !isNd2) { // if numTimes > 1 or isNd2 then dimension order is already fixed
        ZImg tpImg(clipInfo);
        CXYZtoXYZC(tmpImg, tpImg);
        tmpImg.swap(tpImg);
      }

      if (clipChannel) {
        ZImgRegion crgn;
        crgn.start.c = region.start.c;
        crgn.end.c = region.end.c;
        img = tmpImg.crop(crgn);
      } else {
        img.swap(tmpImg);
      }
    }

    parseMetadata(imageIO.GetPointer(), img.metadataRef());
  }
  catch (itk::ExceptionObject& err) {
    throw ZIOException(err.what());
  }

  if (ratio > 1) {
    img.zoom(1.0 / ratio, 1.0 / ratio);
  }
}

void ZImgITKImage::writeImg(const QString& /*filename*/, const ZImg& /*img*/, Compression /*comp*/)
{
//  CHECK(comp == Compression::AUTO || comp == Compression::NONE || comp == Compression::DEFLATE);
//  if (img.numTimes() != 1) {
//    throw ZIOException("time sequence image is not supported");
//  }

//  bool multipleChannel = img.numChannels() > 1;

//  ZImg tmpImg;
//  if (multipleChannel) {
//    tmpImg = ZImg(img.info());
//    XYZCtoCXYZ(img, tmpImg);
//  }

//  int nDims = 3;
//  if (img.depth() == 1) {
//    nDims = 2;
//  }
//  int dimSize[3];
//  dimSize[0] = img.width();
//  dimSize[1] = img.height();
//  dimSize[2] = img.depth();
//  MET_DistanceUnitsEnumType distanceUnitType = MET_DISTANCE_UNITS_UNKNOWN;
//  float elementSpacing[3];
//  if (img.voxelSizeUnit() == VoxelSizeUnit::Voxel) {
//    elementSpacing[0] = img.voxelSizeX();
//    elementSpacing[1] = img.voxelSizeY();
//    elementSpacing[2] = img.voxelSizeZ();
//  } else {
//    distanceUnitType = MET_DISTANCE_UNITS_UM;
//    elementSpacing[0] = img.voxelSizeXInUm();
//    elementSpacing[1] = img.voxelSizeYInUm();
//    elementSpacing[2] = img.voxelSizeZInUm();
//  }
//  MET_ValueEnumType elementType = MET_CHAR;
//  switch (img.voxelFormat()) {
//  case VoxelFormat::Signed:
//    switch (img.bytesPerVoxel()) {
//    case 1:
//      elementType = multipleChannel ? MET_CHAR_ARRAY : MET_CHAR;
//      break;
//    case 2:
//      elementType = multipleChannel ? MET_SHORT_ARRAY : MET_SHORT;
//      break;
//    case 4:
//      elementType = multipleChannel ? MET_INT_ARRAY : MET_INT;
//      break;
//    case 8:
//      elementType = multipleChannel ? MET_LONG_LONG_ARRAY : MET_LONG_LONG;
//      break;
//    default:
//      CHECK(false);
//      break;
//    }
//    break;
//  case VoxelFormat::Unsigned:
//    switch (img.bytesPerVoxel()) {
//    case 1:
//      elementType = multipleChannel ? MET_UCHAR_ARRAY : MET_UCHAR;
//      break;
//    case 2:
//      elementType = multipleChannel ? MET_USHORT_ARRAY : MET_USHORT;
//      break;
//    case 4:
//      elementType = multipleChannel ? MET_UINT_ARRAY : MET_UINT;
//      break;
//    case 8:
//      elementType = multipleChannel ? MET_ULONG_LONG_ARRAY : MET_ULONG_LONG;
//      break;
//    default:
//      CHECK(false);
//      break;
//    }
//    break;
//  case VoxelFormat::Float:
//    switch (img.bytesPerVoxel()) {
//    case 4:
//      elementType = multipleChannel ? MET_FLOAT_ARRAY : MET_FLOAT;
//      break;
//    case 8:
//      elementType = multipleChannel ? MET_DOUBLE_ARRAY : MET_DOUBLE;
//      break;
//    default:
//      CHECK(false);
//      break;
//    }
//  }

//  MetaImage metaImage(nDims, dimSize, elementSpacing, elementType, img.numChannels(),
//                      multipleChannel ? const_cast<uint8_t*>(tmpImg.channelData(0)) : const_cast<uint8_t*>(img.channelData(0)));

//  metaImage.DistanceUnits(distanceUnitType);
//  metaImage.CompressedData(comp != Compression::NONE);

//  if (!metaImage.Write(QFile::encodeName(filename).constData())) {
//    throw ZIOException("Can not write metaimage");
//  }
}

void ZImgITKImage::writeImg(const QString& /*filename*/,
                            const ZImgSliceProvider& /*imgSliceProvider*/,
                            Compression /*comp*/)
{
//  CHECK(comp == Compression::AUTO || comp == Compression::NONE || comp == Compression::DEFLATE);
//  if (imgSliceProvider.imgInfo().numTimes != 1) {
//    throw ZIOException("time sequence image is not supported");
//  }
//  writeImg(filename, imgSliceProvider.allSlices(0), comp);
}

bool ZImgITKImage::supportRead() const
{
  return true;
}

bool ZImgITKImage::supportWrite() const
{
  return false;
}

void ZImgITKImage::parseInfo(const itk::ImageIOBase* imageIO, ZImgInfo& info, bool isNd2)
{
  uint32_t ndims = imageIO->GetNumberOfDimensions();
  if (ndims == 1) {
    info.width = imageIO->GetDimensions(0);
    info.height = 1;
    info.depth = 1;
    info.numTimes = 1;
  } else if (ndims == 2) {
    info.width = imageIO->GetDimensions(0);
    info.height = imageIO->GetDimensions(1);
    info.depth = 1;
    info.numTimes = 1;
  } else if (ndims == 3) {
    info.width = imageIO->GetDimensions(0);
    info.height = imageIO->GetDimensions(1);
    info.depth = imageIO->GetDimensions(2);
    info.numTimes = 1;
  } else if (ndims == 4) {
    info.width = imageIO->GetDimensions(0);
    info.height = imageIO->GetDimensions(1);
    info.depth = imageIO->GetDimensions(2);
    info.numTimes = imageIO->GetDimensions(3);
  } else if (ndims == 5 && isNd2) {
    info.width = imageIO->GetDimensions(0);
    info.height = imageIO->GetDimensions(1);
    info.depth = imageIO->GetDimensions(2);
    info.numTimes = imageIO->GetDimensions(3);
    info.numChannels = imageIO->GetDimensions(4);
    if (imageIO->GetNumberOfComponents() > 1) {
      throw ZIOException(QString("Can not handle this nd2"));
    }
  } else {
    throw ZIOException(QString("NDims not supported: %1.").arg(ndims));
  }
  if (!isNd2) {
    info.numChannels = imageIO->GetNumberOfComponents();
  }
  switch (imageIO->GetComponentType()) {
    case itk::ImageIOBase::CHAR:
      info.bytesPerVoxel = 1;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case itk::ImageIOBase::UCHAR:
      info.bytesPerVoxel = 1;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case itk::ImageIOBase::SHORT:
      info.bytesPerVoxel = 2;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case itk::ImageIOBase::USHORT:
      info.bytesPerVoxel = 2;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case itk::ImageIOBase::INT:
      info.bytesPerVoxel = 4;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case itk::ImageIOBase::UINT:
      info.bytesPerVoxel = 4;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case itk::ImageIOBase::LONG:
      info.bytesPerVoxel = 8;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case itk::ImageIOBase::ULONG:
      info.bytesPerVoxel = 8;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case itk::ImageIOBase::FLOAT:
      info.bytesPerVoxel = 4;
      info.voxelFormat = VoxelFormat::Float;
      break;
    case itk::ImageIOBase::DOUBLE:
      info.bytesPerVoxel = 8;
      info.voxelFormat = VoxelFormat::Float;
      break;
    default:
      throw ZIOException("Not supported ElementType");
      break;
  }

  info.voxelSizeUnit = VoxelSizeUnit::mm;
  if (ndims == 1) {
    info.voxelSizeX = imageIO->GetSpacing(0);
  } else if (ndims == 2) {
    info.voxelSizeX = imageIO->GetSpacing(0);
    info.voxelSizeY = imageIO->GetSpacing(1);
  } else if (ndims > 3) {
    info.voxelSizeX = imageIO->GetSpacing(0);
    info.voxelSizeY = imageIO->GetSpacing(1);
    info.voxelSizeZ = imageIO->GetSpacing(2);
  }
  info.createDefaultDescriptions();
  if (ndims == 4) {
    for (size_t i = 0; i < info.timeStamps.size(); ++i)
      info.timeStamps[i] *= imageIO->GetSpacing(3);
  }

  if (info.isEmpty()) {
    throw ZIOException("Empty Image");
  }

  if (isNd2) {
    using DictionaryType = itk::MetaDataDictionary;
    const DictionaryType& dictionary = imageIO->GetMetaDataDictionary();

    using MetaDataStringType = itk::MetaDataObject<std::string>;

    auto itr = dictionary.Begin();
    auto end = dictionary.End();

    while (itr != end) {
      itk::MetaDataObjectBase::Pointer entry = itr->second;
      MetaDataStringType::Pointer entryvalue =
        dynamic_cast<MetaDataStringType*>( entry.GetPointer());
      if (entryvalue) {
        std::string tagkey = itr->first;
        for (size_t ch = 0; ch < info.numChannels; ++ch) {
          if (tagkey == QString("CH%1ChannelColor").arg(ch + 1).toStdString()) {
            QString tagvalue = QString::fromStdString(entryvalue->GetMetaDataObjectValue());
            bool ok;
            int color = tagvalue.toInt(&ok);
            if (!ok)
              throw ZIOException("Can not parse nd2 channel Color");
            col4 col;
            std::memcpy(&col, &color, 3);
            col.a = 255;
            info.channelColors[ch] = col;
            break;
          }
          if (tagkey == QString("CH%1ChannelDyeName").arg(ch + 1).toStdString()) {
            info.channelNames[ch] = QString::fromStdString(entryvalue->GetMetaDataObjectValue());
            break;
          }
        }
      }
      ++itr;
    }
  }
}

void ZImgITKImage::parseMetadata(const itk::ImageIOBase* imageIO, nim::ZImgMetadata& meta)
{
  using DictionaryType = itk::MetaDataDictionary;
  const DictionaryType& dictionary = imageIO->GetMetaDataDictionary();

  using MetaDataStringType = itk::MetaDataObject<std::string>;

  auto itr = dictionary.Begin();
  auto end = dictionary.End();

  while (itr != end) {
    itk::MetaDataObjectBase::Pointer entry = itr->second;
    MetaDataStringType::Pointer entryvalue =
      dynamic_cast<MetaDataStringType*>( entry.GetPointer());
    if (entryvalue) {
      std::string tagkey = itr->first;
      std::string tagvalue = entryvalue->GetMetaDataObjectValue();
      // std::cout << tagkey << " = " << tagvalue << std::endl;
      meta.attachToTopLevel(ZImgMetatag(QString::fromStdString(tagkey), QString::fromStdString(tagvalue)));
    }
    ++itr;
  }
}

bool ZImgITKImage::hasSCIFIOSupport() const
{
  return !ZGlobal::jarsDIR.isEmpty();
}

}  // namespace nim

