#include "zimg.h"
#include "zimgpack.h"

#include <gtest/gtest.h>

#include <QDir>
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

} // namespace
} // namespace nim
