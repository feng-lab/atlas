#include "zimgdiskcacheentry.h"

#include "zimg.h"
#include "zstructutils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>

namespace nim {
namespace {

constexpr std::array<char, 8> kTestMagic = {'A', 'T', 'L', 'S', 'T', 'S', 'T', '1'};
constexpr uint32_t kTestVersion = 1;

struct ZImgDiskCacheEntryHeaderForTest
{
  std::array<char, 8> magic{};
  uint32_t version = 0;
  uint32_t codecType = 0;
  int32_t codecLevel = 0;
  uint32_t reserved0 = 0;
  uint64_t w = 0;
  uint64_t h = 0;
  uint64_t d = 0;
  uint64_t numChannels = 0;
  uint64_t numTimes = 0;
  uint32_t bytesPerVoxel = 0;
  uint32_t voxelFormat = 0;
  uint32_t validBitCount = 0;
  uint64_t rawBytes = 0;
  uint64_t payloadBytes = 0;
};

void fillWithZeros(ZImg& img)
{
  for (size_t t = 0; t < img.numTimes(); ++t) {
    uint8_t* dst = img.timeData<uint8_t>(t);
    ASSERT_NE(dst, nullptr);
    std::fill_n(dst, img.timeByteNumber(), 0);
  }
}

void fillWithDeterministicRandomBytes(ZImg& img, uint32_t seed)
{
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> dist(0, 255);
  for (size_t t = 0; t < img.numTimes(); ++t) {
    uint8_t* dst = img.timeData<uint8_t>(t);
    ASSERT_NE(dst, nullptr);
    for (size_t i = 0; i < img.timeByteNumber(); ++i) {
      dst[i] = static_cast<uint8_t>(dist(rng));
    }
  }
}

void expectSameImgBytesAndType(const ZImg& expected, const ZImg& actual)
{
  EXPECT_EQ(actual.width(), expected.width());
  EXPECT_EQ(actual.height(), expected.height());
  EXPECT_EQ(actual.depth(), expected.depth());
  EXPECT_EQ(actual.numChannels(), expected.numChannels());
  EXPECT_EQ(actual.numTimes(), expected.numTimes());
  EXPECT_EQ(actual.bytesPerVoxel(), expected.bytesPerVoxel());
  EXPECT_EQ(actual.voxelFormat(), expected.voxelFormat());
  EXPECT_EQ(actual.validBitCount(), expected.validBitCount());
  ASSERT_EQ(actual.byteNumber(), expected.byteNumber());
  ASSERT_EQ(actual.timeByteNumber(), expected.timeByteNumber());

  for (size_t t = 0; t < expected.numTimes(); ++t) {
    const uint8_t* a = expected.timeData<uint8_t>(t);
    const uint8_t* b = actual.timeData<uint8_t>(t);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(std::memcmp(a, b, expected.timeByteNumber()), 0);
  }
}

} // namespace

TEST(ZImgDiskCacheEntry, HeaderBytesMatchesCompactStructSize)
{
  EXPECT_GT(zimgDiskCacheEntryHeaderBytes(), 0u);
  EXPECT_EQ(zimgDiskCacheEntryHeaderBytes(), compactSize(ZImgDiskCacheEntryHeaderForTest{}));
}

TEST(ZImgDiskCacheEntry, RoundTripCompressedZeros_UInt16_MultiTimeMultiChannel)
{
  ZImgInfo info;
  info.width = 64;
  info.height = 64;
  info.depth = 64;
  info.numChannels = 2;
  info.numTimes = 3;
  info.voxelFormat = VoxelFormat::Unsigned;
  info.bytesPerVoxel = 2;
  info.validBitCount = 12;
  info.createDefaultDescriptions();

  ZImg img(info);
  fillWithZeros(img);

  const auto bytes = serializeZImgDiskCacheEntry(img, kTestMagic, kTestVersion);
  ASSERT_FALSE(bytes.empty());

  const size_t headerBytes = zimgDiskCacheEntryHeaderBytes();
  ASSERT_GE(bytes.size(), headerBytes);

  ZImgDiskCacheEntryHeaderForTest hdr{};
  readStructFromCompactMemory(hdr, bytes.data(), headerBytes);
  EXPECT_EQ(hdr.magic, kTestMagic);
  EXPECT_EQ(hdr.version, kTestVersion);
  EXPECT_NE(hdr.codecType, 0u);
  EXPECT_LT(bytes.size(), headerBytes + img.byteNumber());

  auto parsed =
    parseZImgDiskCacheEntry(std::span<const std::uint8_t>(bytes.data(), bytes.size()), kTestMagic, kTestVersion);
  ASSERT_TRUE(parsed);
  expectSameImgBytesAndType(img, *parsed);
}

TEST(ZImgDiskCacheEntry, RoundTripUncompressedRandom_UInt8)
{
  ZImgInfo info;
  info.width = 16;
  info.height = 16;
  info.depth = 16;
  info.numChannels = 1;
  info.numTimes = 1;
  info.voxelFormat = VoxelFormat::Unsigned;
  info.bytesPerVoxel = 1;
  info.validBitCount = 8;
  info.createDefaultDescriptions();

  ZImg img(info);
  fillWithDeterministicRandomBytes(img, /*seed=*/123);

  const auto bytes = serializeZImgDiskCacheEntry(img, kTestMagic, kTestVersion);
  ASSERT_FALSE(bytes.empty());

  const size_t headerBytes = zimgDiskCacheEntryHeaderBytes();
  ASSERT_GE(bytes.size(), headerBytes);

  ZImgDiskCacheEntryHeaderForTest hdr{};
  readStructFromCompactMemory(hdr, bytes.data(), headerBytes);
  EXPECT_EQ(hdr.magic, kTestMagic);
  EXPECT_EQ(hdr.version, kTestVersion);
  EXPECT_EQ(hdr.codecType, 0u);
  EXPECT_EQ(bytes.size(), headerBytes + img.byteNumber());

  auto parsed =
    parseZImgDiskCacheEntry(std::span<const std::uint8_t>(bytes.data(), bytes.size()), kTestMagic, kTestVersion);
  ASSERT_TRUE(parsed);
  expectSameImgBytesAndType(img, *parsed);
}

TEST(ZImgDiskCacheEntry, RoundTripPreservesVoxelFormats_SignedAndFloat)
{
  {
    ZImgInfo info;
    info.width = 32;
    info.height = 16;
    info.depth = 8;
    info.numChannels = 1;
    info.numTimes = 2;
    info.voxelFormat = VoxelFormat::Signed;
    info.bytesPerVoxel = 2;
    info.validBitCount = 0;
    info.createDefaultDescriptions();

    ZImg img(info);
    fillWithDeterministicRandomBytes(img, /*seed=*/7);

    const auto bytes = serializeZImgDiskCacheEntry(img, kTestMagic, kTestVersion);
    ASSERT_FALSE(bytes.empty());

    auto parsed =
      parseZImgDiskCacheEntry(std::span<const std::uint8_t>(bytes.data(), bytes.size()), kTestMagic, kTestVersion);
    ASSERT_TRUE(parsed);
    expectSameImgBytesAndType(img, *parsed);
  }

  {
    ZImgInfo info;
    info.width = 15;
    info.height = 13;
    info.depth = 9;
    info.numChannels = 1;
    info.numTimes = 1;
    info.voxelFormat = VoxelFormat::Float;
    info.bytesPerVoxel = 4;
    info.validBitCount = 0;
    info.createDefaultDescriptions();

    ZImg img(info);
    fillWithDeterministicRandomBytes(img, /*seed=*/42);

    const auto bytes = serializeZImgDiskCacheEntry(img, kTestMagic, kTestVersion);
    ASSERT_FALSE(bytes.empty());

    auto parsed =
      parseZImgDiskCacheEntry(std::span<const std::uint8_t>(bytes.data(), bytes.size()), kTestMagic, kTestVersion);
    ASSERT_TRUE(parsed);
    expectSameImgBytesAndType(img, *parsed);
  }
}

TEST(ZImgDiskCacheEntry, ParseRejectsWrongMagicVersionAndTruncation)
{
  ZImgInfo info;
  info.width = 32;
  info.height = 32;
  info.depth = 32;
  info.numChannels = 1;
  info.numTimes = 1;
  info.voxelFormat = VoxelFormat::Unsigned;
  info.bytesPerVoxel = 1;
  info.validBitCount = 8;
  info.createDefaultDescriptions();

  ZImg img(info);
  fillWithZeros(img);

  const auto bytes = serializeZImgDiskCacheEntry(img, kTestMagic, kTestVersion);
  ASSERT_FALSE(bytes.empty());

  const std::array<char, 8> wrongMagic = {'B', 'A', 'D', 'M', 'A', 'G', 'I', 'C'};
  EXPECT_FALSE(
    parseZImgDiskCacheEntry(std::span<const std::uint8_t>(bytes.data(), bytes.size()), wrongMagic, kTestVersion));
  EXPECT_FALSE(parseZImgDiskCacheEntry(std::span<const std::uint8_t>(bytes.data(), bytes.size()),
                                       kTestMagic,
                                       kTestVersion + 1));

  const size_t headerBytes = zimgDiskCacheEntryHeaderBytes();
  ASSERT_GT(headerBytes, 0u);

  // Truncated header.
  EXPECT_FALSE(
    parseZImgDiskCacheEntry(std::span<const std::uint8_t>(bytes.data(), headerBytes - 1), kTestMagic, kTestVersion));

  // Truncated payload.
  ASSERT_GT(bytes.size(), 1u);
  EXPECT_FALSE(
    parseZImgDiskCacheEntry(std::span<const std::uint8_t>(bytes.data(), bytes.size() - 1), kTestMagic, kTestVersion));

  // Corrupt magic in-place.
  auto corrupt = bytes;
  corrupt[0] ^= 0xFF;
  EXPECT_FALSE(
    parseZImgDiskCacheEntry(std::span<const std::uint8_t>(corrupt.data(), corrupt.size()), kTestMagic, kTestVersion));
}

} // namespace nim
