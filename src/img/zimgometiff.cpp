#include "zimgometiff.h"
#include "ztiff.h"
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include "QsLog.h"
#include "zimgsliceprovider.h"

namespace nim {

ZImgOmeTiff::ZImgOmeTiff()
  : ZImgTiff()
{
}

void ZImgOmeTiff::readIntoInternalStructure(const QString &filename, ZTiff &tiff)
{
  ZImgTiff::readIntoInternalStructure(filename, tiff);
  if (!m_imageDescription.isEmpty() && m_imageDescription.contains("<OME") && m_imageDescription.contains("<Image") &&
      m_imageDescription.contains("<Pixels"))
    readOmeInfo(tiff);
  else
    throw ZIOException("Not OME Tiff file");
}

void ZImgOmeTiff::clearInternalState()
{
  m_omeImgInfo.clear();
  m_ifdIdxPosMap.clear();
}

void ZImgOmeTiff::detectImgInfo(ZTiff &tiff)
{
  ZImgTiff::detectImgInfo(tiff);

  // overwrite some fields based on ome information
  if (m_imgInfo[0].width != m_omeImgInfo.width ||
      m_imgInfo[0].height != m_omeImgInfo.height ||
      m_imgInfo[0].numChannels != 1 ||
      m_imgInfo[0].bytesPerVoxel != m_omeImgInfo.bytesPerVoxel ||
      m_imgInfo[0].voxelFormat != m_omeImgInfo.voxelFormat)
    throw ZIOException(QString("ome meta info <%1> doesn't match image data <%2>").
                       arg(m_omeImgInfo.toQString()).arg(m_imgInfo[0].toQString()));

  m_imgInfo[0].numChannels = m_omeImgInfo.numChannels;
  m_imgInfo[0].depth = m_omeImgInfo.depth;
  m_imgInfo[0].numTimes = m_omeImgInfo.numTimes;

  m_imgInfo[0].voxelSizeUnit = m_omeImgInfo.voxelSizeUnit;
  m_imgInfo[0].voxelSizeX = m_omeImgInfo.voxelSizeX;
  m_imgInfo[0].voxelSizeY = m_omeImgInfo.voxelSizeY;
  m_imgInfo[0].voxelSizeZ = m_omeImgInfo.voxelSizeZ;

  m_imgInfo[0].channelColors = m_omeImgInfo.channelColors;
  m_imgInfo[0].channelNames = m_omeImgInfo.channelNames;
  m_imgInfo[0].timeStamps = m_omeImgInfo.timeStamps;
  //m_imgInfo.locations = m_omeImgInfo.locations;

  m_imgInfo[0].createDefaultDescriptions();

  //LINFO() << m_imgInfo.toQString() << m_dimensionOrder;
}

bool ZImgOmeTiff::mapIFDToImgLocation(size_t ifdIdx, int &z, int &c, int &t, int &l)
{
  if (m_ifdIdxPosMap.find(ifdIdx) == m_ifdIdxPosMap.end())
    return false;
  IFDPos pos = m_ifdIdxPosMap[ifdIdx];
  z = pos.z;
  c = pos.c;
  t = pos.t;
  l = 0;
  //LINFO() << ifdIdx << z << c << t << l;
  return true;
}

QString ZImgOmeTiff::shortName() const
{
  return "OME Tiff";
}

QString ZImgOmeTiff::fullName() const
{
  return "OME Tiff";
}

QStringList ZImgOmeTiff::extensions() const
{
  QStringList res;
  res << "ome.tif" << "ome.tiff";
  return res;
}

void ZImgOmeTiff::writeImg(const QString &filename, const ZImg &img, Compression comp)
{
  ZTiffWriter tiffWriter;
  int extraSample = img.info().lastChannelIsAlphaChannel ? 2 : -1;  //EXTRASAMPLE_UNASSALPHA or none
  if (img.byteNumber() > size_t(1024)*1024*3600)
    tiffWriter.startWriting(filename, comp, extraSample, true);
  else
    tiffWriter.startWriting(filename, comp, extraSample, false);
  std::vector<ZImgMetatag> tags(1);
  makeImageDescriptionTag(img.info(), "XYZCT", tags[0]);
  for (size_t t=0; t<img.numTimes(); ++t) {
    for (size_t c=0; c<img.numChannels(); ++c) {
      for (size_t z=0; z<img.depth(); ++z) {
        //LINFO() << l << t << z << c << img.info().toQString();
        if (t==0 && z==0 && c==0)
          tiffWriter.writeIFD(img, z, t, c, false, tags);
        else
          tiffWriter.writeIFD(img, z, t, c, false);
      }
    }
  }
}

void ZImgOmeTiff::writeImg(const QString &filename, const ZImgSliceProvider &imgSliceProvider, Compression comp)
{
  ZTiffWriter tiffWriter;
  int extraSample = imgSliceProvider.imgInfo().lastChannelIsAlphaChannel ? 2 : -1;  //EXTRASAMPLE_UNASSALPHA or none
  if (imgSliceProvider.imgInfo().byteNumber() > size_t(1024)*1024*3600)
    tiffWriter.startWriting(filename, comp, extraSample, true);
  else
    tiffWriter.startWriting(filename, comp, extraSample, false);
  std::vector<ZImgMetatag> tags(1);
  makeImageDescriptionTag(imgSliceProvider.imgInfo(), "XYCZT", tags[0]);
  for (size_t t=0; t<imgSliceProvider.imgInfo().numTimes; ++t) {
    for (size_t z=0; z<imgSliceProvider.imgInfo().depth; ++z) {
      ZImg img = imgSliceProvider.slice(z,t);
      for (size_t c=0; c<imgSliceProvider.imgInfo().numChannels; ++c) {
        //LINFO() << l << t << z << c << img.info().toQString();
        if (t==0 && z==0 && c==0)
          tiffWriter.writeIFD(img, 0, 0, c, false, tags);
        else
          tiffWriter.writeIFD(img, 0, 0, c, false);
      }
    }
  }
}

bool ZImgOmeTiff::supportRead() const
{
  return true;
}

bool ZImgOmeTiff::supportWrite() const
{
  return true;
}

void ZImgOmeTiff::readOmeInfo(ZTiff &tiff)
{
  // QXmlStreamReader takes any QIODevice.
  QXmlStreamReader xml(m_imageDescription);

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
      if (xml.name() != "OME") {
        continue;
      }
      parseOME(xml, tiff);
      break;
    }
  }
  // Error handling.
  if (xml.hasError()) {
    throw ZIOException(QString("error parsing ome xml: %1").arg(xml.errorString()));
  }
  xml.clear();
}

