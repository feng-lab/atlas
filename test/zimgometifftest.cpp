#include "zimg.h"
#include "ztiff.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace nim {
namespace {

QString omeXmlForTwoSeries()
{
  return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<OME xmlns="http://www.openmicroscopy.org/Schemas/OME/2016-06"
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://www.openmicroscopy.org/Schemas/OME/2016-06 http://www.openmicroscopy.org/Schemas/OME/2016-06/ome.xsd">
  <Image ID="Image:0" Name="mapped-to-ifd-1">
    <Pixels ID="Pixels:0" DimensionOrder="XYZCT" Type="uint8" SizeX="2" SizeY="2" SizeZ="1" SizeC="1" SizeT="1">
      <Channel ID="Channel:0:0" Name="series0"/>
      <TiffData IFD="1" FirstZ="0" FirstC="0" FirstT="0" PlaneCount="1"/>
    </Pixels>
  </Image>
  <Image ID="Image:1" Name="mapped-to-ifd-0">
    <Pixels ID="Pixels:1" DimensionOrder="XYZCT" Type="uint8" SizeX="2" SizeY="2" SizeZ="1" SizeC="1" SizeT="1">
      <Channel ID="Channel:1:0" Name="series1"/>
      <TiffData IFD="0" FirstZ="0" FirstC="0" FirstT="0" PlaneCount="1"/>
    </Pixels>
  </Image>
</OME>)");
}

QString omeXmlForPyramid()
{
  return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<OME xmlns="http://www.openmicroscopy.org/Schemas/OME/2016-06"
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://www.openmicroscopy.org/Schemas/OME/2016-06 http://www.openmicroscopy.org/Schemas/OME/2016-06/ome.xsd">
  <Image ID="Image:0" Name="pyramid">
    <Pixels ID="Pixels:0" DimensionOrder="XYZCT" Type="uint8" SizeX="4" SizeY="4" SizeZ="1" SizeC="1" SizeT="1">
      <Channel ID="Channel:0:0" Name="signal"/>
      <TiffData IFD="0" FirstZ="0" FirstC="0" FirstT="0" PlaneCount="1"/>
    </Pixels>
  </Image>
</OME>)");
}

QString omeXmlForDefaultTiffData()
{
  return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<OME xmlns="http://www.openmicroscopy.org/Schemas/OME/2016-06"
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://www.openmicroscopy.org/Schemas/OME/2016-06 http://www.openmicroscopy.org/Schemas/OME/2016-06/ome.xsd">
  <Image ID="Image:0" Name="default-tiffdata">
    <Pixels ID="Pixels:0" DimensionOrder="XYZCT" Type="uint8" SizeX="2" SizeY="2" SizeZ="2" SizeC="2" SizeT="1">
      <Channel ID="Channel:0:0" Name="c0"/>
      <Channel ID="Channel:0:1" Name="c1"/>
    </Pixels>
  </Image>
</OME>)");
}

QString omeXmlForMetadataOnlyPlusPixelSeries()
{
  return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<OME xmlns="http://www.openmicroscopy.org/Schemas/OME/2016-06"
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://www.openmicroscopy.org/Schemas/OME/2016-06 http://www.openmicroscopy.org/Schemas/OME/2016-06/ome.xsd">
  <Image ID="Image:0" Name="metadata-only">
    <Pixels ID="Pixels:0" DimensionOrder="XYZCT" Type="uint8" SizeX="2" SizeY="2" SizeZ="1" SizeC="1" SizeT="1">
      <MetadataOnly/>
    </Pixels>
  </Image>
  <Image ID="Image:1" Name="pixel-data">
    <Pixels ID="Pixels:1" DimensionOrder="XYZCT" Type="uint8" SizeX="2" SizeY="2" SizeZ="1" SizeC="1" SizeT="1">
      <Channel ID="Channel:1:0" Name="signal"/>
      <TiffData IFD="0" FirstZ="0" FirstC="0" FirstT="0" PlaneCount="1"/>
    </Pixels>
  </Image>
</OME>)");
}

