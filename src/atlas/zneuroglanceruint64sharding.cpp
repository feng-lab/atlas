#include "zneuroglanceruint64sharding.h"

#include "zexception.h"

#include <fmt/format.h>
#include <glog/logging.h>

#include <cstring>

namespace nim {

size_t ZNeuroglancerUint64Sharding::DecodedMinishardIndex::byteSize() const
{
  return sizeof(DecodedMinishardIndex) + (keys.size() + starts.size() + ends.size()) * sizeof(uint64_t);
}

uint64_t ZNeuroglancerUint64Sharding::readU64LE(const uint8_t* p)
{
  return (static_cast<uint64_t>(p[0]) << 0) | (static_cast<uint64_t>(p[1]) << 8) | (static_cast<uint64_t>(p[2]) << 16) |
         (static_cast<uint64_t>(p[3]) << 24) | (static_cast<uint64_t>(p[4]) << 32) | (static_cast<uint64_t>(p[5]) << 40) |
         (static_cast<uint64_t>(p[6]) << 48) | (static_cast<uint64_t>(p[7]) << 56);
}

uint32_t ZNeuroglancerUint64Sharding::rotl32(uint32_t x, int8_t r)
{
  return (x << r) | (x >> (32 - r));
}

uint32_t ZNeuroglancerUint64Sharding::fmix32(uint32_t h)
{
  h ^= h >> 16;
  h *= 0x85ebca6bU;
  h ^= h >> 13;
  h *= 0xc2b2ae35U;
  h ^= h >> 16;
  return h;
}

uint64_t ZNeuroglancerUint64Sharding::murmurHash3X86_128Hash64Bits(uint64_t input, uint32_t seed)
{
  uint32_t h1 = seed;
  uint32_t h2 = seed;
  uint32_t h3 = seed;
  uint32_t h4 = seed;

  constexpr uint32_t c1 = 0x239b961bU;
  constexpr uint32_t c2 = 0xab0e9789U;
  constexpr uint32_t c3 = 0x38b34ae5U;

  uint32_t k2 = static_cast<uint32_t>(input >> 32);
  k2 *= c2;
  k2 = rotl32(k2, 16);
  k2 *= c3;
  h2 ^= k2;

  uint32_t k1 = static_cast<uint32_t>(input & 0xffffffffULL);
  k1 *= c1;
  k1 = rotl32(k1, 15);
  k1 *= c2;
  h1 ^= k1;

  constexpr uint32_t len = 8;
  h1 ^= len;
  h2 ^= len;
  h3 ^= len;
  h4 ^= len;

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

  return static_cast<uint64_t>(h1) | (static_cast<uint64_t>(h2) << 32);
}

uint64_t ZNeuroglancerUint64Sharding::compressedMortonCode(const std::array<uint64_t, 3>& g, const std::array<uint64_t, 3>& gridSize)
{
  for (size_t d = 0; d < 3; ++d) {
    CHECK(g[d] < gridSize[d]);
  }

  uint64_t out = 0;
  size_t j = 0;
  for (size_t i = 0; i < 64; ++i) {
    bool any = false;
    for (size_t dim = 0; dim < 3; ++dim) {
      if ((1ULL << i) < gridSize[dim]) {
        any = true;
        if (j >= 64) {
          throw ZException("Compressed Morton code exceeds 64 bits");
        }
        const uint64_t bit = (g[dim] >> i) & 1ULL;
        out |= (bit << j);
        ++j;
      }
    }
    if (!any) {
      break;
    }
  }
  return out;
}

ZNeuroglancerUint64Sharding::DecodedMinishardIndex ZNeuroglancerUint64Sharding::decodeMinishardIndex(std::span<const uint8_t> bytes,
                                                                                                     uint64_t baseDataOffset)
{
  if (bytes.empty()) {
    return {};
  }
  if ((bytes.size() % 24) != 0) {
    throw ZException(fmt::format("Invalid minishard index length: {} (expected multiple of 24)", bytes.size()));
  }
  const size_t n = bytes.size() / 24;
  CHECK(n * 24 == bytes.size());

  DecodedMinishardIndex out{};
  out.keys.resize(n);
  out.starts.resize(n);
  out.ends.resize(n);

  const uint8_t* p = bytes.data();
  std::vector<uint64_t> keyDeltas(n);
  std::vector<uint64_t> startDeltas(n);
  std::vector<uint64_t> sizes(n);

  for (size_t i = 0; i < n; ++i) {
    keyDeltas[i] = readU64LE(p + i * 8);
  }
  for (size_t i = 0; i < n; ++i) {
    startDeltas[i] = readU64LE(p + (n + i) * 8);
  }
  for (size_t i = 0; i < n; ++i) {
    sizes[i] = readU64LE(p + (2 * n + i) * 8);
  }

  uint64_t prevKey = 0;
  uint64_t prevEnd = baseDataOffset;
  for (size_t i = 0; i < n; ++i) {
    prevKey += keyDeltas[i];
    out.keys[i] = prevKey;

    const uint64_t start = prevEnd + startDeltas[i];
    const uint64_t end = start + sizes[i];
    out.starts[i] = start;
    out.ends[i] = end;
    prevEnd = end;
  }
  return out;
}

} // namespace nim

