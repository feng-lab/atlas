#include "zimgitkimage.h"

#include "zimgsliceprovider.h"
#include "zglobal.h"
#include <itkImageIOBase.h>
#include <itkNiftiImageIOFactory.h>
#include <itkNrrdImageIOFactory.h>
#include <itkImageIOFactory.h>
#include <itkSCIFIOImageIOFactory.h>
#include <itkMetaDataObject.h>
// #define ATLAS_SUPPORT_DICOM
#ifdef ATLAS_SUPPORT_DICOM
#include <itkGDCMImageIOFactory.h>
#endif

#include <QFile>
#include <QStringList>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>

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
    throw ZException(err.what(), ZException::Option::CheckErrno);
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
        // VLOG(1) << "ImageIO: " << io->GetNameOfClass();
        for (const auto& ext : io->GetSupportedReadExtensions()) {
          res.push_back(QString::fromStdString(ext));
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
    throw ZException(err.what(), ZException::Option::CheckErrno);
  }

  return res;
}

void ZImgITKImage::readInfo(const QString& filename,
                            std::vector<ZImgInfo>& infos,
                            std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks)
{
  try {
    itk::ImageIOBase::Pointer imageIO =
      itk::ImageIOFactory::CreateImageIO(QFile::encodeName(filename).constData(),
                                         itk::ImageIOFactory::IOFileModeEnum::ReadMode);

    if (imageIO.IsNull()) {
      throw ZException("can not create reader", ZException::Option::CheckErrno);
    }

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
    throw ZException(err.what(), ZException::Option::CheckErrno);
  }
}

void ZImgITKImage::readMetadata(const QString& filename, ZImgMetadata& meta, size_t /*scene*/)
{
  try {
    itk::ImageIOBase::Pointer imageIO =
      itk::ImageIOFactory::CreateImageIO(QFile::encodeName(filename).constData(),
                                         itk::ImageIOFactory::IOFileModeEnum::ReadMode);

    if (imageIO.IsNull()) {
      throw ZException("can not create reader", ZException::Option::CheckErrno);
    }

    imageIO->SetFileName(QFile::encodeName(filename).constData());
    imageIO->ReadImageInformation();

    parseMetadata(imageIO.GetPointer(), meta);
  }
  catch (itk::ExceptionObject& err) {
    throw ZException(err.what(), ZException::Option::CheckErrno);
  }
}

void ZImgITKImage::readThumbnail(const QString& /*filename*/,
                                 ZImgThumbernail& /*thumbnail*/,
                                 const ZImgRegion& /*region*/,
                                 size_t /*scene*/)
{
  try {
  }
  catch (itk::ExceptionObject& err) {
    throw ZException(err.what(), ZException::Option::CheckErrno);
  }
}

