#include "zexception.h"
#include "zimg.h"
#include "zimgopenimageio.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <vector>

namespace nim {
namespace {

[[nodiscard]] QString writeRgbPngFixture(const QString& dirPath)
{
  ZImgInfo info(5, 4, 1, 3, 1, 1, VoxelFormat::Unsigned);
  ZImg img(info);

  for (size_t c = 0; c < img.numChannels(); ++c) {
    auto* channel = img.channelData<uint8_t>(c);
    for (size_t y = 0; y < img.height(); ++y) {
      for (size_t x = 0; x < img.width(); ++x) {
        channel[y * img.width() + x] = static_cast<uint8_t>(c * 50 + y * img.width() + x);
      }
    }
  }

  const QString path = QDir(dirPath).filePath(QStringLiteral("oiio_fixture.png"));
  img.save(path, FileFormat::Png);
  return path;
}

TEST(ZImgOpenImageIO, ReadsPngInfoAndRegion)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = writeRgbPngFixture(tmp.path());

  std::vector<ZImgInfo> infos = ZImg::readImgInfos(path, nullptr, FileFormat::OpenImageIO);
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].width, 5u);
  EXPECT_EQ(infos[0].height, 4u);
  EXPECT_EQ(infos[0].depth, 1u);
  EXPECT_EQ(infos[0].numChannels, 3u);
  EXPECT_EQ(infos[0].numTimes, 1u);
  EXPECT_EQ(infos[0].bytesPerVoxel, 1u);
  EXPECT_EQ(infos[0].voxelFormat, VoxelFormat::Unsigned);

  const ZImgRegion region(1, 4, 1, 3, 0, 1, 1, 3);
  const ZImg img(path, region, 0, 1, 1, 1, FileFormat::OpenImageIO);
  ASSERT_EQ(img.width(), 3u);
  ASSERT_EQ(img.height(), 2u);
  ASSERT_EQ(img.depth(), 1u);
  ASSERT_EQ(img.numChannels(), 2u);

  for (size_t c = 0; c < img.numChannels(); ++c) {
    const size_t sourceChannel = c + 1;
    for (size_t y = 0; y < img.height(); ++y) {
      for (size_t x = 0; x < img.width(); ++x) {
        const size_t sourceX = x + 1;
        const size_t sourceY = y + 1;
        const uint8_t expected = static_cast<uint8_t>(sourceChannel * 50 + sourceY * 5 + sourceX);
        EXPECT_EQ(*img.data<uint8_t>(x, y, 0, c), expected);
      }
    }
  }
}

TEST(ZImgOpenImageIO, RejectsInvalidScene)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = writeRgbPngFixture(tmp.path());
  EXPECT_THROW((ZImg(path, ZImgRegion(), 1, 1, 1, 1, FileFormat::OpenImageIO)), ZException);
}

TEST(ZImgOpenImageIO, ReadsPngFromMemory)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = writeRgbPngFixture(tmp.path());
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  QByteArray bytes = file.readAll();
  ASSERT_GT(bytes.size(), 0);

  ZImgInfo info;
  ZImgOpenImageIO::readMemInfo(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size(), info);
  ASSERT_EQ(info.width, 5u);
  ASSERT_EQ(info.height, 4u);
  ASSERT_EQ(info.numChannels, 3u);
  ASSERT_EQ(info.byteNumber(), 5u * 4u * 3u);

  std::vector<uint8_t> decoded(info.byteNumber());
  ZImgOpenImageIO::readMemImg(reinterpret_cast<const uint8_t*>(bytes.constData()),
                              bytes.size(),
                              decoded.data(),
                              decoded.size());
  ZImg img;
  img.wrapData(decoded.data(), info);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 0), 0u);
  EXPECT_EQ(*img.data<uint8_t>(0, 0, 0, 1), 50u);
  EXPECT_EQ(*img.data<uint8_t>(4, 3, 0, 2), 119u);
}

TEST(ZImgOpenImageIO, ReadPriorityOrderFollowsFileFormatEnum)
{
  EXPECT_LT(FileFormat::Png, FileFormat::OpenImageIO);
  EXPECT_LT(FileFormat::OpenImageIO, FileFormat::BioFormats);
}

} // namespace
} // namespace nim
