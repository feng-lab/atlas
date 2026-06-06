#include "zneuroglancerprecomputedchunkdecoder.h"
#include "zneuroglancerprecomputed.h"
#include "zexception.h"
#include "zimg.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdint>
#include <span>
#include <vector>

namespace nim {
namespace {

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

std::vector<uint8_t> makeConstantCompressoChunkBytes(size_t sx, size_t sy, size_t sz, uint8_t value)
{
  // Minimal cpso payload that produces a constant-volume output:
  // - format_version=0, connectivity=6
  // - data_width=1
  // - value_size=0, location_size=0, window_bytes=0 => boundaries all false
  // - id_size=1 => ids[1]=value => whole volume is one connected component

  constexpr size_t kHeaderSize = 36;
  std::vector<uint8_t> out;
  out.resize(kHeaderSize + 1, 0);

  out[0] = 'c';
  out[1] = 'p';
  out[2] = 's';
  out[3] = 'o';
  out[4] = 0; // format_version
  out[5] = 1; // data_width

  auto writeU16 = [&](size_t off, uint16_t v) {
    out[off + 0] = static_cast<uint8_t>(v & 0xffu);
    out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xffu);
  };
  auto writeU32 = [&](size_t off, uint32_t v) {
    out[off + 0] = static_cast<uint8_t>((v >> 0) & 0xffu);
    out[off + 1] = static_cast<uint8_t>((v >> 8) & 0xffu);
    out[off + 2] = static_cast<uint8_t>((v >> 16) & 0xffu);
    out[off + 3] = static_cast<uint8_t>((v >> 24) & 0xffu);
  };
  auto writeU64 = [&](size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      out[off + i] = static_cast<uint8_t>((v >> (8 * i)) & 0xffu);
    }
  };

  writeU16(6, static_cast<uint16_t>(sx));
  writeU16(8, static_cast<uint16_t>(sy));
  writeU16(10, static_cast<uint16_t>(sz));

  out[12] = 4; // xstep
  out[13] = 4; // ystep
  out[14] = 1; // zstep

  writeU64(15, 1); // id_size
  writeU32(23, 0); // value_size
  writeU64(27, 0); // location_size

  out[35] = 6; // connectivity

  // ids[1]
  out[kHeaderSize] = value;
  return out;
}

} // namespace

TEST(ZNeuroglancerPrecomputedChunkDecoder, DecodePngRgb8ToRawPlanar)
{
  const size_t width = 2;
  const size_t height = 2;
  const std::vector<uint8_t> interleaved = {
    // row 0
    1, 2, 3, 4, 5, 6,
    // row 1
    7, 8, 9, 10, 11, 12,
  };

  const auto pngBytes = encodeRgb8PngToMemory(width, height, interleaved);
  const auto raw =
    ZNeuroglancerPrecomputedChunkDecoder::decodePngToRaw(std::span<const uint8_t>(pngBytes.data(), pngBytes.size()),
                                                         /*expectedVoxelCount=*/width * height,
                                                         /*expectedChannels=*/3,
                                                         /*bytesPerVoxel=*/1);

  const std::vector<uint8_t> expectedPlanar = {
    // R
    1, 4, 7, 10,
    // G
    2, 5, 8, 11,
    // B
    3, 6, 9, 12,
  };
  EXPECT_EQ(raw, expectedPlanar);
}

TEST(ZNeuroglancerPrecomputedChunkDecoder, DecodePngRejectsInvalidPayload)
{
  const std::vector<uint8_t> notPng = {'n', 'o', 't', '-', 'p', 'n', 'g'};
  EXPECT_THROW(
    {
      (void)ZNeuroglancerPrecomputedChunkDecoder::decodePngToRaw(std::span<const uint8_t>(notPng.data(), notPng.size()),
                                                                 /*expectedVoxelCount=*/1,
                                                                 /*expectedChannels=*/1,
                                                                 /*bytesPerVoxel=*/1);
    },
    ZException);
}

TEST(ZNeuroglancerPrecomputedChunkDecoder, DecodePngRejectsTruncatedPayloadAfterHeader)
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
      (void)ZNeuroglancerPrecomputedChunkDecoder::decodePngToRaw(
        std::span<const uint8_t>(pngBytes.data(), pngBytes.size()),
        /*expectedVoxelCount=*/width * height,
        /*expectedChannels=*/3,
        /*bytesPerVoxel=*/1);
    },
    ZException);
}

TEST(ZNeuroglancerPrecomputedChunkDecoder, DecodeCompressoMinimalConstantChunk)
{
  const size_t sx = 4;
  const size_t sy = 3;
  const size_t sz = 2;
  const uint8_t value = 42;
  const auto bytes = makeConstantCompressoChunkBytes(sx, sy, sz, value);

  const auto raw =
    ZNeuroglancerPrecomputedChunkDecoder::decodeCompressoToRaw(std::span<const uint8_t>(bytes.data(), bytes.size()),
                                                               /*expectedChunkSize=*/{sx, sy, sz},
                                                               /*bytesPerVoxel=*/1);

  ASSERT_EQ(raw.size(), sx * sy * sz);
  for (uint8_t v : raw) {
    EXPECT_EQ(v, value);
  }
}

TEST(ZNeuroglancerPrecomputedVolume, NormalizeRootUrlSupportsS3)
{
  EXPECT_EQ(ZNeuroglancerPrecomputedVolume::normalizeRootUrl(QStringLiteral("s3://my-bucket/dataset")),
            QStringLiteral("https://my-bucket.s3.amazonaws.com/dataset/"));

  EXPECT_EQ(ZNeuroglancerPrecomputedVolume::normalizeRootUrl(QStringLiteral("precomputed://s3://my-bucket/dataset/info")),
            QStringLiteral("https://my-bucket.s3.amazonaws.com/dataset/"));

  // Buckets with dots require path-style addressing to avoid TLS wildcard mismatches.
  EXPECT_EQ(ZNeuroglancerPrecomputedVolume::normalizeRootUrl(QStringLiteral("s3://my.bucket/dataset")),
            QStringLiteral("https://s3.amazonaws.com/my.bucket/dataset/"));
}

} // namespace nim