QString omeXmlForReverseTime()
{
  return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<OME xmlns="http://www.openmicroscopy.org/Schemas/OME/2016-06"
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://www.openmicroscopy.org/Schemas/OME/2016-06 http://www.openmicroscopy.org/Schemas/OME/2016-06/ome.xsd">
  <Image ID="Image:0" Name="reverse-time">
    <Pixels ID="Pixels:0" DimensionOrder="XYZCT" Type="uint8" SizeX="2" SizeY="2" SizeZ="1" SizeC="1" SizeT="2">
      <Channel ID="Channel:0:0" Name="signal"/>
      <TiffData IFD="0" FirstZ="0" FirstC="0" FirstT="1" PlaneCount="1"/>
      <TiffData IFD="1" FirstZ="0" FirstC="0" FirstT="0" PlaneCount="1"/>
    </Pixels>
  </Image>
</OME>)");
}

QString omeXmlForOneBasedIfdCompatibility()
{
  return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<OME xmlns="http://www.openmicroscopy.org/Schemas/OME/2016-06"
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://www.openmicroscopy.org/Schemas/OME/2016-06 http://www.openmicroscopy.org/Schemas/OME/2016-06/ome.xsd">
  <Image ID="Image:0" Name="one-based-ifd">
    <Pixels ID="Pixels:0" DimensionOrder="XYZCT" Type="uint8" SizeX="2" SizeY="2" SizeZ="1" SizeC="1" SizeT="1">
      <Channel ID="Channel:0:0" Name="signal"/>
      <TiffData IFD="1" FirstZ="0" FirstC="0" FirstT="0" PlaneCount="1"/>
    </Pixels>
  </Image>
</OME>)");
}

QString omeXmlForMissingPlane()
{
  return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<OME xmlns="http://www.openmicroscopy.org/Schemas/OME/2016-06"
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://www.openmicroscopy.org/Schemas/OME/2016-06 http://www.openmicroscopy.org/Schemas/OME/2016-06/ome.xsd">
  <Image ID="Image:0" Name="missing-plane">
    <Pixels ID="Pixels:0" DimensionOrder="XYZCT" Type="uint8" SizeX="2" SizeY="2" SizeZ="2" SizeC="1" SizeT="1">
      <Channel ID="Channel:0:0" Name="signal"/>
      <TiffData IFD="0" FirstZ="0" FirstC="0" FirstT="0" PlaneCount="1"/>
    </Pixels>
  </Image>
</OME>)");
}

QString omeXmlForDuplicatePlane()
{
  return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<OME xmlns="http://www.openmicroscopy.org/Schemas/OME/2016-06"
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://www.openmicroscopy.org/Schemas/OME/2016-06 http://www.openmicroscopy.org/Schemas/OME/2016-06/ome.xsd">
  <Image ID="Image:0" Name="duplicate-plane">
    <Pixels ID="Pixels:0" DimensionOrder="XYZCT" Type="uint8" SizeX="2" SizeY="2" SizeZ="1" SizeC="1" SizeT="1">
      <Channel ID="Channel:0:0" Name="signal"/>
      <TiffData IFD="0" FirstZ="0" FirstC="0" FirstT="0" PlaneCount="1"/>
      <TiffData IFD="1" FirstZ="0" FirstC="0" FirstT="0" PlaneCount="1"/>
    </Pixels>
  </Image>
</OME>)");
}

QString omeXmlForMultiFile(const QString& pixelFilename)
{
  return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<OME xmlns="http://www.openmicroscopy.org/Schemas/OME/2016-06"
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://www.openmicroscopy.org/Schemas/OME/2016-06 http://www.openmicroscopy.org/Schemas/OME/2016-06/ome.xsd">
  <Image ID="Image:0" Name="multifile">
    <Pixels ID="Pixels:0" DimensionOrder="XYZCT" Type="uint8" SizeX="2" SizeY="2" SizeZ="1" SizeC="1" SizeT="1">
      <Channel ID="Channel:0:0" Name="signal"/>
      <TiffData IFD="0" FirstZ="0" FirstC="0" FirstT="0" PlaneCount="1">
        <UUID FileName="%1">urn:uuid:11111111-1111-1111-1111-111111111111</UUID>
      </TiffData>
    </Pixels>
  </Image>
</OME>)")
    .arg(pixelFilename);
}

QString binaryOnlyXml(const QString& metadataFilename)
{
  return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<OME xmlns="http://www.openmicroscopy.org/Schemas/OME/2016-06">
  <BinaryOnly MetadataFile="%1" UUID="urn:uuid:22222222-2222-2222-2222-222222222222"/>
</OME>)")
    .arg(metadataFilename);
}

