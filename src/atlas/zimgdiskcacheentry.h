#pragma once

#include "zsqlitelrucache.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace nim {

class ZImg;

// Returns the compact-serialized header size (bytes) used by Atlas ZImg disk-cache entries.
[[nodiscard]] size_t zimgDiskCacheEntryHeaderBytes();

// Serializes a ZImg into a self-describing disk-cache entry suitable for storage in ZSqliteLRUCache.
//
// Best-effort boundary:
// - Returns an empty blob on unsupported platforms (non-little-endian) or on serialization failure.
[[nodiscard]] ZSqliteLRUCache::Blob serializeZImgDiskCacheEntry(const ZImg& img,
                                                                const std::array<char, 8>& magic,
                                                                uint32_t version);

// Parses a disk-cache entry produced by serializeZImgDiskCacheEntry().
//
// Best-effort boundary:
// - Returns nullptr on parse or decompression failure.
[[nodiscard]] std::shared_ptr<ZImg> parseZImgDiskCacheEntry(std::span<const std::uint8_t> bytes,
                                                            const std::array<char, 8>& expectedMagic,
                                                            uint32_t expectedVersion);

} // namespace nim

