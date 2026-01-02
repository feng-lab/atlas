#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace nim {

class ZNeuroglancerUint64Sharding
{
public:
  struct DecodedMinishardIndex
  {
    std::vector<uint64_t> keys;
    std::vector<uint64_t> starts;
    std::vector<uint64_t> ends;

    [[nodiscard]] size_t byteSize() const;
  };

  static uint64_t readU64LE(const uint8_t* p);

  static uint64_t murmurHash3X86_128Hash64Bits(uint64_t input, uint32_t seed = 0);

  static uint64_t compressedMortonCode(const std::array<uint64_t, 3>& g, const std::array<uint64_t, 3>& gridSize);

  static DecodedMinishardIndex decodeMinishardIndex(std::span<const uint8_t> bytes, uint64_t baseDataOffset);

private:
  static uint32_t rotl32(uint32_t x, int8_t r);
  static uint32_t fmix32(uint32_t h);
};

} // namespace nim

