#include "ztest.h"

#include "zbioformatsbridgeclient.h"
#include "zimgbioformats.h"
#include "zimginit.h"
#include "zimgio.h"

#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>

namespace nim {
namespace {

std::optional<std::string> bioFormatsRuntimeSkipReason()
{
  static bool checked = false;
  static std::optional<std::string> skipReason;

  if (!checked) {
    checked = true;

    const QFileInfo testDataDir(QStringLiteral(ATLAS_TEST_DATA_DIR));
    const QDir repoRoot = testDataDir.dir();
    const QDir jarsDir(repoRoot.filePath("src/3rdparty/build/jars"));
    if (!jarsDir.exists("bioformats_package.jar")) {
      skipReason = fmt::format("missing {}", jarsDir.filePath("bioformats_package.jar"));
      return skipReason;
    }
    if (!jarsDir.exists("atlas-bioformats-bridge.jar")) {
      skipReason = fmt::format("missing {}", jarsDir.filePath("atlas-bioformats-bridge.jar"));
      return skipReason;
    }

    try {
      ZImgInit::instance("", "", jarsDir.absolutePath(), false);
      if (!ZImgBioFormats().supportRead()) {
        skipReason = "Bio-Formats runtime support is not available";
      }
    }
    catch (const std::exception& e) {
      skipReason = fmt::format("Bio-Formats runtime initialization failed: {}", e.what());
    }
  }

  return skipReason;
}

QString createFakeReaderFile(QTemporaryDir& dir, const QString& basename)
{
  const QString filename = basename.endsWith(QStringLiteral(".fake")) ? basename : basename + QStringLiteral(".fake");
  const QString path = dir.filePath(filename);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    throw std::runtime_error(
      fmt::format("failed to create FakeReader file '{}': {}", path.toStdString(), file.errorString().toStdString()));
  }
  return path;
}

size_t nonNegativeIndex(index_t value)
{
  CHECK(value >= 0);
  return static_cast<size_t>(value);
}

void expectSameRegionPixels(const ZImg& full, const ZImg& region, const ZImgRegion& sourceRegion)
{
  ASSERT_EQ(sourceRegion.end.x - sourceRegion.start.x, region.sWidth());
  ASSERT_EQ(sourceRegion.end.y - sourceRegion.start.y, region.sHeight());
  ASSERT_EQ(sourceRegion.end.z - sourceRegion.start.z, region.sDepth());
  ASSERT_EQ(sourceRegion.end.c - sourceRegion.start.c, region.sNumChannels());
  ASSERT_EQ(sourceRegion.end.t - sourceRegion.start.t, region.sNumTimes());

  for (index_t t = 0; t < region.sNumTimes(); ++t) {
    for (index_t c = 0; c < region.sNumChannels(); ++c) {
      for (index_t z = 0; z < region.sDepth(); ++z) {
        for (index_t y = 0; y < region.sHeight(); ++y) {
          for (index_t x = 0; x < region.sWidth(); ++x) {
            EXPECT_EQ(*full.data<uint16_t>(nonNegativeIndex(sourceRegion.start.x + x),
                                           nonNegativeIndex(sourceRegion.start.y + y),
                                           nonNegativeIndex(sourceRegion.start.z + z),
                                           nonNegativeIndex(sourceRegion.start.c + c),
                                           nonNegativeIndex(sourceRegion.start.t + t)),
                      *region.data<uint16_t>(nonNegativeIndex(x),
                                             nonNegativeIndex(y),
                                             nonNegativeIndex(z),
                                             nonNegativeIndex(c),
                                             nonNegativeIndex(t)))
              << "x=" << x << ", y=" << y << ", z=" << z << ", c=" << c << ", t=" << t;
          }
        }
      }
    }
  }
}

std::vector<QString> corpusFilesFromManifest(const QString& rootPath)
{
  const QString manifestPath = QDir(rootPath).filePath(QStringLiteral("manifest.json"));
  QFile manifestFile(manifestPath);
  if (!manifestFile.exists()) {
    return {};
  }
  if (!manifestFile.open(QIODevice::ReadOnly)) {
    throw std::runtime_error(fmt::format("failed to open {}", manifestPath.toStdString()));
  }

  QJsonParseError error;
  const QJsonDocument doc = QJsonDocument::fromJson(manifestFile.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject()) {
    throw std::runtime_error(
      fmt::format("failed to parse {}: {}", manifestPath.toStdString(), error.errorString().toStdString()));
  }

  std::vector<QString> files;
  const QJsonArray samples = doc.object().value(QStringLiteral("samples")).toArray();
  files.reserve(static_cast<size_t>(samples.size()));
  for (const QJsonValue& value : samples) {
    const QJsonObject object = value.toObject();
    const QString relativePath = object.value(QStringLiteral("relative_path")).toString();
    if (!relativePath.isEmpty()) {
      files.push_back(QDir(rootPath).filePath(relativePath));
    }
  }
  return files;
}

std::vector<QString> corpusFilesByWalking(const QString& rootPath)
{
  std::vector<QString> files;
  QDirIterator it(rootPath, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString path = it.next();
    if (QFileInfo(path).fileName() == QStringLiteral("manifest.json")) {
      continue;
    }
    files.push_back(path);
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::vector<QString> publicCorpusFiles()
{
  const QByteArray root = qgetenv("ATLAS_BIOFORMATS_BREADTH_DIR");
  if (root.isEmpty()) {
    return {};
  }
  const QString rootPath = QString::fromUtf8(root);
  if (!QDir(rootPath).exists()) {
    throw std::runtime_error(fmt::format("ATLAS_BIOFORMATS_BREADTH_DIR does not exist: {}", rootPath.toStdString()));
  }
  std::vector<QString> files = corpusFilesFromManifest(rootPath);
  if (files.empty()) {
    files = corpusFilesByWalking(rootPath);
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::vector<ZImgRegion> smokeRegions(const ZImgInfo& info)
{
  static constexpr index_t kSmokeTileSize = 16;
  std::vector<ZImgRegion> regions;
  auto addRegion = [&](index_t x0, index_t y0, index_t z0, index_t t0, index_t width, index_t height) {
    regions.emplace_back(ZVoxelCoordinate(x0, y0, z0, 0, t0),
                         ZVoxelCoordinate(std::min<index_t>(info.sWidth(), x0 + width),
                                          std::min<index_t>(info.sHeight(), y0 + height),
                                          z0 + 1,
                                          info.sNumChannels(),
                                          t0 + 1));
  };

  addRegion(0,
            0,
            0,
            0,
            std::min<index_t>(kSmokeTileSize, info.sWidth()),
            std::min<index_t>(kSmokeTileSize, info.sHeight()));
  if (info.sWidth() > kSmokeTileSize || info.sHeight() > kSmokeTileSize) {
    addRegion(std::max<index_t>(0, info.sWidth() / 2 - kSmokeTileSize / 2),
              std::max<index_t>(0, info.sHeight() / 2 - kSmokeTileSize / 2),
              0,
              0,
              std::min<index_t>(kSmokeTileSize, info.sWidth()),
              std::min<index_t>(kSmokeTileSize, info.sHeight()));
  }
  if (info.sDepth() > 1 || info.sNumTimes() > 1) {
    addRegion(0, 0, info.sDepth() - 1, info.sNumTimes() - 1, 1, 1);
  }
  return regions;
}

} // namespace

TEST(ZBioFormatsTest, FormatExtensionListDoesNotRequireAnOpenFile)
{
  if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
    GTEST_SKIP() << *reason;
  }

  const QStringList extensions = ZImgBioFormats().extensions();
  ASSERT_FALSE(extensions.empty());
  EXPECT_TRUE(extensions.contains(QStringLiteral("fake"), Qt::CaseInsensitive));
}

TEST(ZBioFormatsTest, FakeReaderMetadataCreatesDeterministicInfoAndSubBlocks)
{
  if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
    GTEST_SKIP() << *reason;
  }

  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path =
    createFakeReaderFile(dir, QStringLiteral("metadata&sizeX=7&sizeY=5&sizeZ=3&sizeC=2&sizeT=4&pixelType=uint8"));

  std::vector<ZImgInfo> infos;
  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  ZImgIO::instance().readInfos(path, infos, &subBlocks, FileFormat::BioFormats);

  ASSERT_EQ(1u, infos.size());
  const ZImgInfo& info = infos.front();
  EXPECT_EQ(7u, info.width);
  EXPECT_EQ(5u, info.height);
  EXPECT_EQ(3u, info.depth);
  EXPECT_EQ(2u, info.numChannels);
  EXPECT_EQ(4u, info.numTimes);
  EXPECT_TRUE(info.isType<uint8_t>());

  ASSERT_EQ(infos.size(), subBlocks.size());
  ASSERT_FALSE(subBlocks.front().empty());
  const std::shared_ptr<ZImgSubBlock>& firstBlock = subBlocks.front().front();
  ASSERT_NE(nullptr, firstBlock);
  EXPECT_EQ(0, firstBlock->x);
  EXPECT_EQ(0, firstBlock->y);
  EXPECT_EQ(0, firstBlock->z);
  EXPECT_EQ(0, firstBlock->t);
  EXPECT_EQ(7, firstBlock->width);
  EXPECT_EQ(5, firstBlock->height);
  EXPECT_EQ(1, firstBlock->depth);
}

TEST(ZBioFormatsTest, IndexedFalseColorDataStaysSingleChannel)
{
  if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
    GTEST_SKIP() << *reason;
  }

  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = createFakeReaderFile(
    dir,
    QStringLiteral(
      "indexed&sizeX=9&sizeY=7&sizeZ=1&sizeC=1&sizeT=1&pixelType=uint8&indexed=true&falseColor=true&lutLength=256"));

  std::vector<ZImgInfo> infos;
  ZImgIO::instance().readInfos(path, infos, nullptr, FileFormat::BioFormats);

  ASSERT_EQ(1u, infos.size());
  EXPECT_EQ(1u, infos.front().numChannels);
  EXPECT_TRUE(infos.front().isType<uint8_t>());

  const ZImg img(path, ZImgRegion(), 0, 1, 1, 1, FileFormat::BioFormats);
  EXPECT_EQ(1u, img.numChannels());
  EXPECT_TRUE(img.info().isType<uint8_t>());
}

TEST(ZBioFormatsTest, ChannelColorMetadataIsPreserved)
{
  if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
    GTEST_SKIP() << *reason;
  }

  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = createFakeReaderFile(
    dir,
    QStringLiteral("color&sizeX=9&sizeY=7&sizeZ=1&sizeC=1&sizeT=1&pixelType=uint8&color=0x00ff00ff"));