QString omeXmlForSamplesPerPixel()
{
  return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<OME xmlns="http://www.openmicroscopy.org/Schemas/OME/2016-06"
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://www.openmicroscopy.org/Schemas/OME/2016-06 http://www.openmicroscopy.org/Schemas/OME/2016-06/ome.xsd">
  <Image ID="Image:0" Name="rgb-like">
    <Pixels ID="Pixels:0" DimensionOrder="XYCZT" Type="uint8" SizeX="2" SizeY="2" SizeZ="1" SizeC="6" SizeT="1">
      <Channel ID="Channel:0:0" Name="rgb0" SamplesPerPixel="3"/>
      <Channel ID="Channel:0:1" Name="rgb1" SamplesPerPixel="3"/>
      <TiffData IFD="0" FirstZ="0" FirstC="0" FirstT="0" PlaneCount="1"/>
      <TiffData IFD="1" FirstZ="0" FirstC="3" FirstT="0" PlaneCount="1"/>
    </Pixels>
  </Image>
</OME>)");
}

ZImg singleChannelImage(size_t width, size_t height, uint8_t firstValue)
{
  ZImg img(ZImgInfo(width, height, 1, 1, 1, 1, VoxelFormat::Unsigned));
  for (size_t y = 0; y < height; ++y) {
    for (size_t x = 0; x < width; ++x) {
      *img.data<uint8_t>(x, y) = static_cast<uint8_t>(firstValue + y * width + x);
    }
  }
  return img;
}

ZImg multiChannelImage(size_t width, size_t height, std::initializer_list<uint8_t> firstValues)
{
  ZImg img(ZImgInfo(width, height, 1, firstValues.size(), 1, 1, VoxelFormat::Unsigned));
  size_t c = 0;
  for (uint8_t firstValue : firstValues) {
    for (size_t y = 0; y < height; ++y) {
      for (size_t x = 0; x < width; ++x) {
        *img.data<uint8_t>(x, y, 0, c) = static_cast<uint8_t>(firstValue + y * width + x);
      }
    }
    ++c;
  }
  return img;
}

void writeSingleIfd(ZTiffWriter& writer, const ZImg& img, const std::vector<ZImgMetatag>& tags = {})
{
  writer.writeIFD(img, 0, 0, 0, false, tags);
}

void writeTextFile(const QString& filename, const QString& text)
{
  QFile file(filename);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
  ASSERT_EQ(file.write(text.toUtf8()), text.toUtf8().size());
}

TEST(ZImgOmeTiff, TiffDataMapsMultipleSeriesWithoutIfdOrderAssumption)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("mapped.ome.tif"));

  ZTiffWriter writer;
  writer.startWriting(filename, Compression::NONE, -1, false);
  std::vector<ZImgMetatag> tags;
  tags.emplace_back("ImageDescription", omeXmlForTwoSeries(), 270);
  writeSingleIfd(writer, singleChannelImage(2, 2, 20), tags);
  writeSingleIfd(writer, singleChannelImage(2, 2, 10));
  writer.finishWriting();

  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  const std::vector<ZImgInfo> infos = ZImg::readImgInfos(filename, &subBlocks, FileFormat::OmeTiff);
  ASSERT_EQ(infos.size(), 2u);
  ASSERT_EQ(infos[0].width, 2u);
  ASSERT_EQ(infos[1].width, 2u);
  ASSERT_EQ(subBlocks.size(), 2u);

  const ZImg scene0(filename, ZImgRegion(), 0, 1, 1, 1, FileFormat::OmeTiff);
  const ZImg scene1(filename, ZImgRegion(), 1, 1, 1, 1, FileFormat::OmeTiff);
  EXPECT_EQ(*scene0.data<uint8_t>(0, 0), 10u);
  EXPECT_EQ(*scene0.data<uint8_t>(1, 1), 13u);
  EXPECT_EQ(*scene1.data<uint8_t>(0, 0), 20u);
  EXPECT_EQ(*scene1.data<uint8_t>(1, 1), 23u);
}

