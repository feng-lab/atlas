#include "zneuroglanceruint64sharding.h"

#include "zexception.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "../src/3rdparty/thread-safe-lru/hash.h"

namespace nim {
namespace {

void appendU64LE(std::vector<uint8_t>& out, uint64_t v)
{
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xffULL));
  }
}

uint64_t murmurHash3X86_128Hash64BitsReference(uint64_t input, uint32_t seed)
{
  uint8_t bytes[8]{};
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<uint8_t>((input >> (8 * i)) & 0xffULL);
  }

  uint32_t out32[4]{};
  tstarling::MurmurHash3::hash_x86_128(bytes, 8, seed, out32);
  return static_cast<uint64_t>(out32[0]) | (static_cast<uint64_t>(out32[1]) << 32);
}

} // namespace

TEST(ZNeuroglancerUint64Sharding, MurmurHash3X86_128Hash64BitsMatchesReference)
{
  constexpr uint32_t seed = 0;
  const std::array<uint64_t, 6> inputs = {
    0ULL,
    1ULL,
    2ULL,
    0x1234567890abcdefULL,
    0xffffffffffffffffULL,
    0x00000001ffffffffULL,
  };

  for (uint64_t input : inputs) {
    const uint64_t ref = murmurHash3X86_128Hash64BitsReference(input, seed);
    const uint64_t got = ZNeuroglancerUint64Sharding::murmurHash3X86_128Hash64Bits(input, seed);
    EXPECT_EQ(got, ref) << "input=" << std::hex << input;
  }
}

TEST(ZNeuroglancerUint64Sharding, CompressedMortonCode)
{
  {
    const std::array<uint64_t, 3> gridSize{2, 2, 1};
    EXPECT_EQ(ZNeuroglancerUint64Sharding::compressedMortonCode({0, 0, 0}, gridSize), 0ULL);
    EXPECT_EQ(ZNeuroglancerUint64Sharding::compressedMortonCode({1, 0, 0}, gridSize), 1ULL);
    EXPECT_EQ(ZNeuroglancerUint64Sharding::compressedMortonCode({0, 1, 0}, gridSize), 2ULL);
    EXPECT_EQ(ZNeuroglancerUint64Sharding::compressedMortonCode({1, 1, 0}, gridSize), 3ULL);
  }
  {
    const std::array<uint64_t, 3> gridSize{4, 4, 4};
    EXPECT_EQ(ZNeuroglancerUint64Sharding::compressedMortonCode({1, 2, 3}, gridSize), 53ULL);
  }
}

TEST(ZNeuroglancerUint64Sharding, DecodeMinishardIndex)
{
  // Four entries: chunk IDs 0,1,2,3 stored consecutively starting at baseDataOffset.
  const uint64_t baseDataOffset = 16;
  const std::array<uint64_t, 4> keyDeltas{0, 1, 1, 1};
  const std::array<uint64_t, 4> startDeltas{0, 0, 0, 0};
  const std::array<uint64_t, 4> sizes{4, 4, 4, 4};

  std::vector<uint8_t> bytes;
  bytes.reserve(24 * 4);
  for (uint64_t v : keyDeltas) {
    appendU64LE(bytes, v);
  }
  for (uint64_t v : startDeltas) {
    appendU64LE(bytes, v);
  }
  for (uint64_t v : sizes) {
    appendU64LE(bytes, v);
  }

  auto decoded = ZNeuroglancerUint64Sharding::decodeMinishardIndex(std::span<const uint8_t>(bytes.data(), bytes.size()), baseDataOffset);
  ASSERT_EQ(decoded.keys.size(), 4U);
  ASSERT_EQ(decoded.starts.size(), 4U);
  ASSERT_EQ(decoded.ends.size(), 4U);

  EXPECT_EQ(decoded.keys, (std::vector<uint64_t>{0, 1, 2, 3}));
  EXPECT_EQ(decoded.starts, (std::vector<uint64_t>{16, 20, 24, 28}));
  EXPECT_EQ(decoded.ends, (std::vector<uint64_t>{20, 24, 28, 32}));
}

TEST(ZNeuroglancerUint64Sharding, DecodeMinishardIndexRejectsInvalidLength)
{
  const std::vector<uint8_t> bytes{1, 2, 3};
  EXPECT_THROW((ZNeuroglancerUint64Sharding::decodeMinishardIndex(std::span<const uint8_t>(bytes.data(), bytes.size()), 0)),
               ZException);
}

} // namespace nim