  std::vector<ZImgInfo> infos;
  ZImgIO::instance().readInfos(path, infos, nullptr, FileFormat::BioFormats);

  ASSERT_EQ(1u, infos.size());
  ASSERT_EQ(1u, infos.front().channelColors.size());
  EXPECT_EQ(0, infos.front().channelColors[0].r);
  EXPECT_EQ(255, infos.front().channelColors[0].g);
  EXPECT_EQ(0, infos.front().channelColors[0].b);
  EXPECT_EQ(255, infos.front().channelColors[0].a);
}

TEST(ZBioFormatsTest, FakeReaderRegionMatchesTheSameCoordinatesFromFullRead)
{
  if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
    GTEST_SKIP() << *reason;
  }

  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path =
    createFakeReaderFile(dir, QStringLiteral("region&sizeX=13&sizeY=11&sizeZ=3&sizeC=2&sizeT=2&pixelType=uint16"));

  ZImg full(path, ZImgRegion(), 0, 1, 1, 1, FileFormat::BioFormats);
  ASSERT_TRUE(full.info().isType<uint16_t>());
  ASSERT_EQ(13u, full.width());
  ASSERT_EQ(11u, full.height());
  ASSERT_EQ(3u, full.depth());
  ASSERT_EQ(2u, full.numChannels());
  ASSERT_EQ(2u, full.numTimes());

  const ZImgRegion sourceRegion(2, 9, 3, 8, 1, 3, 0, 2, 1, 2);
  ZImg region;
  ZImgIO::instance().readImg(path, region, sourceRegion, 0, 1, 1, 1, FileFormat::BioFormats);

  ASSERT_TRUE(region.info().isType<uint16_t>());
  expectSameRegionPixels(full, region, sourceRegion);
}

