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
                            std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  try {
    itk::ImageIOBase::Pointer imageIO = itk::ImageIOFactory::CreateImageIO(QFile::encodeName(filename).constData(),
                                                                           itk::ImageIOFactory::IOFileModeEnum::ReadMode);

    if (imageIO.IsNull())
      throw ZIOException("can not create reader");

    imageIO->SetFileName(QFile::encodeName(filename).constData());
    imageIO->ReadImageInformation();

    bool isNrrd = QString(imageIO->GetNameOfClass()).contains("Nrrd");

    bool isNd2 = filename.endsWith(".nd2", Qt::CaseInsensitive);

    infos.resize(1);
    parseInfo(imageIO.GetPointer(), infos[0], isNd2);

    if (isNrrd) {
      createEmptySubBlocks(infos, subBlocks);
    } else {
      createDefaultSubBlocks(filename, infos, subBlocks);
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
                                                                           itk::ImageIOFactory::IOFileModeEnum::ReadMode);

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

void ZImgITKImage::readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene)
{
  if (scene != 0) {
    throw ZIOException("invalid scene");
  }

  try {
    itk::ImageIOBase::Pointer imageIO = itk::ImageIOFactory::CreateImageIO(QFile::encodeName(filename).constData(),
                                                                           itk::ImageIOFactory::IOFileModeEnum::ReadMode);

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
}

void ZImgITKImage::checkImgBeforeWriting(const QString& filename, const ZImgInfo& info,
                                         const ZImgWriteParameters& paras)
{
  ZImgFormat::checkImgBeforeWriting(filename, info, paras);
  if (!filename.endsWith(".nrrd", Qt::CaseInsensitive)) {
    throw ZIOException("only support nrrd format for now");
  }
  if (!(paras.compression == Compression::AUTO ||
        paras.compression == Compression::NONE ||
        paras.compression == Compression::DEFLATE)) {
    throw ZIOException(QString("compression %1 is not supported").arg(enumToString(paras.compression)));
  }
  if (info.numTimes != 1) {
    throw ZIOException("time sequence image is not supported");
  }
}

void ZImgITKImage::writeImg(const QString& filename, const ZImg& img, const ZImgWriteParameters& paras)
{
  checkImgBeforeWriting(filename, img.info(), paras);

  //
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
    case itk::IOComponentEnum::CHAR:
      info.bytesPerVoxel = 1;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case itk::IOComponentEnum::UCHAR:
      info.bytesPerVoxel = 1;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case itk::IOComponentEnum::SHORT:
      info.bytesPerVoxel = 2;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case itk::IOComponentEnum::USHORT:
      info.bytesPerVoxel = 2;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case itk::IOComponentEnum::INT:
      info.bytesPerVoxel = 4;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case itk::IOComponentEnum::UINT:
      info.bytesPerVoxel = 4;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case itk::IOComponentEnum::LONG:
      info.bytesPerVoxel = 8;
      info.voxelFormat = VoxelFormat::Signed;
      break;
    case itk::IOComponentEnum::ULONG:
      info.bytesPerVoxel = 8;
      info.voxelFormat = VoxelFormat::Unsigned;
      break;
    case itk::IOComponentEnum::FLOAT:
      info.bytesPerVoxel = 4;
      info.voxelFormat = VoxelFormat::Float;
      break;
    case itk::IOComponentEnum::DOUBLE:
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
            std::memcpy(static_cast<void*>(&col), &color, 3);
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

