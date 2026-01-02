#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nim {

class ZNeuroglancerPrecomputedChunkDecoder
{
public:
  static std::vector<uint8_t> decodePngToRaw(std::span<const uint8_t> pngBytes,
                                             size_t expectedVoxelCount,
                                             size_t expectedChannels,
                                             size_t bytesPerVoxel);

  static std::vector<uint8_t> decodeJpegToRaw(std::span<const uint8_t> jpegBytes,
                                              size_t expectedVoxelCount,
                                              size_t expectedChannels);

  static std::vector<uint8_t> decodeCompressoToRaw(std::span<const uint8_t> bytes,
                                                   const std::array<size_t, 3>& expectedChunkSize,
                                                   size_t bytesPerVoxel);

  static std::vector<uint8_t> decodeCompressedSegmentationToRaw(std::span<const uint8_t> bytes,
                                                                const std::array<size_t, 3>& chunkSize,
                                                                size_t numChannels,
                                                                const std::array<size_t, 3>& blockSize,
                                                                bool isUint64);
};

} // namespace nim
