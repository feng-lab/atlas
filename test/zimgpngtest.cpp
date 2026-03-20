#include "zexception.h"
#include "zimg.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace nim {
namespace {

[[nodiscard]] QString writeTestPngOrThrow(const QString& dirPath)
{
  ZImgInfo info(4, 3, 1, 3, 1, 1, VoxelFormat::Unsigned);
  ZImg img(info);

  const size_t pixelCount = img.width() * img.height();
  for (size_t i = 0; i < pixelCount; ++i) {
    img.channelData<uint8_t>(0)[i] = static_cast<uint8_t>(i + 1);
    img.channelData<uint8_t>(1)[i] = static_cast<uint8_t>(50 + i);
    img.channelData<uint8_t>(2)[i] = static_cast<uint8_t>(100 + i);
  }

  const QString path = QDir(dirPath).filePath(QStringLiteral("fixture.png"));
  img.save(path, FileFormat::Png);
  return path;
}

TEST(ZImgPng, RoundTripsBasicRgbImage)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString path = writeTestPngOrThrow(tmp.path());
  const ZImg img(path, ZImgRegion(), 0, 1, 1, 1, FileFormat::Png);

  ASSERT_EQ(img.width(), 4u);
  ASSERT_EQ(img.height(), 3u);
  ASSERT_EQ(img.numChannels(), 3u);
  EXPECT_EQ(img.channelData<uint8_t>(0)[0], 1u);
  EXPECT_EQ(img.channelData<uint8_t>(1)[0], 50u);
  EXPECT_EQ(img.channelData<uint8_t>(2)[0], 100u);
  EXPECT_EQ(img.channelData<uint8_t>(0)[img.width() * img.height() - 1], 12u);
}

TEST(ZImgPng, RejectsTruncatedLocalFile)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const QString validPath = writeTestPngOrThrow(tmp.path());
  QFile validFile(validPath);
  ASSERT_TRUE(validFile.open(QIODevice::ReadOnly));
  QByteArray bytes = validFile.readAll();
  validFile.close();

  ASSERT_GT(bytes.size(), 16);
  bytes.chop(16);

  const QString truncatedPath = QDir(tmp.path()).filePath(QStringLiteral("truncated.png"));
  QFile truncatedFile(truncatedPath);
  ASSERT_TRUE(truncatedFile.open(QIODevice::WriteOnly));
  ASSERT_EQ(truncatedFile.write(bytes), bytes.size());
  truncatedFile.close();

  EXPECT_THROW((void)ZImg::readImgInfos(truncatedPath, nullptr, FileFormat::Png), ZException);
  EXPECT_THROW((void)ZImg(truncatedPath, ZImgRegion(), 0, 1, 1, 1, FileFormat::Png), ZException);
}

} // namespace
} // namespace nim