void ZImgITKImage::readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene)
{
  if (scene != 0) {
    throw ZException("invalid scene");
  }

  try {
    itk::ImageIOBase::Pointer imageIO =
      itk::ImageIOFactory::CreateImageIO(QFile::encodeName(filename).constData(),
                                         itk::ImageIOFactory::IOFileModeEnum::ReadMode);

    if (imageIO.IsNull()) {
      throw ZException("can not create reader", ZException::Option::CheckErrno);
    }

    imageIO->SetFileName(QFile::encodeName(filename).constData());
    imageIO->ReadImageInformation();

    bool isNrrd = QString(imageIO->GetNameOfClass()).contains("Nrrd");

    bool isNd2 = filename.endsWith(".nd2", Qt::CaseInsensitive);

    ZImgInfo imgInfo;
    parseInfo(imageIO.GetPointer(), imgInfo, isNd2);

    if (region.isEmpty() || !region.isValid(imgInfo)) {
      throw ZException(fmt::format("Invalid image region. Image info: '{}', region: '{}'", imgInfo, region));
    }

    if (isNd2) {
      ZImgRegion rgn = region;
      rgn.resolveRegionEnd(imgInfo);

      ZImgInfo clipInfo = rgn.clip(imgInfo);
      img = ZImg(clipInfo);

      itk::ImageIORegion ioRegion(5);
      ioRegion.SetIndex(0, rgn.start.x);
      ioRegion.SetIndex(1, rgn.start.y);
      ioRegion.SetIndex(2, rgn.start.z);
      ioRegion.SetIndex(3, rgn.start.t);
      ioRegion.SetIndex(4, rgn.start.c);
      ioRegion.SetSize(0, rgn.end.x - rgn.start.x);
      ioRegion.SetSize(1, rgn.end.y - rgn.start.y);
      ioRegion.SetSize(2, rgn.end.z - rgn.start.z);
      ioRegion.SetSize(3, rgn.end.t - rgn.start.t);
      ioRegion.SetSize(4, rgn.end.c - rgn.start.c);

      if (imageIO->GetNumberOfComponents() <= 1) {
        imageIO->SetIORegion(ioRegion);
        if (clipInfo.numTimes > 1) {
          auto buf = std::make_unique_for_overwrite<uint8_t[]>(img.byteNumber());
          imageIO->Read(buf.get());
          fixDimensionOrder(buf.get(), "XYZTC", img);
        } else {
          imageIO->Read(img.channelData(0));
        }
      } else {
        ZImgInfo scClipInfo = clipInfo;
        scClipInfo.numChannels = imageIO->GetNumberOfComponents();
        scClipInfo.createDefaultDescriptions();
        ZImg tmpScImg(scClipInfo);
        auto buf = std::make_unique_for_overwrite<uint8_t[]>(tmpScImg.byteNumber());
        // VLOG(2) << tmpScImg.byteNumber();
        for (auto ch = rgn.start.c; ch < rgn.end.c; ++ch) {
          ioRegion.SetIndex(4, ch);
          ioRegion.SetSize(4, 1);
          imageIO->SetIORegion(ioRegion);
          imageIO->Read(buf.get());
          fixDimensionOrder(buf.get(), "CXYZT", tmpScImg);
          size_t bestChannel = 0;
          double bestChannelSum = tmpScImg.createView(0, -1).sum();
          for (size_t c = 1; c < tmpScImg.numChannels(); ++c) {
            auto channelSum = tmpScImg.createView(c, -1).sum();
            if (channelSum > bestChannelSum) {
              bestChannel = c;
              bestChannelSum = channelSum;
            }
          }
          img.pasteImg(tmpScImg.createView(bestChannel, -1), ZVoxelCoordinate(0, 0, 0, ch, 0));
        }
      }
    } else if (region.containsWholeImg(imgInfo) || isNrrd) {
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
        auto buf = std::make_unique_for_overwrite<uint8_t[]>(img.byteNumber());
        imageIO->Read(buf.get());
        fixDimensionOrder(buf.get(), "CXYZT", img);
      } else {
        imageIO->Read(img.channelData(0));
        if (imgInfo.numChannels > 1) {
          ZImg tpImg(imgInfo);
          CXYZtoXYZC(img, tpImg);
          img.swap(tpImg);
        }
      }

      if (isNrrd && !region.containsWholeImg(imgInfo)) {
        img = img.crop(region);
      }
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
        auto buf = std::make_unique_for_overwrite<uint8_t[]>(tmpImg.byteNumber());
        imageIO->Read(buf.get());
        fixDimensionOrder(buf.get(), "CXYZT", tmpImg);
      } else {
        imageIO->Read(tmpImg.channelData(0));
        if (clipInfo.numChannels > 1) {
          ZImg tpImg(clipInfo);
          CXYZtoXYZC(tmpImg, tpImg);
          tmpImg.swap(tpImg);
        }
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
    throw ZException(err.what(), ZException::Option::CheckErrno);
  }
}

