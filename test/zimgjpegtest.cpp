#include "zexception.h"
#include "zimg.h"
#include "zimgjpeg.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <span>
#include <vector>

namespace nim {
namespace {

ZImg makeGray16Img(size_t width, size_t height, size_t validBitCount)
{
  ZImgInfo info(width, height, 1, 1, 1, 2, VoxelFormat::Unsigned);
  info.validBitCount = validBitCount;
  info.createDefaultDescriptions();
  ZImg img(info);
  for (size_t y = 0; y < img.height(); ++y) {
    for (size_t x = 0; x < img.width(); ++x) {
      *img.data<uint16_t>(x, y) = static_cast<uint16_t>(y * 257 + x * 31);
    }
  }
  return img;
}

TEST(ZImgJpeg, WritesAndReads16BitLossless)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  const ZImg src = makeGray16Img(7, 5, 16);
  const QString path = QDir(tmp.path()).filePath(QStringLiteral("gray16.jpg"));
  src.save(path, FileFormat::Jpeg);

  std::vector<ZImgInfo> infos = ZImg::readImgInfos(path, nullptr, FileFormat::Jpeg);
  ASSERT_EQ(infos.size(), 1u);
  EXPECT_EQ(infos[0].width, src.width());
  EXPECT_EQ(infos[0].height, src.height());
  EXPECT_EQ(infos[0].numChannels, 1u);
  EXPECT_EQ(infos[0].bytesPerVoxel, 2u);
  EXPECT_EQ(infos[0].validBitCount, 16u);

  const ZImg decoded(path, ZImgRegion(), 0, 1, 1, 1, FileFormat::Jpeg);
  ASSERT_EQ(decoded.info().toString(), src.info().toString());
  for (size_t y = 0; y < src.height(); ++y) {
    for (size_t x = 0; x < src.width(); ++x) {
      EXPECT_EQ(*decoded.data<uint16_t>(x, y), *src.data<uint16_t>(x, y));
    }
  }
}

TEST(ZImgJpeg, WritesAndReads12BitFromMemory)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  ZImg src = makeGray16Img(8, 6, 12);
  for (size_t y = 0; y < src.height(); ++y) {
    for (size_t x = 0; x < src.width(); ++x) {
      *src.data<uint16_t>(x, y) = static_cast<uint16_t>((y * 521 + x * 67) & 0x0FFF);
    }
  }

  ZImgWriteParameters paras;
  paras.jpegQuality = 100;
  const QString path = QDir(tmp.path()).filePath(QStringLiteral("gray12.jpg"));
  src.save(path, FileFormat::Jpeg, paras);

  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  const QByteArray bytes = file.readAll();
  ASSERT_GT(bytes.size(), 0);
  const std::span<const uint8_t> jpegBytes(reinterpret_cast<const uint8_t*>(bytes.constData()),
                                           static_cast<size_t>(bytes.size()));

  ZImgInfo info = ZImgJpeg::readMemInfo(jpegBytes);
  EXPECT_EQ(info.width, src.width());
  EXPECT_EQ(info.height, src.height());
  EXPECT_EQ(info.numChannels, 1u);
  EXPECT_EQ(info.bytesPerVoxel, 2u);
  EXPECT_EQ(info.validBitCount, 12u);

  std::vector<uint8_t> decodedBytes(info.byteNumber());
  ZImgJpeg::readMemImg(jpegBytes, std::span<uint8_t>(decodedBytes.data(), decodedBytes.size()));
  ZImg decoded;
  decoded.wrapData(decodedBytes.data(), info);
  for (size_t y = 0; y < decoded.height(); ++y) {
    for (size_t x = 0; x < decoded.width(); ++x) {
      EXPECT_LE(*decoded.data<uint16_t>(x, y), 4095u);
    }
  }
}

TEST(ZImgJpeg, RejectsValuesOutsideDeclaredBitDepth)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  ZImg img = makeGray16Img(3, 2, 12);
  *img.data<uint16_t>(1, 1) = 4096;

  const QString path = QDir(tmp.path()).filePath(QStringLiteral("invalid12.jpg"));
  EXPECT_THROW(img.save(path, FileFormat::Jpeg), ZException);
}

} // namespace
} // namespace nim
