#include "zimgleica.h"

#include "zlog.h"
#include "zioutils.h"
#include "zimgio.h"
#include <QUuid>
#include <QXmlStreamReader>
#include <QFile>
#include <QTextStream>
#include <QUrl>

namespace {

#pragma pack(push, 1)

// Info for the next block to read out of the file
struct NextBlock
{
  int32_t test; // test == 0x70
  int32_t length; // Number of bytes to read
};

// XML Description or Type Content
struct XMLOrTypeContent
{
  unsigned char test; // test == 0x2A
  int32_t textLength; // Number of unicode characters
  //QChar text[]; // XML Object Description or Type Name: "LMS_Object_File"
};

struct Int32Block
{
  unsigned char test; // test == 0x2A
  uint32_t number; // Number of unicode characters
};

struct UInt64Block
{
  unsigned char test; // test == 0x2A
  uint64_t Number; // Number of unicode characters
};

// Memory Description for File Version 1 (32 Bit)
struct MemoryBlock32
{
  unsigned char test1; // test == 0x2A
  uint32_t memorySize; // Memory size of the object
  unsigned char test2; // test == 0x2A
  int32_t textLength; // Number of Unicode characters
  //QChar text[]; // Short image description (Name)
};

// Memory Description for File Version 2 (64 Bit)
struct MemoryBlock64
{
  unsigned char test1; // test == 0x2A
  uint64_t memorySize;// Memory size of the object
  unsigned char test2; // test == 0x2A
  int32_t textLength; // Number of Unicode characters
  //QChar text[]; // Short image description (Name)
};

#pragma pack(pop)

}

