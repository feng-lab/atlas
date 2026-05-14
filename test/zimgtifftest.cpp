#include "zimg.h"
#include "ztiff.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QTemporaryDir>

#include <algorithm>

namespace nim {
namespace {

constexpr size_t kDefaultTileSize = 512;

uint8_t pixelValue(size_t x, size_t y, uint8_t seed)
{
  return static_cast<uint8_t>((seed + x * 3 + y * 5) % 251);
}

ZImg singleChannelImage(size_t width, size_t height, uint8_t seed)
{
  ZImg img(ZImgInfo(width, height, 1, 1, 1, 1, VoxelFormat::Unsigned));
  for (size_t y = 0; y < height; ++y) {
    for (size_t x = 0; x < width; ++x) {
      *img.data<uint8_t>(x, y) = pixelValue(x, y, seed);
    }
  }
  return img;
}

void expectRegionMatchesSource(const ZImg& region, const ZImgRegion& sourceRegion, uint8_t seed)
{
  const size_t xStart = static_cast<size_t>(sourceRegion.start.x);
  const size_t yStart = static_cast<size_t>(sourceRegion.start.y);
  for (size_t y = 0; y < region.height(); ++y) {
    for (size_t x = 0; x < region.width(); ++x) {
      EXPECT_EQ(pixelValue(xStart + x, yStart + y, seed), *region.data<uint8_t>(x, y));
    }
  }
}

bool hasSubBlock(const std::vector<std::shared_ptr<ZImgSubBlock>>& subBlocks,
                 index_t x,
                 index_t y,
                 index_t width,
                 index_t height)
{
  return std::ranges::any_of(subBlocks, [&](const std::shared_ptr<ZImgSubBlock>& block) {
    return block->x == x && block->y == y && block->width == width && block->height == height;
  });
}

TEST(ZImgTiff, ReadInfoUsesNativeTileSizeForTiledTiff)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("manual-tiled.tif"));

  constexpr uint8_t seed = 7;
  ZTiffWriter writer;
  writer.startWriting(filename, Compression::NONE, -1, false);
  writer.writeIFD(singleChannelImage(640, 384, seed), 0, 0, 0, false, {}, nullptr, 256, 128);
  writer.finishWriting();

  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  const std::vector<ZImgInfo> infos = ZImg::readImgInfos(filename, &subBlocks, FileFormat::Tiff);
  ASSERT_EQ(size_t{1}, infos.size());
  ASSERT_EQ(size_t{1}, subBlocks.size());
  ASSERT_EQ(size_t{9}, subBlocks[0].size());
  EXPECT_TRUE(hasSubBlock(subBlocks[0], 0, 0, 256, 128));
  EXPECT_TRUE(hasSubBlock(subBlocks[0], 512, 256, 128, 128));

  const ZImgRegion region(250, 266, 126, 134);
  const ZImg readRegion(filename, region, 0, 1, 1, 1, FileFormat::Tiff);
  ASSERT_EQ(size_t{16}, readRegion.width());
  ASSERT_EQ(size_t{8}, readRegion.height());
  expectRegionMatchesSource(readRegion, region, seed);
}

TEST(ZImgTiff, NonTiledTiffUsesLogicalTilesAndRegionReads)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("stripped.tif"));

  constexpr uint8_t seed = 11;
  ZTiffWriter writer;
  writer.startWriting(filename, Compression::NONE, -1, false);
  writer.writeIFD(singleChannelImage(1024, 768, seed), 0, 0, 0, false);
  writer.finishWriting();

  ZTiff tiff;
  tiff.load(filename);
  ASSERT_TRUE(tiff.isValid());
  ASSERT_FALSE(tiff.ifds().empty());
  EXPECT_FALSE(tiff.ifds()[0].isTiledImage());

  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  const std::vector<ZImgInfo> infos = ZImg::readImgInfos(filename, &subBlocks, FileFormat::Tiff);
  ASSERT_EQ(size_t{1}, infos.size());
  ASSERT_EQ(size_t{1}, subBlocks.size());
  ASSERT_EQ(size_t{4}, subBlocks[0].size());
  EXPECT_TRUE(hasSubBlock(subBlocks[0], 0, 0, 512, 512));
  EXPECT_TRUE(hasSubBlock(subBlocks[0], 512, 512, 512, 256));

  const ZImgRegion region(510, 518, 382, 390);
  const ZImg readRegion(filename, region, 0, 1, 1, 1, FileFormat::Tiff);
  ASSERT_EQ(size_t{8}, readRegion.width());
  ASSERT_EQ(size_t{8}, readRegion.height());
  expectRegionMatchesSource(readRegion, region, seed);
}

TEST(ZImgTiff, WriterCreatesTiledTiffAndRegionReads)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("atlas-tiled.tif"));

  constexpr uint8_t seed = 19;
  const ZImg source = singleChannelImage(1024, 768, seed);
  source.save(filename, FileFormat::Tiff, ZImgWriteParameters{.compression = Compression::NONE});

  ZTiff tiff;
  tiff.load(filename);
  ASSERT_TRUE(tiff.isValid());
  ASSERT_FALSE(tiff.ifds().empty());
  ASSERT_TRUE(tiff.ifds()[0].isTiledImage());
  EXPECT_EQ(kDefaultTileSize, tiff.ifds()[0].tileWidth());
  EXPECT_EQ(kDefaultTileSize, tiff.ifds()[0].tileHeight());

  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  const std::vector<ZImgInfo> infos = ZImg::readImgInfos(filename, &subBlocks, FileFormat::Tiff);
  ASSERT_EQ(size_t{1}, infos.size());
  ASSERT_EQ(size_t{1}, subBlocks.size());
  ASSERT_EQ(size_t{4}, subBlocks[0].size());
  EXPECT_TRUE(hasSubBlock(subBlocks[0], 0, 0, 512, 512));
  EXPECT_TRUE(hasSubBlock(subBlocks[0], 512, 512, 512, 256));

  const ZImgRegion region(510, 518, 382, 390);
  const ZImg readRegion(filename, region, 0, 1, 1, 1, FileFormat::Tiff);
  ASSERT_EQ(size_t{8}, readRegion.width());
  ASSERT_EQ(size_t{8}, readRegion.height());
  expectRegionMatchesSource(readRegion, region, seed);
}

} // namespace
} // namespace nim