TEST(ZBioFormatsTest, FakeReaderReadReassemblesMultiplePixelChunks)
{
  if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
    GTEST_SKIP() << *reason;
  }

  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path =
    createFakeReaderFile(dir, QStringLiteral("chunks&sizeX=4097&sizeY=2048&sizeZ=1&sizeC=1&sizeT=1&pixelType=uint8"));

  ZImg img(path, ZImgRegion(), 0, 1, 1, 1, FileFormat::BioFormats);

  ASSERT_TRUE(img.info().isType<uint8_t>());
  EXPECT_EQ(4097u, img.width());
  EXPECT_EQ(2048u, img.height());
  EXPECT_EQ(4097u * 2048u, img.byteNumber());
  EXPECT_GT(img.byteNumber(), 8u * 1024u * 1024u);

  const auto boundary = *img.data<uint8_t>(4096, 2047);
  const auto reread = ZImg(path, ZImgRegion(4096, 4097, 2047, 2048), 0, 1, 1, 1, FileFormat::BioFormats);
  ASSERT_TRUE(reread.info().isType<uint8_t>());
  ASSERT_EQ(1u, reread.width());
  ASSERT_EQ(1u, reread.height());
  EXPECT_EQ(boundary, *reread.data<uint8_t>(0, 0));
}