void ZImgITKImage::checkImgBeforeWriting(const QString& filename,
                                         const ZImgInfo& info,
                                         const ZImgWriteParameters& paras)
{
  ZImgFormat::checkImgBeforeWriting(filename, info, paras);
  if (!filename.endsWith(".nrrd", Qt::CaseInsensitive)) {
    throw ZException("only support nrrd format for now");
  }
  if (!(paras.compression == Compression::AUTO || paras.compression == Compression::NONE ||
        paras.compression == Compression::DEFLATE)) {
    throw ZException(fmt::format("compression {} is not supported", paras.compression));
  }
  if (info.numTimes != 1) {
    throw ZException("time sequence image is not supported");
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
  auto ndims = imageIO->GetNumberOfDimensions();
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
    if (isNd2) {
      info.numChannels = 1;
    }
  } else if (ndims == 4) {
    info.width = imageIO->GetDimensions(0);
    info.height = imageIO->GetDimensions(1);
    info.depth = imageIO->GetDimensions(2);
    info.numTimes = imageIO->GetDimensions(3);
    if (isNd2) {
      info.numChannels = 1;
    }
  } else if (ndims == 5 && isNd2) {
    info.width = imageIO->GetDimensions(0);
    info.height = imageIO->GetDimensions(1);
    info.depth = imageIO->GetDimensions(2);
    info.numTimes = imageIO->GetDimensions(3);
    info.numChannels = imageIO->GetDimensions(4);
  } else {
    throw ZException(fmt::format("NDims not supported: {}", ndims));
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
      throw ZException("Not supported ElementType");
  }

  info.voxelSizeUnit = VoxelSizeUnit::mm;
  if (ndims == 1) {
    info.voxelSizeX = imageIO->GetSpacing(0);
  } else if (ndims == 2) {
    info.voxelSizeX = imageIO->GetSpacing(0);
    info.voxelSizeY = imageIO->GetSpacing(1);
  } else if (ndims >= 3) {
    info.voxelSizeX = imageIO->GetSpacing(0);
    info.voxelSizeY = imageIO->GetSpacing(1);
    info.voxelSizeZ = imageIO->GetSpacing(2);
  }
  info.createDefaultDescriptions();
  if (ndims == 4) {
    for (auto& timeStamp : info.timeStamps) {
      timeStamp *= imageIO->GetSpacing(3);
    }
  }

  if (info.isEmpty()) {
    throw ZException("Empty Image");
  }

  if (isNd2) {
    using DictionaryType = itk::MetaDataDictionary;
    const DictionaryType& dictionary = imageIO->GetMetaDataDictionary();

    using MetaDataStringType = itk::MetaDataObject<std::string>;

    std::vector<size_t> usedChannels;
    std::string key = "sSpecSettings";
    if (dictionary.HasKey(key)) {
      if (auto value = dynamic_cast<const MetaDataStringType*>(dictionary.Get(key)); value) {
        QString valueStr = QString::fromStdString(value->GetMetaDataObjectValue());
        // VLOG(2) << valueStr;

        static QRegularExpression channelInfo(R"(^CH(\d+)\s+{Laser Wavelength}:.*)");

        QTextStream in(&valueStr, QIODevice::ReadOnly);

        QString line;
        bool ok1;
        while (in.readLineInto(&line)) {
          auto match = channelInfo.match(line);
          if (match.hasMatch()) {
            usedChannels.push_back(match.captured(1).toUInt(&ok1));
            VLOG(2) << line << " " << usedChannels.back();
            CHECK(ok1) << line << " " << ok1;
            continue;
          }
        }
      }
    }
    if (usedChannels.size() < info.numChannels) {
      usedChannels.clear();
    }

    for (size_t ch = 0; ch < info.numChannels; ++ch) {
      auto actualChannel = usedChannels.empty() ? ch + 1 : usedChannels[ch];
      key = fmt::format("CH{}ChannelColor", actualChannel);
      if (dictionary.HasKey(key)) {
        if (auto value = dynamic_cast<const MetaDataStringType*>(dictionary.Get(key)); value) {
          QString tagvalue = QString::fromStdString(value->GetMetaDataObjectValue());
          bool ok;
          int32_t color = tagvalue.toInt(&ok);
          if (!ok) {
            throw ZException("Can not parse nd2 channel Color");
          }
          col4 col;
          std::memcpy(&col, &color, 3);
          CHECK(col.a == 255);
          info.channelColors[ch] = col;
        }
      }
      key = fmt::format("CH{}ChannelDyeName", actualChannel);
      if (dictionary.HasKey(key)) {
        if (auto value = dynamic_cast<const MetaDataStringType*>(dictionary.Get(key)); value) {
          info.channelNames[ch] = value->GetMetaDataObjectValue();
        }
      }
    }

    VLOG(2) << info.toString();
  }
}

void ZImgITKImage::parseMetadata(const itk::ImageIOBase* imageIO, ZImgMetadata& meta)
{
  using DictionaryType = itk::MetaDataDictionary;
  const DictionaryType& dictionary = imageIO->GetMetaDataDictionary();

  using MetaDataStringType = itk::MetaDataObject<std::string>;

  auto itr = dictionary.Begin();
  auto end = dictionary.End();

  while (itr != end) {
    itk::MetaDataObjectBase::Pointer entry = itr->second;
    MetaDataStringType::Pointer entryvalue = dynamic_cast<MetaDataStringType*>(entry.GetPointer());
    if (entryvalue) {
      std::string tagkey = itr->first;
      std::string tagvalue = entryvalue->GetMetaDataObjectValue();
      // std::cout << tagkey << " = " << tagvalue << std::endl;
      meta.attachToTopLevel(ZImgMetatag(tagkey, tagvalue));
    }
    ++itr;
  }
}

bool ZImgITKImage::hasSCIFIOSupport()
{
  return !ZImgGlobal::instance().jarsDIR.isEmpty();
}

} // namespace nim
