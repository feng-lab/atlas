#include "zimg.h"
#include "zimgpack.h"
#include "ztiff.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace nim {
namespace {

ZImg singleChannelImage(size_t width, size_t height)
{
  ZImg img(ZImgInfo(width, height, 1, 1, 1, 1, VoxelFormat::Unsigned));
  for (size_t y = 0; y < height; ++y) {
    for (size_t x = 0; x < width; ++x) {
      *img.data<uint8_t>(x, y) = static_cast<uint8_t>((x + y * width) % 251);
    }
  }
  return img;
}

void writeTiffWithImageDescription(const QString& filename, const QString& imageDescription)
{
  QFile::remove(filename);

  ZImg img = singleChannelImage(4, 3);
  std::vector<ZImgMetatag> tags;
  tags.emplace_back("ImageDescription", imageDescription, 270);

  ZTiffWriter writer;
  writer.startWriting(filename, Compression::NONE, -1, false);
  writer.writeIFD(img, 0, 0, 0, false, tags);
}

TEST(ZImgOmeTiffPack, ReadRatioUsesNativeSubIfdOverview)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("pack-pyramid.ome.tif"));

  ZImg img = singleChannelImage(1024, 768);
  img.save(filename, FileFormat::OmeTiff, ZImgWriteParameters{.compression = Compression::NONE});

  ZImgPack pack(ZImgSource(filename, ZImgRegion(), 0, FileFormat::OmeTiff));
  ZImg overview = pack.resizedImg(512, 384, 1, 0);
  EXPECT_EQ(overview.width(), 512u);
  EXPECT_EQ(overview.height(), 384u);
}

TEST(ZImgOmeTiffPack, DetailedInfoLoadsMetadataOnDemand)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("pack-metadata.tif"));

  writeTiffWithImageDescription(filename, QStringLiteral("eager-metadata-value"));
  ZImgPack pack(ZImgSource(filename, ZImgRegion(), 0, FileFormat::Tiff));

  writeTiffWithImageDescription(filename, QStringLiteral("lazy-metadata-value"));
  const QString detailedInfo = pack.detailedInfo();

  EXPECT_TRUE(detailedInfo.contains(QStringLiteral("lazy-metadata-value"))) << detailedInfo;
  EXPECT_FALSE(detailedInfo.contains(QStringLiteral("eager-metadata-value"))) << detailedInfo;
}

TEST(ZImgReadOptions, PixelsOnlyReadSkipsTiffMetadata)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("pixels-only-metadata.tif"));

  writeTiffWithImageDescription(filename, QStringLiteral("metadata-that-should-not-be-attached"));

  ZImg completeImg(filename, ZImgRegion(), 0, 1, 1, 1, FileFormat::Tiff);
  EXPECT_FALSE(completeImg.metadata().isEmpty());
  EXPECT_TRUE(QString::fromStdString(completeImg.metadata().toString())
                .contains(QStringLiteral("metadata-that-should-not-be-attached")));

  ZImg pixelsOnlyImg = ZImg::readImgPixelsOnly(filename, ZImgRegion(), 0, 1, 1, 1, FileFormat::Tiff);
  EXPECT_TRUE(pixelsOnlyImg.info().isSameType(completeImg.info()));
  EXPECT_TRUE(pixelsOnlyImg.info().isSameSize(completeImg.info()));
  EXPECT_TRUE(pixelsOnlyImg.metadata().isEmpty());
  EXPECT_FALSE(pixelsOnlyImg.hasThumbnail());
}

TEST(ZImgReadOptions, SubBlockReadUsesPixelsOnlyPolicy)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());
  const QString filename = QDir(tmp.path()).filePath(QStringLiteral("subblock-pixels-only.tif"));

  writeTiffWithImageDescription(filename, QStringLiteral("subblock-metadata-that-should-not-be-attached"));

  std::vector<std::vector<std::shared_ptr<ZImgSubBlock>>> subBlocks;
  const std::vector<ZImgInfo> infos = ZImg::readImgInfos(filename, &subBlocks, FileFormat::Tiff);
  ASSERT_FALSE(infos.empty());
  ASSERT_FALSE(subBlocks.empty());
  ASSERT_FALSE(subBlocks[0].empty());

  const std::shared_ptr<ZImg> blockImg = subBlocks[0][0]->read();
  ASSERT_TRUE(blockImg);
  EXPECT_TRUE(blockImg->metadata().isEmpty());
  EXPECT_FALSE(blockImg->hasThumbnail());
}

} // namespace
} // namespace nim