TEST(ZBioFormatsTest, FakeReaderResolutionMetadataCreatesPyramidSubBlocks)
{
  if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
    GTEST_SKIP() << *reason;
  }

  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = createFakeReaderFile(
    dir,
    QStringLiteral(
      "pyramid&sizeX=64&sizeY=48&sizeZ=1&sizeC=1&sizeT=1&pixelType=uint8&resolutions=3&resolutionScale=2"));

  const ZBioFormatsDatasetInfo& dataset = ZBioFormatsBridgeClient::instance().openDataset(path);
  ASSERT_EQ(1u, dataset.series.size());
  ASSERT_GE(dataset.series.front().resolutions.size(), 3u);
  EXPECT_EQ(64u, dataset.series.front().resolutions[0].sizeX);
  EXPECT_EQ(48u, dataset.series.front().resolutions[0].sizeY);
  EXPECT_EQ(32u, dataset.series.front().resolutions[1].sizeX);
  EXPECT_EQ(24u, dataset.series.front().resolutions[1].sizeY);
  EXPECT_EQ(16u, dataset.series.front().resolutions[2].sizeX);
  EXPECT_EQ(12u, dataset.series.front().resolutions[2].sizeY);

  std::vector<ZImgInfo> infos;
  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  ZImgIO::instance().readInfos(path, infos, &subBlocks, FileFormat::BioFormats);
  ASSERT_EQ(1u, subBlocks.size());
  EXPECT_TRUE(std::ranges::any_of(subBlocks.front(), [](const std::shared_ptr<ZImgSubBlock>& block) {
    return block && block->xRatio > 1 && block->yRatio > 1;
  }));
}

