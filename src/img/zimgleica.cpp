#include "zimgleica.h"

#include "zlog.h"
#include "zioutils.h"
#include <QUuid>
#include <QXmlStreamReader>
#include <QFile>
#include <QTextStream>

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

  readLeicaInfo(xml);

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

}

void ZImgLeica::clearInternalState()
{
  m_version = 0;
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
      do {
        if (!inputFileStream.read(reinterpret_cast<char*>(&nb), sizeof(NextBlock))) {
          break;
        }

        if (m_version == 1) {
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
        } else if (m_version == 2) {
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
          throw ZIOException(QString("not supported leica lif version: %1").arg(m_version));
        }
      } while (true);
    }
  }

  std::cout << xml.toLocal8Bit();
}

void ZImgLeica::readLeicaInfo(const QString& xmlString)
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

      QXmlStreamAttributes attributes = xml.attributes();
      bool ok = false;
      if (attributes.hasAttribute("Version")) {
        m_version = attributes.value("Version").toInt(&ok);
      }
      if (!ok) {
        throw ZIOException("Can not parse Leica file version");
      }

      parseMetadata(xml);
      break;
    }
  }
  // Error handling.
  if (xml.hasError()) {
    throw ZIOException(QString("error parsing leica metadata xml: %1").arg(xml.errorString()));
  }
  xml.clear();
}

void ZImgLeica::parseMetadata(QXmlStreamReader& xml)
{
  CHECK(xml.isStartElement() && xml.name() == "LMSDataContainerHeader");

  while (xml.readNextStartElement()) {
    if (xml.name() == "Element") {
      parseElement(xml);
    } else {
      xml.skipCurrentElement();
    }
  }

//  if (m_voxelSizeX > 0 && m_voxelSizeY > 0) {
//    m_hasVoxelSizeInfo = true;
//    m_voxelSizeX *= 1e6;
//    m_voxelSizeY *= 1e6;
//    m_voxelSizeZ = m_voxelSizeZ <= 0 ? 1 : (m_voxelSizeZ * 1e6);
//    //LOG(INFO) << m_voxelSizeX << " " << m_voxelSizeY << " " << m_voxelSizeZ;
//  }
//
//  if (m_channelColors.size() < m_channelNames.size()) {
//    m_channelColors.clear();
//  }
//  if (m_channelPixelType.size() < m_channelNames.size()) {
//    m_channelPixelType.clear();
//  }
//  if (m_channelValidBitCount.size() < m_channelNames.size()) {
//    m_channelValidBitCount.clear();
//  }
//  if (m_channelColorsFromDisplaySettings.size() < m_channelNamesFromDisplaySettings.size()) {
//    m_channelColorsFromDisplaySettings.clear();
//  }
//
//  if (m_channelNamesFromDisplaySettings.size() == m_channelNames.size()) {
//    for (size_t i = 0; i < m_channelNames.size(); ++i) {
//      if (m_channelNames[i].isEmpty())
//        m_channelNames[i] = m_channelNamesFromDisplaySettings[i];
//    }
//    if (m_channelColors.empty() && !m_channelColorsFromDisplaySettings.empty())
//      m_channelColors = m_channelColorsFromDisplaySettings;
//  }
//  //  for (size_t i=0; i<channelNames.size(); ++i) {
//  //    LOG(INFO) << channelNames[i] << " " << channelIs12Bit[i] << " " << channelPixelType[i];
//  //  }
//  // channels have different data types
//  for (size_t i = 1; i < m_channelPixelType.size(); ++i) {
//    if (m_channelPixelType[i] != m_channelPixelType[0]) {
//      m_shouldSeparateChannelsToDifferentScenes = true;
//      break;
//    }
//  }
//  // more than 1 channel and each channel contains BGR image
//  if (!m_shouldSeparateChannelsToDifferentScenes && m_channelPixelType.size() > 1 &&
//      pixelTypeIsBGR(m_channelPixelType[0])) {
//    m_shouldSeparateChannelsToDifferentScenes = true;
//  }
//
//  if (m_channelNames.size() > 0)
//    m_hasChannelInfo = true;
//
//  if (m_sceneCenterX.size() > 0 && m_sceneCenterX.size() == m_sceneCenterY.size()) {
//    m_hasSceneInfo = true;
//  }
}