TEST(ZImgOmeTiff, SamplesPerPixelUsesFullChannelFirstCOffsets)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("samples-per-pixel.ome.tif"));

  ZTiffWriter writer;
  writer.startWriting(filename, Compression::NONE, -1, false);
  std::vector<ZImgMetatag> tags;
  tags.emplace_back("ImageDescription", omeXmlForSamplesPerPixel(), 270);
  writer.writeIFD(multiChannelImage(2, 2, {10, 20, 30}), 0, 0, -1, false, tags);
  writer.writeIFD(multiChannelImage(2, 2, {40, 50, 60}), 0, 0, -1, false);
  writer.finishWriting();

  const ZImg img(filename, ZImgRegion(), 0, 1, 1, 1, FileFormat::OmeTiff);
  ASSERT_EQ(img.numChannels(), 6u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 0), 10u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 1), 20u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 2), 30u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 3), 40u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 4), 50u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 5), 60u);
}

TEST(ZImgOmeTiff, MetadataOnlyImagesAreSkippedAsScenes)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("metadata-only.ome.tif"));

  ZTiffWriter writer;
  writer.startWriting(filename, Compression::NONE, -1, false);
  std::vector<ZImgMetatag> tags;
  tags.emplace_back("ImageDescription", omeXmlForMetadataOnlyPlusPixelSeries(), 270);
  writeSingleIfd(writer, singleChannelImage(2, 2, 50), tags);
  writer.finishWriting();

  const std::vector<ZImgInfo> infos = ZImg::readImgInfos(filename, nullptr, FileFormat::OmeTiff);
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].width, 2u);
  EXPECT_EQ(infos[0].height, 2u);

  const ZImg img(filename, ZImgRegion(), 0, 1, 1, 1, FileFormat::OmeTiff);
  EXPECT_EQ(*img.data<uint8_t>(0, 0), 50u);
  EXPECT_EQ(*img.data<uint8_t>(1, 1), 53u);
}

TEST(ZImgOmeTiff, DefaultTiffDataMapsFiveDPlanes)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("default.ome.tif"));

  ZTiffWriter writer;
  writer.startWriting(filename, Compression::NONE, -1, false);
  std::vector<ZImgMetatag> tags;
  tags.emplace_back("ImageDescription", omeXmlForDefaultTiffData(), 270);
  writeSingleIfd(writer, singleChannelImage(2, 2, 10), tags);
  writeSingleIfd(writer, singleChannelImage(2, 2, 20));
  writeSingleIfd(writer, singleChannelImage(2, 2, 30));
  writeSingleIfd(writer, singleChannelImage(2, 2, 40));
  writer.finishWriting();

  const ZImg img(filename, ZImgRegion(), 0, 1, 1, 1, FileFormat::OmeTiff);
  ASSERT_EQ(img.depth(), 2u);
  ASSERT_EQ(img.numChannels(), 2u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 0), 10u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 1, 0), 20u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 1), 30u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 1, 1), 40u);
}

TEST(ZImgOmeTiff, ExplicitTiffDataCanReverseTime)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("reverse-time.ome.tif"));

  ZTiffWriter writer;
  writer.startWriting(filename, Compression::NONE, -1, false);
  std::vector<ZImgMetatag> tags;
  tags.emplace_back("ImageDescription", omeXmlForReverseTime(), 270);
  writeSingleIfd(writer, singleChannelImage(2, 2, 20), tags);
  writeSingleIfd(writer, singleChannelImage(2, 2, 10));
  writer.finishWriting();

  const ZImg img(filename, ZImgRegion(), 0, 1, 1, 1, FileFormat::OmeTiff);
  ASSERT_EQ(img.numTimes(), 2u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 0, 0), 10u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 0, 1), 20u);
}

TEST(ZImgOmeTiff, OneBasedIfdCompatibilityOnlyWhenZeroBasedMappingIsInvalid)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("one-based-ifd.ome.tif"));

  ZTiffWriter writer;
  writer.startWriting(filename, Compression::NONE, -1, false);
  std::vector<ZImgMetatag> tags;
  tags.emplace_back("ImageDescription", omeXmlForOneBasedIfdCompatibility(), 270);
  writeSingleIfd(writer, singleChannelImage(2, 2, 31), tags);
  writer.finishWriting();

  const ZImg img(filename, ZImgRegion(), 0, 1, 1, 1, FileFormat::OmeTiff);
  EXPECT_EQ(*img.data<uint8_t>(0, 0), 31u);
  EXPECT_EQ(*img.data<uint8_t>(1, 1), 34u);
}