void ZImgOmeTiff::makeImageDescriptionTag(const ZImgInfo &info, const QString &dimensionOrder, ZImgMetatag &tag)
{
  tag.setTag(270); // TIFFTAG_IMAGEDESCRIPTION
  tag.setName("ImageDescription");
  tag.setDataType(DataType::Ascii);

  QByteArray xml = createOmeXml(info, dimensionOrder).toUtf8();
  tag.setCount(xml.size()+1);
  memcpy(tag.dataArray<char>(), xml.constData(), tag.dataByteNumber());
}

void ZImgOmeTiff::parseOME(QXmlStreamReader &xml, ZTiff &tiff)
{
  Q_ASSERT(xml.isStartElement() && xml.name() == "OME");

  while (xml.readNextStartElement()) {
    if (xml.name() == "Image") {
      while (xml.readNextStartElement()) {
        if (xml.name() == "Pixels") {
          parsePixels(xml, tiff);
        } else {
          xml.skipCurrentElement();
        }
      }
    } else {
      xml.skipCurrentElement();
    }
  }
}

void ZImgOmeTiff::parsePixels(QXmlStreamReader &xml, ZTiff &tiff)
{
  // Let's get the attributes
  QXmlStreamAttributes attributes = xml.attributes();

  if (attributes.hasAttribute("DimensionOrder")) {
    QString dimOrder = attributes.value("DimensionOrder").toString();
    dimOrder = dimOrder.remove(0, 2) + "L";
    setDimensionOrder(dimOrder);
  }
  m_omeImgInfo.voxelSizeUnit = VoxelSizeUnit::um;
  if (attributes.hasAttribute("PhysicalSizeX")) {
    bool ok;
    double ps = attributes.value("PhysicalSizeX").toString().toDouble(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome PhysicalSizeX");
    m_omeImgInfo.voxelSizeX = ps;
  }
  if (attributes.hasAttribute("PhysicalSizeY")) {
    bool ok;
    double ps = attributes.value("PhysicalSizeY").toString().toDouble(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome PhysicalSizeY");
    m_omeImgInfo.voxelSizeY = ps;
  }
  if (attributes.hasAttribute("PhysicalSizeZ")) {
    bool ok;
    double ps = attributes.value("PhysicalSizeZ").toString().toDouble(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome PhysicalSizeZ");
    m_omeImgInfo.voxelSizeZ = ps;
  }
  m_omeImgInfo.numTimes = 1;
  m_omeImgInfo.numChannels = 1;
  m_omeImgInfo.depth = 1;
  m_omeImgInfo.width = 1;
  m_omeImgInfo.height = 1;
  if (attributes.hasAttribute("SizeX")) {
    bool ok;
    int sz = attributes.value("SizeX").toString().toInt(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome SizeX");
    m_omeImgInfo.width = sz;
  }
  if (attributes.hasAttribute("SizeY")) {
    bool ok;
    int sz = attributes.value("SizeY").toString().toInt(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome SizeY");
    m_omeImgInfo.height = sz;
  }
  if (attributes.hasAttribute("SizeZ")) {
    bool ok;
    int sz = attributes.value("SizeZ").toString().toInt(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome SizeZ");
    m_omeImgInfo.depth = sz;
  }
  if (attributes.hasAttribute("SizeC")) {
    bool ok;
    int sz = attributes.value("SizeC").toString().toInt(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome SizeC");
    m_omeImgInfo.numChannels = sz;
  }
  if (attributes.hasAttribute("SizeT")) {
    bool ok;
    int sz = attributes.value("SizeT").toString().toInt(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome SizeT");
    m_omeImgInfo.numTimes = sz;
  }
  if (attributes.hasAttribute("TimeIncrement")) {
    bool ok;
    double ti = attributes.value("TimeIncrement").toString().toDouble(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome TimeIncrement");
    m_omeImgInfo.timeStamps.resize(m_omeImgInfo.numTimes);
    m_omeImgInfo.timeStamps[0] = 0;
    for (size_t i=1; i<m_omeImgInfo.numTimes; ++i)
      m_omeImgInfo.timeStamps[i] = ti + m_omeImgInfo.timeStamps[i-1];
  }
  m_omeImgInfo.voxelFormat = VoxelFormat::Unsigned;
  QString type;
  if (attributes.hasAttribute("Type")) {
    type = attributes.value("Type").toString();
  } else if (attributes.hasAttribute("PixelType")) {
    type = attributes.value("PixelType").toString();
  }
  if (type.isEmpty()) {
    throw ZIOException("Can not find ome PixelType or Type attribute");
  } else if (type.compare("int8", Qt::CaseInsensitive) == 0) {
    m_omeImgInfo.bytesPerVoxel = 1;
    m_omeImgInfo.voxelFormat = VoxelFormat::Signed;
  } else if (type.compare("int16", Qt::CaseInsensitive) == 0) {
    m_omeImgInfo.bytesPerVoxel = 2;
    m_omeImgInfo.voxelFormat = VoxelFormat::Signed;
  } else if (type.compare("int32", Qt::CaseInsensitive) == 0) {
    m_omeImgInfo.bytesPerVoxel = 4;
    m_omeImgInfo.voxelFormat = VoxelFormat::Signed;
  } else if (type.compare("uint8", Qt::CaseInsensitive) == 0) {
    m_omeImgInfo.bytesPerVoxel = 1;
  } else if (type.compare("uint16", Qt::CaseInsensitive) == 0) {
    m_omeImgInfo.bytesPerVoxel = 2;
  } else if (type.compare("uint32", Qt::CaseInsensitive) == 0) {
    m_omeImgInfo.bytesPerVoxel = 4;
  } else if (type.compare("float", Qt::CaseInsensitive) == 0) {
    m_omeImgInfo.bytesPerVoxel = 4;
    m_omeImgInfo.voxelFormat = VoxelFormat::Float;
  } else if (type.compare("double", Qt::CaseInsensitive) == 0) {
    m_omeImgInfo.bytesPerVoxel = 8;
    m_omeImgInfo.voxelFormat = VoxelFormat::Float;
  } else {
    throw ZIOException(QString("Not supported ome type: %1").arg(type));
  }

  while (xml.readNextStartElement()) {
    if (xml.name() == "TiffData") {
      parseTiffData(xml, tiff);
    } else if (xml.name() == "Channel") {
      parseChannel(xml);
    } else {
      xml.skipCurrentElement();
    }
  }
}

void ZImgOmeTiff::parseTiffData(QXmlStreamReader &xml, ZTiff &tiff)
{
  // Let's get the attributes
  QXmlStreamAttributes attributes = xml.attributes();

  size_t ifd = 0;
  size_t planeCount = tiff.ifds().size();
  if (attributes.hasAttribute("IFD")) {
    bool ok;
    ifd = attributes.value("IFD").toString().toInt(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome TiffData IFD");
    planeCount = 1;
  }
  if (attributes.hasAttribute("PlaneCount")) {
    bool ok;
    planeCount = attributes.value("PlaneCount").toString().toInt(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome TiffData IFD");
  }
  size_t firstZ = 0;
  size_t firstT = 0;
  size_t firstC = 0;
  if (attributes.hasAttribute("FirstZ")) {
    bool ok;
    firstZ = attributes.value("FirstZ").toString().toInt(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome TiffData FirstZ");
  }
  if (attributes.hasAttribute("FirstT")) {
    bool ok;
    firstT = attributes.value("FirstT").toString().toInt(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome TiffData FirstT");
  }
  if (attributes.hasAttribute("FirstC")) {
    bool ok;
    firstC = attributes.value("FirstC").toString().toInt(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome TiffData FirstC");
  }

  for (size_t i=ifd; i<ifd+planeCount; ++i) {
    int t;
    int c;
    int l;
    int z;
    if (IFDToLoc(i, z, c, t, l, ifd, m_dimensionOrder, m_omeImgInfo, 1,
                 firstZ, firstC, firstT, 0)) {
      //LINFO() << i << z << c << t << l << m_dimensionOrder;
      m_ifdIdxPosMap[i] = IFDPos(z, c, t);
    }
  }

  xml.skipCurrentElement();
}

void ZImgOmeTiff::parseChannel(QXmlStreamReader &xml)
{
  // Let's get the attributes
  QXmlStreamAttributes attributes = xml.attributes();

  if (attributes.hasAttribute("Name")) {
    QString name = attributes.value("Name").toString();
    m_omeImgInfo.channelNames.push_back(name);
  }
  if (attributes.hasAttribute("Color")) {
    bool ok;
    int color = attributes.value("Color").toString().toInt(&ok);
    if (!ok)
      throw ZIOException("Can not parse ome channel Color");
    col4 col;
    memcpy(&col, &color, 3);
    std::swap(col.r, col.b);
    col.a = 255;
    m_omeImgInfo.channelColors.push_back(col);
  }

  xml.skipCurrentElement();
}

QString ZImgOmeTiff::createOmeXml(const ZImgInfo &info, const QString& dimensionOrder)
{
  QByteArray res;  // no " encoding="UTF-8" " if use QString
  QXmlStreamWriter xml(&res);
  xml.setCodec("UTF-8");
  xml.writeStartDocument();

  xml.writeStartElement("OME");
  xml.writeAttribute("xmlns:xsd", "http://www.w3.org/2001/XMLSchema");
  xml.writeAttribute("xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance");
  xml.writeAttribute("xmlns", "http://www.openmicroscopy.org/Schemas/OME/2013-06");
  xml.writeAttribute("xmlns:ROI", "http://www.openmicroscopy.org/Schemas/ROI/2013-06");
  xml.writeAttribute("xmlns:OME", "http://www.openmicroscopy.org/Schemas/OME/2013-06");
  xml.writeAttribute("xmlns:BIN", "http://www.openmicroscopy.org/Schemas/BinaryFile/2013-06");
  xml.writeAttribute("xsi:schemaLocation", "http://www.openmicroscopy.org/Schemas/OME/2013-06 http://www.openmicroscopy.org/Schemas/OME/2013-06/ome.xsd");

  xml.writeStartElement("Image");
  xml.writeAttribute("ID", "Image:0");
  xml.writeAttribute("Name", "");
  xml.writeEmptyElement("Description");
  xml.writeStartElement("Pixels");
  xml.writeAttribute("ID", "Pixels:0");
  xml.writeAttribute("DimensionOrder", dimensionOrder);
  if (info.voxelFormat == VoxelFormat::Float) {
    if (info.bytesPerVoxel == 4)
      xml.writeAttribute("Type", "float");
    else if (info.bytesPerVoxel == 8)
      xml.writeAttribute("Type", "double");
    else
      throw ZIOException(QString("%1 bytes float pixel?").arg(info.bytesPerVoxel));
  } else if (info.voxelFormat == VoxelFormat::Signed) {
    xml.writeAttribute("Type", QString("int%1").arg(info.bytesPerVoxel * 8));
  } else {
    xml.writeAttribute("Type", QString("uint%1").arg(info.bytesPerVoxel * 8));
  }
  xml.writeAttribute("SizeX", QString("%1").arg(info.width));
  xml.writeAttribute("SizeY", QString("%1").arg(info.height));
  xml.writeAttribute("SizeZ", QString("%1").arg(info.depth));
  xml.writeAttribute("SizeC", QString("%1").arg(info.numChannels));
  xml.writeAttribute("SizeT", QString("%1").arg(info.numTimes));
  if (info.voxelSizeUnit != VoxelSizeUnit::none) {
    xml.writeAttribute("PhysicalSizeX", QString("%1").arg(info.voxelSizeXInUm()));
    xml.writeAttribute("PhysicalSizeY", QString("%1").arg(info.voxelSizeYInUm()));
    xml.writeAttribute("PhysicalSizeZ", QString("%1").arg(info.voxelSizeZInUm()));
  }

  for (size_t i=0; i<info.numChannels; ++i) {
    xml.writeStartElement("Channel");
    xml.writeAttribute("ID", QString("Channel:0:%1").arg(i));
    xml.writeAttribute("Name", info.channelNames[i]);
    col4 col = info.channelColors[i];
    col.a = 0;
    std::swap(col.r, col.b);
    int color = bit_cast<int>(col);
    xml.writeAttribute("Color", QString("%1").arg(color));
    xml.writeEndElement();
  }

  xml.writeEmptyElement("TiffData");
  xml.writeEndElement();
  xml.writeEndElement();
  xml.writeEndElement();

  xml.writeEndDocument();

  return res;
}

}  // namespace