void ZImgLeica::parseElement(QXmlStreamReader& xml)
{
  QString name;
  bool visibilityInvalid = true;
  bool copyOptionInvalid = true;

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
  }

  while (xml.readNextStartElement()) {
    if (xml.name() == "Color") {
      QString colorStr = xml.readElementText();
      if (colorStr.startsWith("#")) {
        colorStr = colorStr.mid(1);
      }
      if (colorStr.size() == 8) {
        colorStr = colorStr.mid(2);
      }
      if (colorStr.size() == 6) {
        int color = colorStr.toInt(&ok, 16);
        if (ok) {
          std::memcpy(&col, &color, 3);
          std::swap(col.r, col.b);
          col.a = 255;
          hasColor = true;
        } else {
          LOG(WARNING) << "can not parse czi channel color " << colorStr;
        }
      }
    } else if (xml.name() == "PixelType") {
      QString pixelTypeStr = xml.readElementText();
      if (pixelTypeStr.isEmpty()) {
        throw ZIOException("Can not parse czi channel pixel type");
      } else if (pixelTypeStr.compare("Gray8", Qt::CaseInsensitive) == 0) {
        pixelType = 0;
      } else if (pixelTypeStr.compare("Gray16", Qt::CaseInsensitive) == 0) {
        pixelType = 1;
      } else if (pixelTypeStr.compare("Gray32Float", Qt::CaseInsensitive) == 0) {
        pixelType = 2;
      } else if (pixelTypeStr.compare("Bgr24", Qt::CaseInsensitive) == 0) {
        pixelType = 3;
      } else if (pixelTypeStr.compare("Bgr48", Qt::CaseInsensitive) == 0) {
        pixelType = 4;
      } else if (pixelTypeStr.compare("Bgr96Float", Qt::CaseInsensitive) == 0) {
        pixelType = 8;
      } else if (pixelTypeStr.compare("Bgra32", Qt::CaseInsensitive) == 0) {
        pixelType = 9;
      } else if (pixelTypeStr.compare("Gray64ComplexFloat", Qt::CaseInsensitive) == 0) {
        pixelType = 10;
      } else if (pixelTypeStr.compare("Gray192ComplexFloat", Qt::CaseInsensitive) == 0) {
        pixelType = 11;
      } else if (pixelTypeStr.compare("Gray32", Qt::CaseInsensitive) == 0) {
        pixelType = 12;
      } else if (pixelTypeStr.compare("Gray64", Qt::CaseInsensitive) == 0) {
        pixelType = 13;
      } else {
        throw ZIOException(QString("Not supported czi pixel type: %1").arg(pixelTypeStr));
      }
      hasPixelType = true;
    } else if (xml.name() == "ComponentBitCount") {
      bc = xml.readElementText().toInt(&ok);
      if (!ok)
        throw ZIOException("Can not parse czi bit count");
      hasBC = true;
    } else {
      xml.skipCurrentElement();
    }
  }

//  m_channelNames.push_back(name);
//  if (hasBC)
//    m_channelValidBitCount.push_back(bc);
//  if (hasPixelType)
//    m_channelPixelType.push_back(pixelType);
//  if (hasColor) {
//    m_channelColors.push_back(col);
//  }
}

void ZImgLeica::parseReference(QXmlStreamReader& xml)
{
  double sceneCenterX;
  double sceneCenterY;

  bool ok;
  while (xml.readNextStartElement()) {
    if (xml.name() == "CenterPosition") {
      QString centerPositionStr = xml.readElementText();
      QStringList nums = centerPositionStr.split(",");
      if (nums.size() != 2)
        return;
      sceneCenterX = nums[0].toDouble(&ok);
      if (!ok)
        return;
      sceneCenterY = nums[1].toDouble(&ok);
      if (!ok)
        return;
      //m_sceneCenterX.push_back(sceneCenterX);
      //m_sceneCenterY.push_back(sceneCenterY);
    } else {
      xml.skipCurrentElement();
    }
  }
}

} // namespace nim