TEST(ZImgOmeTiff, MissingAndDuplicatePlanesFailClearly)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString missingFilename = QDir(tmp.path()).filePath(QStringLiteral("missing.ome.tif"));
  {
    ZTiffWriter writer;
    writer.startWriting(missingFilename, Compression::NONE, -1, false);
    std::vector<ZImgMetatag> tags;
    tags.emplace_back("ImageDescription", omeXmlForMissingPlane(), 270);
    writeSingleIfd(writer, singleChannelImage(2, 2, 10), tags);
    writer.finishWriting();
  }
  EXPECT_THROW((ZImg(missingFilename, ZImgRegion(), 0, 1, 1, 1, FileFormat::OmeTiff)), ZException);

  const QString duplicateFilename = QDir(tmp.path()).filePath(QStringLiteral("duplicate.ome.tif"));
  {
    ZTiffWriter writer;
    writer.startWriting(duplicateFilename, Compression::NONE, -1, false);
    std::vector<ZImgMetatag> tags;
    tags.emplace_back("ImageDescription", omeXmlForDuplicatePlane(), 270);
    writeSingleIfd(writer, singleChannelImage(2, 2, 10), tags);
    writeSingleIfd(writer, singleChannelImage(2, 2, 20));
    writer.finishWriting();
  }
  EXPECT_THROW((ZImg::readImgInfos(duplicateFilename, nullptr, FileFormat::OmeTiff)), ZException);
}

TEST(ZImgOmeTiff, MultiFileAndBinaryOnlyResolveRelativePixelFiles)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  QDir dir(tmp.path());

  const QString metadataFilename = dir.filePath(QStringLiteral("metadata.ome.tif"));
  const QString pixelsFilename = dir.filePath(QStringLiteral("pixels.ome.tif"));
  {
    ZTiffWriter writer;
    writer.startWriting(pixelsFilename, Compression::NONE, -1, false);
    writeSingleIfd(writer, singleChannelImage(2, 2, 70));
    writer.finishWriting();
  }
  {
    ZTiffWriter writer;
    writer.startWriting(metadataFilename, Compression::NONE, -1, false);
    std::vector<ZImgMetatag> tags;
    tags.emplace_back("ImageDescription", omeXmlForMultiFile(QStringLiteral("pixels.ome.tif")), 270);
    writeSingleIfd(writer, singleChannelImage(1, 1, 1), tags);
    writer.finishWriting();
  }
  const ZImg multiFileRead(metadataFilename, ZImgRegion(), 0, 1, 1, 1, FileFormat::OmeTiff);
  EXPECT_EQ(*multiFileRead.data<uint8_t>(0, 0), 70u);
  EXPECT_EQ(*multiFileRead.data<uint8_t>(1, 1), 73u);

  const QString companionFilename = dir.filePath(QStringLiteral("companion.ome"));
  writeTextFile(companionFilename, omeXmlForMultiFile(QStringLiteral("binary.ome.tif")));
  const QString binaryFilename = dir.filePath(QStringLiteral("binary.ome.tif"));
  {
    ZTiffWriter writer;
    writer.startWriting(binaryFilename, Compression::NONE, -1, false);
    std::vector<ZImgMetatag> tags;
    tags.emplace_back("ImageDescription", binaryOnlyXml(QStringLiteral("companion.ome")), 270);
    writeSingleIfd(writer, singleChannelImage(2, 2, 90), tags);
    writer.finishWriting();
  }
  const ZImg binaryOnlyRead(binaryFilename, ZImgRegion(), 0, 1, 1, 1, FileFormat::OmeTiff);
  EXPECT_EQ(*binaryOnlyRead.data<uint8_t>(0, 0), 90u);
  EXPECT_EQ(*binaryOnlyRead.data<uint8_t>(1, 1), 93u);
}