namespace nim {

static_assert(sizeof(QUuid) == 16 && std::is_trivially_copyable<QUuid>::value, "wrong uuid type");

bool ZImgLeica::supportRead() const
{
  return true;
}

bool ZImgLeica::supportWrite() const
{
  return false;
}

QString ZImgLeica::shortName() const
{
  return "Leica Image";
}

QString ZImgLeica::fullName() const
{
  return "Leica Image";
}

QStringList ZImgLeica::extensions() const
{
  QStringList res;
  res << "lif" << "lof" << "xlef" << "xllf";
  return res;
}

void ZImgLeica::readInfo(const QString& filename, std::vector<ZImgInfo>& infos,
                         std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>>* subBlocks,
                         std::vector<std::set<size_t>>* pyramidalRatios)
{
  clearInternalState();

  QString xml;
  std::vector<std::tuple<size_t, QString, size_t>> memoryOffsetNameLength;
  readXml(filename, xml, memoryOffsetNameLength);

  std::vector<ImageInfo> leicaImageInfos;
  readLeicaInfo(xml, QFileInfo(filename).absoluteDir(), leicaImageInfos);

  infos.clear();
  detectInfos(infos, leicaImageInfos);

  createDefaultSubBlocks(filename, infos, subBlocks, pyramidalRatios);
}

void ZImgLeica::readMetadata(const QString& filename, ZImgMetadata& meta, size_t /*scene*/)
{
  clearInternalState();

  QString xml;
  std::vector<std::tuple<size_t, QString, size_t>> memoryOffsetNameLength;
  readXml(filename, xml, memoryOffsetNameLength);

  ZImgMetatag tag("metadata", xml);
  meta.attachToTopLevel(tag);
}

void  ZImgLeica::readThumbnail(const QString& /*filename*/, ZImgThumbernail& /*thumbnail*/,
                               const ZImgRegion& /*region*/, size_t /*scene*/)
{
}

void ZImgLeica::readImg(const QString& filename, ZImg& img, const ZImgRegion& region, size_t scene, size_t ratio)
{
  clearInternalState();

  QString xml;
  std::vector<std::tuple<size_t, QString, size_t>> allMemoryOffsetNameLength;
  readXml(filename, xml, allMemoryOffsetNameLength);

  std::vector<ImageInfo> leicaImageInfos;
  readLeicaInfo(xml, QFileInfo(filename).absoluteDir(), leicaImageInfos);

  std::vector<ZImgInfo> infos;
  detectInfos(infos, leicaImageInfos);

  if (scene >= infos.size()) {
    throw ZIOException("invalid scene");
  }
  ZImgInfo& info = infos[scene];

  if (region.isEmpty() || !region.isValid(info)) {
    throw ZIOException(
      QString("Invalid image region. Image info: '%1', region: '%2'").arg(info.toQString()).arg(region.toQString()));
  }

  ZImgRegion rgn = region;
  rgn.resolveRegionEnd(info);

  CHECK(ratio >= 1);

  const auto& ii = leicaImageInfos[scene];
  if (!allMemoryOffsetNameLength.empty()) { // lof or lif
    auto monl = allMemoryOffsetNameLength[0];
    if (allMemoryOffsetNameLength.size() > 1) {
      bool found = false;
      for (const auto& tmp : allMemoryOffsetNameLength) {
        //LOG(INFO) << std::get<1>(tmp);
        //LOG(INFO) << ii.imageMemory.memoryBlockID;
        if (std::get<1>(tmp) == ii.imageMemory.memoryBlockID) {
          found = true;
          monl = tmp;
          break;
        }
      }
      if (!found) {
        throw ZIOException("can not find valid memory, please send this file to flq@live.com");
      }
    }
    if (std::get<2>(monl) == 0) {
      throw ZIOException("find invalid memory, please send this file to flq@live.com");
    }
    std::vector<size_t> dimensionStrides(5, 0);  // XYZCT
    std::vector<uint64_t> channelOffsets;
    for (const auto& cd : ii.channels) {
      channelOffsets.push_back(cd.bytesInc);
    }
    if (channelOffsets.size() > 1) {
      std::sort(channelOffsets.begin(), channelOffsets.end());
      dimensionStrides[3] = channelOffsets[1] - channelOffsets[0];
    }
    for (const auto& dd : ii.dimensions) {
      switch (dd.dimID) {
        case 1:
          dimensionStrides[0] = dd.bytesInc;
          break;
        case 2:
          dimensionStrides[1] = dd.bytesInc;
          break;
        case 3:
          dimensionStrides[2] = dd.bytesInc;
          break;
        case 4:
          dimensionStrides[4] = dd.bytesInc;
          break;
        default:
          throw ZIOException("only support XYZT dimension");
      }
    }
    if (dimensionStrides[0] == 0) {
      dimensionStrides[0] = info.voxelByteNumber();
    }
    if (dimensionStrides[1] == 0) {
      dimensionStrides[1] = dimensionStrides[0] * info.size(0);
    }
    if (dimensionStrides[3] == 0) {
      dimensionStrides[3] = dimensionStrides[1] * info.size(1);
    }
    if (dimensionStrides[2] == 0) {
      dimensionStrides[2] = std::max(dimensionStrides[3] * info.size(3), dimensionStrides[1] * info.size(1));
    }
    if (dimensionStrides[4] == 0) {
      dimensionStrides[4] = std::max(dimensionStrides[3] * info.size(3), dimensionStrides[2] * info.size(2));
    }


    img = readRawImg(filename, info, dimensionStrides, std::get<0>(monl), rgn);
  } else {
    ZImgIO imgIO;
    ZImgInfo resInfo = rgn.clip(info);
    if (ii.imageMemory.fileNames.size() == 1) {
      std::vector<ZImgInfo> fileInfos;
      imgIO.readInfo(ii.imageMemory.fileNames[0], fileInfos);
      if (!fileInfos.empty() && fileInfos[0].isSameType(info) && fileInfos[0].isSameSize(info)) {
        imgIO.readImg(ii.imageMemory.fileNames[0], img, rgn);
        img.infoRef() = resInfo;
      } else {
        throw ZIOException("image and metadata do not match, please send this file to flq@live.com");
      }
    } else if (size_t(ii.imageMemory.fileNames.size()) == info.numTimes) {
      std::vector<ZImgInfo> fileInfos;
      imgIO.readInfo(ii.imageMemory.fileNames, Dimension::T, fileInfos);
      if (!fileInfos.empty() && fileInfos[0].isSameType(info) && fileInfos[0].isSameSize(info)) {
        imgIO.readImg(ii.imageMemory.fileNames, Dimension::T, rgn, img);
        img.infoRef() = resInfo;
      } else {
        throw ZIOException("image and metadata do not match, please send this file to flq@live.com");
      }
    } else if (size_t(ii.imageMemory.fileNames.size()) == info.depth) {
      std::vector<ZImgInfo> fileInfos;
      imgIO.readInfo(ii.imageMemory.fileNames, Dimension::Z, fileInfos);
      if (!fileInfos.empty() && fileInfos[0].isSameType(info) && fileInfos[0].isSameSize(info)) {
        imgIO.readImg(ii.imageMemory.fileNames, Dimension::Z, rgn, img);
        img.infoRef() = resInfo;
      } else {
        throw ZIOException("image and metadata do not match, please send this file to flq@live.com");
      }
    } else {
      throw ZIOException("Unhandled leica image sequence, please send this file to flq@live.com");
    }
  }

  ZImgMetatag tag("metadata", xml);
  img.metadataRef().attachToTopLevel(tag);

  if (ratio > 1) {
    img.zoom(1.0 / ratio, 1.0 / ratio);
  }
}

void ZImgLeica::clearInternalState()
{
}

int ZImgLeica::parseLIFVersion(const QString& xmlString) const
{
  int res = 0;
  QXmlStreamReader xml(xmlString);

  while (!xml.atEnd() && !xml.hasError()) {
    /* Read next element.*/
    QXmlStreamReader::TokenType token = xml.readNext();
    // If token is just StartDocument, we'll go to next.
    if (token == QXmlStreamReader::StartDocument) {
      continue;
    }
    // If token is StartElement, we'll see if we can read it.
    if (token == QXmlStreamReader::StartElement) {
      if (xml.name() != "LMSDataContainerHeader") {
        continue;
      }

      QXmlStreamAttributes attributes = xml.attributes();
      bool ok = false;
      if (attributes.hasAttribute("Version")) {
        res = attributes.value("Version").toInt(&ok);
        if (!ok) {
          throw ZIOException("Can not parse Leica file version");
        }
      }
      return res;
    }
  }
  return res;
}

void ZImgLeica::readXml(const QString& filename, QString& xml,
                        std::vector<std::tuple<size_t, QString, size_t>>& memoryOffsetNameLength) const
{
  if (filename.endsWith(".xlef", Qt::CaseInsensitive) || filename.endsWith(".xllf", Qt::CaseInsensitive)) {
    QFile f(filename);
    if (!f.open(QFile::ReadOnly | QFile::Text)) {
      throw ZIOException("can not open file");
    }
    QTextStream in(&f);
    xml = in.readAll();
  } else {
    std::ifstream inputFileStream;
    openFileStream(inputFileStream, filename, std::ios_base::in | std::ios_base::binary);

    NextBlock nb;
    readStream(inputFileStream, &nb, sizeof(NextBlock));
    if (nb.test != 0x70) {
      throw ZIOException("incorrect leica file header");
    }

    XMLOrTypeContent xtc;
    readStream(inputFileStream, &xtc, sizeof(XMLOrTypeContent));
    if (xtc.test != 0x2A || nb.length != 5 + xtc.textLength * 2) {
      throw ZIOException("incorrect lecia xml or type content");
    }
    std::vector<QChar> charBuf(xtc.textLength);
    readStream(inputFileStream, charBuf.data(), xtc.textLength * 2);
    xml = QString(charBuf.data(), charBuf.size());
    bool isLOF = xml == "LMS_Object_File";

    int majorVersion = 0;
    int minorVersion = 0;
    if (isLOF) {
      Int32Block majorVersionBlock;
      readStream(inputFileStream, &majorVersionBlock, sizeof(majorVersionBlock));
      if (majorVersionBlock.test == 0x2A) {
        majorVersion = majorVersionBlock.number;
      } else {
        throw ZIOException("incorrect lecia LOF major version");
      }
      Int32Block minorVersionBlock;
      readStream(inputFileStream, &minorVersionBlock, sizeof(minorVersionBlock));
      if (minorVersionBlock.test == 0x2A) {
        minorVersion = minorVersionBlock.number;
      } else {
        throw ZIOException("incorrect lecia LOF minor version");
      }
      UInt64Block memorySizeBlock;
      readStream(inputFileStream, &memorySizeBlock, sizeof(memorySizeBlock));
      if (memorySizeBlock.test != 0x2A) {
        throw ZIOException("incorrect lecia LOF memory size");
      }
      memoryOffsetNameLength.push_back(std::make_tuple(size_t(inputFileStream.tellg()),
                                                       QString(""), size_t(memorySizeBlock.Number)));
      inputFileStream.seekg(memorySizeBlock.Number, std::ios_base::cur);

      readStream(inputFileStream, &nb, sizeof(NextBlock));
      if (nb.test != 0x70) {
        throw ZIOException("incorrect leica LOF xml header");
      }

      readStream(inputFileStream, &xtc, sizeof(XMLOrTypeContent));
      if (xtc.test != 0x2A || nb.length != 5 + xtc.textLength * 2) {
        throw ZIOException("incorrect lecia LOF xml content");
      }
      charBuf.resize(xtc.textLength);
      readStream(inputFileStream, charBuf.data(), xtc.textLength * 2);
      xml = QString(charBuf.data(), charBuf.size());
    } else { // LIF
      int lifVersion = parseLIFVersion(xml);
      do {
        if (!inputFileStream.read(reinterpret_cast<char*>(&nb), sizeof(NextBlock))) {
          break;
        }

        if (lifVersion == 1) {
          MemoryBlock32 md;
          readStream(inputFileStream, &md, sizeof(MemoryBlock32));
          if (md.test1 != 0x2A || md.test2 != 0x2A || nb.length != 10 + md.textLength * 2) {
            throw ZIOException("incorrect lecia LOF xml content");
          }
          charBuf.resize(md.textLength);
          readStream(inputFileStream, charBuf.data(), md.textLength * 2);
          QString imageDescription(charBuf.data(), charBuf.size());
          memoryOffsetNameLength.push_back(std::make_tuple(size_t(inputFileStream.tellg()),
                                                           imageDescription, size_t(md.memorySize)));

          inputFileStream.seekg(md.memorySize, std::ios_base::cur);
        } else if (lifVersion == 2) {
          MemoryBlock64 md;
          readStream(inputFileStream, &md, sizeof(MemoryBlock64));
          if (md.test1 != 0x2A || md.test2 != 0x2A || nb.length != 14 + md.textLength * 2) {
            throw ZIOException("incorrect lecia LOF xml content");
          }
          charBuf.resize(md.textLength);
          readStream(inputFileStream, charBuf.data(), md.textLength * 2);
          QString imageDescription(charBuf.data(), charBuf.size());
          memoryOffsetNameLength.push_back(std::make_tuple(size_t(inputFileStream.tellg()),
                                                           imageDescription, size_t(md.memorySize)));

          inputFileStream.seekg(md.memorySize, std::ios_base::cur);
        } else {
          throw ZIOException(QString("not supported leica lif version: %1").arg(lifVersion));
        }
      } while (true);
    }
  }

  std::cout << xml.toLocal8Bit();
}

void ZImgLeica::readLeicaInfo(const QString& xmlString, const QDir& xmlDir, std::vector<ImageInfo>& imageInfos)
{
  // QXmlStreamReader takes any QIODevice.
  QXmlStreamReader xml(xmlString);

  // We'll parse the XML until we reach end of it.
  while (!xml.atEnd() && !xml.hasError()) {
    /* Read next element.*/
    QXmlStreamReader::TokenType token = xml.readNext();
    // If token is just StartDocument, we'll go to next.
    if (token == QXmlStreamReader::StartDocument) {
      continue;
    }
    // If token is StartElement, we'll see if we can read it.
    if (token == QXmlStreamReader::StartElement) {
      if (xml.name() != "LMSDataContainerHeader") {
        continue;
      }

      parseMetadata(xml, xmlDir, imageInfos);
      break;
    }
  }
  // Error handling.
  if (xml.hasError()) {
    throw ZIOException(QString("error parsing leica metadata xml: %1").arg(xml.errorString()));
  }
  xml.clear();
}

void ZImgLeica::parseMetadata(QXmlStreamReader& xml, const QDir& xmlDir, std::vector<ImageInfo>& imageInfos)
{
  CHECK(xml.isStartElement() && xml.name() == "LMSDataContainerHeader");

  while (xml.readNextStartElement()) {
    if (xml.name() == "Element") {
      parseElement(xml, xmlDir, imageInfos);
    } else {
      xml.skipCurrentElement();
    }
  }
}

void ZImgLeica::parseElement(QXmlStreamReader& xml, const QDir& xmlDir, std::vector<ImageInfo>& imageInfos)
{
  QString name;
  bool visibilityInvalid = false;
  bool copyOptionInvalid = false;

  bool ok;
  QXmlStreamAttributes attributes = xml.attributes();
  if (attributes.hasAttribute("Name")) {
    name = attributes.value("Name").toString();
  }
  if (attributes.hasAttribute("Visibility")) {
    int v = attributes.value("Visibility").toInt(&ok);
    if (!ok)
      throw ZIOException("Can not parse leica element Visibility");
    visibilityInvalid = v != 1;
  }
  if (attributes.hasAttribute("CopyOption")) {
    int v = attributes.value("CopyOption").toInt(&ok);
    if (!ok)
      throw ZIOException("Can not parse leica element CopyOption");
    copyOptionInvalid = v != 1;
  }
  if (visibilityInvalid || copyOptionInvalid) {
    xml.skipCurrentElement();
    return;
  }

  ImageInfo imageInfo;

  while (xml.readNextStartElement()) {

    if (xml.name() == "Data") {
      while (xml.readNextStartElement()) {
        if (xml.name() == "Experiment") {
          xml.skipCurrentElement();
        } else if (xml.name() == "Image") {
          while (xml.readNextStartElement()) {
            if (xml.name() == "ImageDescription") {
              while (xml.readNextStartElement()) {
                if (xml.name() == "Channels") {
                  while (xml.readNextStartElement()) {
                    if (xml.name() == "ChannelDescription") {
                      ChannelDescription cd;
                      attributes = xml.attributes();
                      cd.dataType = attributes.value("DataType").toInt(&ok);
                      if (!ok)
                        throw ZIOException("Can not parse leica ChannelDescription DataType");
                      cd.channelTag = attributes.value("ChannelTag").toInt(&ok);
                      if (!ok)
                        throw ZIOException("Can not parse leica ChannelDescription ChannelTag");
                      cd.resolution = attributes.value("Resolution").toUInt(&ok);
                      if (!ok)
                        throw ZIOException("Can not parse leica ChannelDescription Resolution");
                      cd.nameOfMeasuredQuantity = attributes.value("NameOfMeasuredQuantity").toString();
                      cd.min = attributes.value("Min").toDouble(&ok);
                      if (!ok)
                        throw ZIOException("Can not parse leica ChannelDescription Min");
                      cd.max = attributes.value("Max").toDouble(&ok);
                      if (!ok)
                        throw ZIOException("Can not parse leica ChannelDescription Max");
                      cd.unit = attributes.value("Unit").toString();
                      cd.LUTName = attributes.value("LUTName").toString();
                      cd.isLUTInverted = attributes.value("IsLUTInverted").toInt(&ok);
                      if (!ok)
                        throw ZIOException("Can not parse leica ChannelDescription isLUTInverted");
                      cd.bytesInc = attributes.value("BytesInc").toULongLong(&ok);
                      if (!ok)
                        throw ZIOException("Can not parse leica ChannelDescription BytesInc");
                      cd.bitInc = attributes.value("BitInc").toUInt(&ok);
                      if (!ok)
                        throw ZIOException("Can not parse leica ChannelDescription BitInc");
                      imageInfo.channels.push_back(cd);
                    }
                    xml.skipCurrentElement();
                  }
                } else if (xml.name() == "Dimensions") {
                  while (xml.readNextStartElement()) {
                    if (xml.name() == "DimensionDescription") {
                      DimensionDescription dd;
                      attributes = xml.attributes();
                      dd.dimID = attributes.value("DimID").toInt(&ok);
                      if (!ok)
                        throw ZIOException("Can not parse leica DimensionDescription DimID");
                      dd.numberOfElements = attributes.value("NumberOfElements").toUInt(&ok);
                      if (!ok)
                        throw ZIOException("Can not parse leica DimensionDescription NumberOfElements");
                      dd.origin = attributes.value("Origin").toDouble(&ok);
                      if (!ok)
                        throw ZIOException("Can not parse leica DimensionDescription Origin");
                      dd.length = attributes.value("Length").toDouble(&ok);
                      if (!ok)
                        throw ZIOException("Can not parse leica DimensionDescription Length");
                      dd.unit = attributes.value("Unit").toString();
                      dd.bytesInc = attributes.value("BytesInc").toULongLong(&ok);
                      if (!ok)
                        throw ZIOException("Can not parse leica DimensionDescription BytesInc");
                      dd.bitInc = attributes.value("BitInc").toUInt(&ok);
                      if (!ok)
                        throw ZIOException("Can not parse leica DimensionDescription BitInc");
                      imageInfo.dimensions.push_back(dd);
                    }
                    xml.skipCurrentElement();
                  }
                } else {
                  xml.skipCurrentElement();
                }
              }
            } else if (xml.name() == "TimeStampList") {
              std::vector<uint64_t> values;
              attributes = xml.attributes();
              if (attributes.hasAttribute("NumberOfTimeStamps")) {
                uint64_t nts = attributes.value("NumberOfTimeStamps").toULongLong(&ok);
                if (!ok)
                  throw ZIOException("Can not parse leica TimeStampList NumberOfTimeStamps");
                QString tsText = xml.readElementText();
                QStringList nums = tsText.split(" ", QString::SkipEmptyParts);
                if (uint64_t(nums.size()) != nts)
                  throw ZIOException("TimeStampList NumberOfTimeStamps does not match actual number");
                for (const auto& num : nums) {
                  values.push_back(num.toULongLong(&ok, 16));
                  if (!ok)
                    throw ZIOException(QString("Can not parse leica TimeStamp string: %1").arg(num));
                }
              } else {
                while (xml.readNextStartElement()) {
                  if (xml.name() == "TimeStamp") {
                    attributes = xml.attributes();
                    uint64_t highInteger = attributes.value("HighInteger").toULongLong(&ok);
                    if (!ok)
                      throw ZIOException("Can not parse leica TimeStamp HighInteger");
                    uint64_t lowInteger = attributes.value("LowInteger").toULongLong(&ok);
                    if (!ok)
                      throw ZIOException("Can not parse leica TimeStamp LowInteger");
                    values.push_back((highInteger << 32) + lowInteger);
                  } else {
                    throw ZIOException(QString("invalid child of leica TimeStampList: %1").arg(xml.name().toString()));
                  }
                  xml.skipCurrentElement();
                }
              }

              for (size_t i = 1; i < values.size(); ++i) {
                values[i] -= values[0];
              }
              values[0] = 0;
              for (auto val : values) {
                imageInfo.timeStamps.push_back(val * 1e-7);
              }
            } else {
              xml.skipCurrentElement();
            }
          }
        } else if (xml.name() == "Collection") {
          xml.skipCurrentElement(); //todo: what is ChildTypeTest?
        } else {
          xml.skipCurrentElement();
        }
      }
    } else if (xml.name() == "Memory") {
      attributes = xml.attributes();
      imageInfo.imageMemory.size = attributes.value("Size").toULongLong(&ok);
      if (!ok)
        throw ZIOException("Can not parse leica Memory Size");
      imageInfo.imageMemory.memoryBlockID = attributes.value("MemoryBlockID").toString();
      while (xml.readNextStartElement()) {
        if (xml.name() == "Block" || xml.name() == "Frame") {
          attributes = xml.attributes();
          QString fp = QUrl::fromPercentEncoding(attributes.value("File").toString().toUtf8());
          fp.replace(QChar('\\'), QChar('/'));
          fp = xmlDir.filePath(fp);
          uint64_t foffset = attributes.value("Offset").toULongLong(&ok);
          if (!ok)
            throw ZIOException("Can not parse leica Memory Block/Frame Offset");
          uint64_t fsize = attributes.value("Size").toULongLong(&ok);
          if (!ok)
            throw ZIOException("Can not parse leica Memory Block/Frame Size");
          imageInfo.imageMemory.fileNames.push_back(fp);
          imageInfo.imageMemory.fileOffsets.push_back(foffset);
          imageInfo.imageMemory.fileSizes.push_back(fsize);
        }
        xml.skipCurrentElement();
      }
      //LOG(INFO) << imageInfo.imageMemory.memoryBlockID;
    } else if (xml.name() == "Children") {
      while (xml.readNextStartElement()) {
        if (xml.name() == "Element") {
          parseElement(xml, xmlDir, imageInfos);
        } else if (xml.name() == "Reference") {
          attributes = xml.attributes();
          if (attributes.hasAttribute("File")) {
            QString fp = QUrl::fromPercentEncoding(attributes.value("File").toString().toUtf8());
            fp.replace(QChar('\\'), QChar('/'));
            fp = xmlDir.filePath(fp);
            if (fp.endsWith(".xlif", Qt::CaseInsensitive)) {
              parseXLIF(fp, imageInfos);
            } else if (fp.endsWith(".xlcf", Qt::CaseInsensitive)) {
              parseXLCF(fp, imageInfos);
            }
          }
          xml.skipCurrentElement();
        } else {
          xml.skipCurrentElement();
        }
      }
    } else {
      xml.skipCurrentElement();
    }
  }

  bool willRead = !imageInfo.channels.empty() && !imageInfo.dimensions.empty();
//    LOG(INFO) << imageInfo.channels.size();
//    LOG(INFO) << imageInfo.dimensions.size();
  if (willRead) {
    for (const auto& channel : imageInfo.channels) {
      if (channel.bitInc != 0) {
        willRead = false;
        LOG(WARNING) << "Leica image with nonzero channel BitInc is not supported";
        break;
      }
    }
  }
  if (willRead) {
    for (const auto& dimension : imageInfo.dimensions) {
      if (dimension.bitInc != 0) {
        willRead = false;
        LOG(WARNING) << "Leica image with nonzero dimension BitInc is not supported";
        break;
      }
    }
  }
  if (willRead) {
    imageInfos.push_back(imageInfo);
  }
}

void ZImgLeica::parseXLIF(const QString& filename, std::vector<ImageInfo>& imageInfos)
{
  QFile f(filename);
  if (!f.open(QFile::ReadOnly | QFile::Text)) {
    throw ZIOException("can not open file");
  }
  QTextStream in(&f);
  QString xml = in.readAll();

  readLeicaInfo(xml, QFileInfo(filename).absoluteDir(), imageInfos);
}

void ZImgLeica::parseXLCF(const QString& filename, std::vector<ImageInfo>& imageInfos)
{
  QFile f(filename);
  if (!f.open(QFile::ReadOnly | QFile::Text)) {
    throw ZIOException("can not open file");
  }
  QTextStream in(&f);
  QString xml = in.readAll();

  readLeicaInfo(xml, QFileInfo(filename).absoluteDir(), imageInfos);
}

void ZImgLeica::detectInfos(std::vector<ZImgInfo>& infos, const std::vector<ImageInfo>& leicaImageInfos)
{
  for (const auto& ii : leicaImageInfos) {
    ZImgInfo info;

    info.width = 1;
    info.height = 1;
    info.depth = 1;
    info.numTimes = 1;
    std::vector<VoxelSizeUnit> vsus;
    for (const auto& dd : ii.dimensions) {
      switch (dd.dimID) {
        case 1:
          info.width = dd.numberOfElements;
          info.voxelSizeX = dd.length / (std::max(dd.numberOfElements, 1_u32) - 1);
          break;
        case 2:
          info.height = dd.numberOfElements;
          info.voxelSizeY = dd.length / (std::max(dd.numberOfElements, 1_u32) - 1);
          break;
        case 3:
          info.depth = dd.numberOfElements;
          info.voxelSizeZ = dd.length / (std::max(dd.numberOfElements, 1_u32) - 1);
          break;
        case 4:
          info.numTimes = dd.numberOfElements;
          break;
        default:
          throw ZIOException("only support XYZT dimension");
      }
      if (dd.dimID == 1 || dd.dimID == 2 || dd.dimID == 3) {
        //"none", "inch", "cm", "mm", "um", "nm", "m", "hm", "km"
        if (dd.unit.compare("inch", Qt::CaseInsensitive) == 0) {
          vsus.push_back(VoxelSizeUnit::inch);
        } else if (dd.unit.compare("cm", Qt::CaseInsensitive) == 0) {
          vsus.push_back(VoxelSizeUnit::cm);
        } else if (dd.unit.compare("mm", Qt::CaseInsensitive) == 0) {
          vsus.push_back(VoxelSizeUnit::mm);
        } else if (dd.unit.compare("um", Qt::CaseInsensitive) == 0) {
          vsus.push_back(VoxelSizeUnit::um);
        } else if (dd.unit.compare("nm", Qt::CaseInsensitive) == 0) {
          vsus.push_back(VoxelSizeUnit::nm);
        } else if (dd.unit.compare("m", Qt::CaseInsensitive) == 0) {
          vsus.push_back(VoxelSizeUnit::m);
        } else if (dd.unit.compare("hm", Qt::CaseInsensitive) == 0) {
          vsus.push_back(VoxelSizeUnit::hm);
        } else if (dd.unit.compare("km", Qt::CaseInsensitive) == 0) {
          vsus.push_back(VoxelSizeUnit::km);
        } else {
          vsus.push_back(VoxelSizeUnit::none);
        }
      }
    }
    bool voxelSizeUnitConsistent = true;
    for (size_t i = 1; i < vsus.size(); ++i) {
      if (vsus[i] != vsus[0]) {
        voxelSizeUnitConsistent = false;
        break;
      }
    }
    if (voxelSizeUnitConsistent) {
      info.voxelSizeUnit = vsus[0];
    }

    info.numChannels = ii.channels.size();
    std::set<int32_t> dataTypes;
    std::set<uint32_t> resolutions;
    std::vector<uint64_t> channelOffsets;
    for (const auto& cd : ii.channels) {
      dataTypes.insert(cd.dataType);
      resolutions.insert(cd.resolution);
      channelOffsets.push_back(cd.bytesInc);
    }
    if (dataTypes.size() != 1) {
      throw ZIOException("inconsistent channel data types");
    } else {
      switch (*dataTypes.begin()) {
        case 0:
          info.voxelFormat = VoxelFormat::Unsigned;
          break;
        case 1:
          info.voxelFormat = VoxelFormat::Float;
          break;
        default:
          throw ZIOException("invalid channel data type");
      }
    }
    if (resolutions.size() != 1) {
      throw ZIOException("inconsistent channel resolutions");
    } else {
      uint32_t resl = *resolutions.begin();
      info.validBitCount = resl;
      info.bytesPerVoxel = (resl + 7) / 8;
    }
    std::vector<size_t> channelOffsetOrder = argSort(channelOffsets.begin(), channelOffsets.end());
    std::sort(channelOffsets.begin(), channelOffsets.end());
    if (channelOffsets.size() > 1) {
      uint64_t channelStride = channelOffsets[1] - channelOffsets[0];
      if (channelStride == 0) {
        throw ZIOException("invalid channel stride");
      }
      for (size_t i = 2; i < channelOffsets.size(); ++i) {
        if (channelOffsets[i] - channelOffsets[i - 1] != channelStride) {
          throw ZIOException("inconsistent channel strides not supported");
        }
      }
    }

    info.createDefaultDescriptions();

    for (size_t i = 0; i < info.channelColors.size(); ++i) {
      const auto& cd = ii.channels[channelOffsetOrder[i]];
      if (cd.channelTag == 1) {
        info.channelColors[i] = col4(255, 0, 0);
      } else if (cd.channelTag == 2) {
        info.channelColors[i] = col4(0, 255, 0);
      } else if (cd.channelTag == 3) {
        info.channelColors[i] = col4(0, 0, 255);
      } else if (cd.channelTag == 0) {
        if (cd.LUTName.compare("Gray", Qt::CaseInsensitive) == 0) {
          info.channelColors[i] = col4(255, 255, 255);
        } else if (cd.LUTName.compare("red", Qt::CaseInsensitive) == 0) {
          info.channelColors[i] = col4(255, 0, 0);
        } else if (cd.LUTName.compare("green", Qt::CaseInsensitive) == 0) {
          info.channelColors[i] = col4(0, 255, 0);
        } else if (cd.LUTName.compare("blue", Qt::CaseInsensitive) == 0) {
          info.channelColors[i] = col4(0, 0, 255);
        } else if (cd.LUTName.compare("cyan", Qt::CaseInsensitive) == 0) {
          info.channelColors[i] = col4(0, 255, 255);
        } else if (cd.LUTName.compare("magenta", Qt::CaseInsensitive) == 0) {
          info.channelColors[i] = col4(255, 0, 255);
        } else if (cd.LUTName.compare("yellow", Qt::CaseInsensitive) == 0) {
          info.channelColors[i] = col4(255, 255, 0);
        } else if (!cd.LUTName.isEmpty()) {
          throw ZIOException(
            QString("unhandled LUT name: %1, please send this message to flq@live.com").arg(cd.LUTName));
        }
      } else {
        throw ZIOException("invalid channel tag");
      }
    }

    if (info.timeStamps.size() == ii.timeStamps.size()) {
      info.timeStamps = ii.timeStamps;
    } else if (info.timeStamps.size() * info.numChannels == ii.timeStamps.size()) {
      for (size_t i = 0; i < info.timeStamps.size(); ++i) {
        info.timeStamps[i] = ii.timeStamps[i * info.numChannels];
      }
    } else if (info.timeStamps.size() * info.numChannels * info.depth == ii.timeStamps.size()) {
      for (size_t i = 0; i < info.timeStamps.size(); ++i) {
        info.timeStamps[i] = ii.timeStamps[i * info.numChannels * info.depth];
      }
    } else {
      throw ZIOException("unhandled time stamp list, please send this file to flq@live.com");
    }

    infos.push_back(info);
  }
}

} // namespace nim