TEST(ZBioFormatsTest, FakeReaderThumbnailIsReadWhenAvailable)
{
  if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
    GTEST_SKIP() << *reason;
  }

  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = createFakeReaderFile(
    dir,
    QStringLiteral("thumbnail&sizeX=64&sizeY=48&sizeZ=1&sizeC=1&sizeT=1&pixelType=uint8&thumbSizeX=8&thumbSizeY=6"));

  ZImgThumbernail thumbnail = ZImg::readImgThumbnail(path, ZImgRegion(), 0, FileFormat::BioFormats);
  ASSERT_TRUE(thumbnail.hasPlaneAttachment(0, 0));
  const std::vector<ZImg>& thumbnails = thumbnail.planeAttachments(0, 0);
  ASSERT_FALSE(thumbnails.empty());
  EXPECT_EQ(8u, thumbnails.front().width());
  EXPECT_EQ(6u, thumbnails.front().height());
  EXPECT_EQ(1u, thumbnails.front().numChannels());
  EXPECT_TRUE(thumbnails.front().info().isType<uint8_t>());
}

TEST(ZBioFormatsTest, PublicCorpusReadsMetadataAndSmallRegions)
{
  if (const auto reason = bioFormatsRuntimeSkipReason(); reason.has_value()) {
    GTEST_SKIP() << *reason;
  }

  const std::vector<QString> files = publicCorpusFiles();
  if (files.empty()) {
    GTEST_SKIP() << "set ATLAS_BIOFORMATS_BREADTH_DIR to run public Bio-Formats corpus smoke tests";
  }
  const bool strict = !qgetenv("ATLAS_BIOFORMATS_BREADTH_STRICT").isEmpty();

  size_t openedFiles = 0;
  size_t unsupportedFiles = 0;
  size_t openFailures = 0;
  size_t regionFailures = 0;
  size_t thumbnailFailures = 0;
  for (const QString& path : files) {
    SCOPED_TRACE(path.toStdString());
    ZImgBioFormats reader;
    if (!reader.canRead(path)) {
      ++unsupportedFiles;
      continue;
    }

    std::vector<ZImgInfo> infos;
    try {
      ZImgIO::instance().readInfos(path, infos, nullptr, FileFormat::BioFormats);
    }
    catch (const std::exception& e) {
      ++openFailures;
      ZBioFormatsBridgeClient::instance().closeDataset(path);
      if (strict) {
        FAIL() << e.what();
      }
      continue;
    }
    ASSERT_FALSE(infos.empty());
    ++openedFiles;

    for (size_t scene = 0; scene < infos.size(); ++scene) {
      SCOPED_TRACE(fmt::format("scene {}", scene));
      for (const ZImgRegion& region : smokeRegions(infos[scene])) {
        ZImg img;
        try {
          ZImgIO::instance().readImg(path, img, region, scene, 1, 1, 1, FileFormat::BioFormats);
        }
        catch (const std::exception& e) {
          ++regionFailures;
          if (strict) {
            FAIL() << e.what();
          }
          continue;
        }
        EXPECT_FALSE(img.isEmpty());
      }
      ZImgThumbernail thumbnail;
      try {
        ZImgIO::instance().readThumbnail(path,
                                         thumbnail,
                                         ZImgRegion(0, -1, 0, -1, 0, 1, 0, -1, 0, 1),
                                         scene,
                                         FileFormat::BioFormats);
      }
      catch (const std::exception& e) {
        ++thumbnailFailures;
        if (strict) {
          FAIL() << e.what();
        }
      }
    }
    ZBioFormatsBridgeClient::instance().closeDataset(path);
  }

  RecordProperty("files", static_cast<int>(files.size()));
  RecordProperty("opened_files", static_cast<int>(openedFiles));
  RecordProperty("unsupported_or_companion_files", static_cast<int>(unsupportedFiles));
  RecordProperty("open_failures", static_cast<int>(openFailures));
  RecordProperty("region_failures", static_cast<int>(regionFailures));
  RecordProperty("thumbnail_failures", static_cast<int>(thumbnailFailures));
  EXPECT_GT(openedFiles, 0u);
  if (strict) {
    EXPECT_EQ(0u, openFailures);
    EXPECT_EQ(0u, regionFailures);
    EXPECT_EQ(0u, thumbnailFailures);
  }
}

} // namespace nim