TEST(ZImgOmeTiff, SubIfdsCreateReadablePyramidSubBlocks)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("pyramid.ome.tif"));

  ZImg base = singleChannelImage(4, 4, 1);
  ZImg reduced = singleChannelImage(2, 2, 101);
  base.thumbnailRef().attachToPlane(reduced, 0, 0);

  ZTiffWriter writer;
  writer.startWriting(filename, Compression::NONE, -1, false);
  std::vector<ZImgMetatag> tags;
  tags.emplace_back("ImageDescription", omeXmlForPyramid(), 270);
  writer.writeIFD(base, 0, 0, 0, true, tags);
  writer.finishWriting();

  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  const std::vector<ZImgInfo> infos = ZImg::readImgInfos(filename, &subBlocks, FileFormat::OmeTiff);
  ASSERT_EQ(infos.size(), 1u);
  ASSERT_EQ(infos[0].width, 4u);
  ASSERT_EQ(infos[0].height, 4u);
  ASSERT_EQ(subBlocks.size(), 1u);

  const ZImgSubBlock* pyramidBlock = nullptr;
  for (const std::shared_ptr<ZImgSubBlock>& block : subBlocks[0]) {
    if (block->xRatio == 2 && block->yRatio == 2) {
      pyramidBlock = block.get();
      break;
    }
  }
  ASSERT_NE(pyramidBlock, nullptr);

  const ZImgInfo reducedInfo = pyramidBlock->readInfo();
  EXPECT_EQ(reducedInfo.width, 2u);
  EXPECT_EQ(reducedInfo.height, 2u);
  EXPECT_EQ(reducedInfo.numChannels, 1u);

  const std::shared_ptr<ZImg> reducedRead = pyramidBlock->read();
  ASSERT_NE(reducedRead, nullptr);
  ASSERT_EQ(reducedRead->width(), 2u);
  ASSERT_EQ(reducedRead->height(), 2u);
  EXPECT_EQ(*reducedRead->data<uint8_t>(0, 0), 101u);
  EXPECT_EQ(*reducedRead->data<uint8_t>(1, 1), 104u);

  const ZImg baseRead(filename, ZImgRegion(1, 3, 1, 3), 0, 1, 1, 1, FileFormat::OmeTiff);
  ASSERT_EQ(baseRead.width(), 2u);
  ASSERT_EQ(baseRead.height(), 2u);
  EXPECT_EQ(*baseRead.data<uint8_t>(0, 0), 6u);
  EXPECT_EQ(*baseRead.data<uint8_t>(1, 1), 11u);
}

TEST(ZImgOmeTiff, WriterCreatesTiledPyramidalOmeTiff)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("written.ome.tif"));

  ZImg img = singleChannelImage(1024, 768, 1);
  img.save(filename, FileFormat::OmeTiff, ZImgWriteParameters{.compression = Compression::NONE});

  ZTiff tiff;
  tiff.load(filename);
  ASSERT_FALSE(tiff.ifds().empty());
  EXPECT_TRUE(tiff.ifds()[0].isTiledImage());
  ASSERT_FALSE(tiff.ifds()[0].subIFDs().empty());
  EXPECT_TRUE(tiff.ifds()[0].subIFDs()[0].isReducedResolutionImage());
  EXPECT_TRUE(tiff.ifds()[0].subIFDs()[0].isTiledImage());

  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  const std::vector<ZImgInfo> infos = ZImg::readImgInfos(filename, &subBlocks, FileFormat::OmeTiff);
  ASSERT_EQ(infos.size(), 1u);
  ASSERT_EQ(subBlocks.size(), 1u);
  EXPECT_TRUE(std::any_of(subBlocks[0].begin(), subBlocks[0].end(), [](const std::shared_ptr<ZImgSubBlock>& block) {
    return block->xRatio == 2 && block->yRatio == 2 && block->zRatio == 1;
  }));

  const ZImg region(filename, ZImgRegion(510, 516, 382, 388), 0, 1, 1, 1, FileFormat::OmeTiff);
  ASSERT_EQ(region.width(), 6u);
  ASSERT_EQ(region.height(), 6u);
  EXPECT_EQ(*region.data<uint8_t>(0, 0), *img.data<uint8_t>(510, 382));
  EXPECT_EQ(*region.data<uint8_t>(5, 5), *img.data<uint8_t>(515, 387));
}

TEST(ZImgOmeTiff, BigTiffExtensionWritesAndReadsNatively)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("small.ome.tf8"));

  ZImg img = singleChannelImage(3, 2, 11);
  img.save(filename, FileFormat::OmeTiff, ZImgWriteParameters{.compression = Compression::NONE});
  const ZImg read(filename, ZImgRegion(), 0, 1, 1, 1, FileFormat::OmeTiff);
  ASSERT_EQ(read.width(), 3u);
  ASSERT_EQ(read.height(), 2u);
  EXPECT_EQ(*read.data<uint8_t>(0, 0), 11u);
  EXPECT_EQ(*read.data<uint8_t>(2, 1), 16u);
}

} // namespace
} // namespace nim
