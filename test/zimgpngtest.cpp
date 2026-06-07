#include "zexception.h"
#include "zimg.h"
#include "zimgpng.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <span>
#include <vector>

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

std::vector<uint8_t> encodeRgb8PngToMemory(size_t width, size_t height, const std::vector<uint8_t>& interleavedRgb)
{
  if (width == 0 || height == 0) {
    throw ZException("invalid PNG dims");
  }
  if (interleavedRgb.size() != width * height * 3) {
    throw ZException("invalid rgb size");
  }

  ZImgInfo info(width, height, 1, 3, 1, 1, VoxelFormat::Unsigned);
  ZImg img(info);
  const size_t pixelCount = width * height;
  for (size_t i = 0; i < pixelCount; ++i) {
    img.channelData<uint8_t>(0)[i] = interleavedRgb[i * 3 + 0];
    img.channelData<uint8_t>(1)[i] = interleavedRgb[i * 3 + 1];
    img.channelData<uint8_t>(2)[i] = interleavedRgb[i * 3 + 2];
  }

  QTemporaryDir tmp;
  if (!tmp.isValid()) {
    throw ZException("failed to create temporary PNG directory");
  }

  const QString path = QDir(tmp.path()).filePath(QStringLiteral("fixture.png"));
  img.save(path, FileFormat::Png);

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    throw ZException("failed to open temporary PNG fixture");
  }

  const QByteArray bytes = file.readAll();
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
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

TEST(ZImgPng, ReadMemRawRgb8ToPlanar)
{
  const size_t width = 2;
  const size_t height = 2;
  const std::vector<uint8_t> interleaved = {
    // row 0
    1,
    2,
    3,
    4,
    5,
    6,
    // row 1
    7,
    8,
    9,
    10,
    11,
    12,
  };

  const auto pngBytes = encodeRgb8PngToMemory(width, height, interleaved);
  const auto raw = ZImgPng::readMemRaw(std::span<const uint8_t>(pngBytes.data(), pngBytes.size()),
                                       /*expectedVoxelCount=*/width * height,
                                       /*expectedChannels=*/3,
                                       /*bytesPerVoxel=*/1);

  const std::vector<uint8_t> expectedPlanar = {
    // R
    1,
    4,
    7,
    10,
    // G
    2,
    5,
    8,
    11,
    // B
    3,
    6,
    9,
    12,
  };
  EXPECT_EQ(raw, expectedPlanar);
}

TEST(ZImgPng, ReadMemRawRejectsInvalidPayload)
{
  const std::vector<uint8_t> notPng = {'n', 'o', 't', '-', 'p', 'n', 'g'};
  EXPECT_THROW(
    {
      (void)ZImgPng::readMemRaw(std::span<const uint8_t>(notPng.data(), notPng.size()),
                                /*expectedVoxelCount=*/1,
                                /*expectedChannels=*/1,
                                /*bytesPerVoxel=*/1);
    },
    ZException);
}

TEST(ZImgPng, ReadMemRawRejectsTruncatedPayloadAfterHeader)
{
  const size_t width = 4;
  const size_t height = 4;
  std::vector<uint8_t> interleaved(width * height * 3);
  for (size_t i = 0; i < interleaved.size(); ++i) {
    interleaved[i] = static_cast<uint8_t>(i);
  }

  auto pngBytes = encodeRgb8PngToMemory(width, height, interleaved);
  ASSERT_GT(pngBytes.size(), 16u);
  pngBytes.resize(pngBytes.size() - 16);

  EXPECT_THROW(
    {
      (void)ZImgPng::readMemRaw(std::span<const uint8_t>(pngBytes.data(), pngBytes.size()),
                                /*expectedVoxelCount=*/width * height,
                                /*expectedChannels=*/3,
                                /*bytesPerVoxel=*/1);
    },
    ZException);
}

} // namespace
} // namespace nim
