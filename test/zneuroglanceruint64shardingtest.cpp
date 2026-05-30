#include "zneuroglanceruint64sharding.h"

#include "ztest.h"

#include <array>
#include <cstdint>
#include <vector>

namespace nim {
namespace {

// MurmurHash3 was written by Austin Appleby and placed in the public domain.
// This test includes a small reference implementation to avoid depending on
// third-party headers that are not portable across toolchains (e.g., GCC-only
// __attribute__ annotations).
uint32_t rotl32(uint32_t x, int8_t r)
{
  return (x << r) | (x >> (32 - r));
}

uint32_t fmix32(uint32_t h)
{
  h ^= h >> 16;
  h *= 0x85ebca6bU;
  h ^= h >> 13;
  h *= 0xc2b2ae35U;
  h ^= h >> 16;
  return h;
}

uint32_t readU32LE(const uint8_t* p)
{
  return (static_cast<uint32_t>(p[0]) << 0) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void murmurHash3X86_128Reference(const uint8_t* data, size_t len, uint32_t seed, uint32_t out[4])
{
  uint32_t h1 = seed;
  uint32_t h2 = seed;
  uint32_t h3 = seed;
  uint32_t h4 = seed;

  constexpr uint32_t c1 = 0x239b961bU;
  constexpr uint32_t c2 = 0xab0e9789U;
  constexpr uint32_t c3 = 0x38b34ae5U;
  constexpr uint32_t c4 = 0xa1e38b93U;

  // Body (process 16-byte blocks as 4x32-bit little-endian words).
  const size_t nblocks = len / 16;
  for (size_t i = 0; i < nblocks; ++i) {
    const uint8_t* block = data + i * 16;
    uint32_t k1 = readU32LE(block + 0);
    uint32_t k2 = readU32LE(block + 4);
    uint32_t k3 = readU32LE(block + 8);
    uint32_t k4 = readU32LE(block + 12);

    k1 *= c1;
    k1 = rotl32(k1, 15);
    k1 *= c2;
    h1 ^= k1;
    h1 = rotl32(h1, 19);
    h1 += h2;
    h1 = h1 * 5U + 0x561ccd1bU;

    k2 *= c2;
    k2 = rotl32(k2, 16);
    k2 *= c3;
    h2 ^= k2;
    h2 = rotl32(h2, 17);
    h2 += h3;
    h2 = h2 * 5U + 0x0bcaa747U;

    k3 *= c3;
    k3 = rotl32(k3, 17);
    k3 *= c4;
    h3 ^= k3;
    h3 = rotl32(h3, 15);
    h3 += h4;
    h3 = h3 * 5U + 0x96cd1c35U;

    k4 *= c4;
    k4 = rotl32(k4, 18);
    k4 *= c1;
    h4 ^= k4;
    h4 = rotl32(h4, 13);
    h4 += h1;
    h4 = h4 * 5U + 0x32ac3b17U;
  }

  // Tail (last 0..15 bytes).
  const uint8_t* tail = data + nblocks * 16;
  uint32_t k1 = 0;
  uint32_t k2 = 0;
  uint32_t k3 = 0;
  uint32_t k4 = 0;

  switch (len & 15U) {
  case 15:
    k4 ^= static_cast<uint32_t>(tail[14]) << 16;
    [[fallthrough]];
  case 14:
    k4 ^= static_cast<uint32_t>(tail[13]) << 8;
    [[fallthrough]];
  case 13:
    k4 ^= static_cast<uint32_t>(tail[12]) << 0;
    k4 *= c4;
    k4 = rotl32(k4, 18);
    k4 *= c1;
    h4 ^= k4;
    [[fallthrough]];
  case 12:
    k3 ^= static_cast<uint32_t>(tail[11]) << 24;
    [[fallthrough]];
  case 11:
    k3 ^= static_cast<uint32_t>(tail[10]) << 16;
    [[fallthrough]];
  case 10:
    k3 ^= static_cast<uint32_t>(tail[9]) << 8;
    [[fallthrough]];
  case 9:
    k3 ^= static_cast<uint32_t>(tail[8]) << 0;
    k3 *= c3;
    k3 = rotl32(k3, 17);
    k3 *= c4;
    h3 ^= k3;
    [[fallthrough]];
  case 8:
    k2 ^= static_cast<uint32_t>(tail[7]) << 24;
    [[fallthrough]];
  case 7:
    k2 ^= static_cast<uint32_t>(tail[6]) << 16;
    [[fallthrough]];
  case 6:
    k2 ^= static_cast<uint32_t>(tail[5]) << 8;
    [[fallthrough]];
  case 5:
    k2 ^= static_cast<uint32_t>(tail[4]) << 0;
    k2 *= c2;
    k2 = rotl32(k2, 16);
    k2 *= c3;
    h2 ^= k2;
    [[fallthrough]];
  case 4:
    k1 ^= static_cast<uint32_t>(tail[3]) << 24;
    [[fallthrough]];
  case 3:
    k1 ^= static_cast<uint32_t>(tail[2]) << 16;
    [[fallthrough]];
  case 2:
    k1 ^= static_cast<uint32_t>(tail[1]) << 8;
    [[fallthrough]];
  case 1:
    k1 ^= static_cast<uint32_t>(tail[0]) << 0;
    k1 *= c1;
    k1 = rotl32(k1, 15);
    k1 *= c2;
    h1 ^= k1;
    break;
  default:
    break;
  }

  // Finalization.
  const uint32_t u32len = static_cast<uint32_t>(len);
  h1 ^= u32len;
  h2 ^= u32len;
  h3 ^= u32len;
  h4 ^= u32len;

  h1 += h2;
  h1 += h3;
  h1 += h4;
  h2 += h1;
  h3 += h1;
  h4 += h1;

  h1 = fmix32(h1);
  h2 = fmix32(h2);
  h3 = fmix32(h3);
  h4 = fmix32(h4);

  h1 += h2;
  h1 += h3;
  h1 += h4;
  h2 += h1;
  h3 += h1;
  h4 += h1;

  out[0] = h1;
  out[1] = h2;
  out[2] = h3;
  out[3] = h4;
}

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
  murmurHash3X86_128Reference(bytes, 8, seed, out32);
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
    EXPECT_EQ(got, ref) << fmt::format("input={:x}", input);
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
