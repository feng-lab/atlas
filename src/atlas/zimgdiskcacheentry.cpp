#include "zimgdiskcacheentry.h"

#include "zimg.h"
#include "zlog.h"
#include "zstructutils.h"

#include <folly/compression/Compression.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>

namespace nim {

namespace {

struct ZImgDiskCacheCompressionConfig
{
  folly::compression::CodecType codecType = folly::compression::CodecType::NO_COMPRESSION;
  int codecLevel = folly::compression::COMPRESSION_LEVEL_DEFAULT;
};

// Disk-cache entry compression policy for ZImg payloads.
// This is shared by all ZImg disk-cache entry types (region cache, preview cache, etc.).
constexpr ZImgDiskCacheCompressionConfig kImgDiskCacheCompression{
  .codecType = folly::compression::CodecType::ZSTD,
  .codecLevel = folly::compression::COMPRESSION_LEVEL_DEFAULT,
};

struct ZImgDiskCacheEntryHeader
{
  std::array<char, 8> magic{};
  uint32_t version = 0;
  uint32_t codecType = 0; // folly::compression::CodecType (0 = none / raw)
  int32_t codecLevel = 0; // informational (ignored on decompress)
  uint32_t reserved0 = 0;
  uint64_t w = 0;
  uint64_t h = 0;
  uint64_t d = 0;
  uint64_t numChannels = 0;
  uint64_t numTimes = 0;
  uint32_t bytesPerVoxel = 0;
  uint32_t voxelFormat = 0;
  uint32_t validBitCount = 0;
  uint64_t rawBytes = 0;     // uncompressed bytes
  uint64_t payloadBytes = 0; // bytes stored after the header (compressed or raw)
};

[[nodiscard]] std::shared_ptr<ZImg> parseZImgDiskCacheEntryImpl(std::span<const std::uint8_t> bytes,
                                                                const std::array<char, 8>& expectedMagic,
                                                                uint32_t expectedVersion)
{
  if (std::endian::native != std::endian::little) {
    return std::shared_ptr<ZImg>();
  }

  const size_t headerBytes = compactSize(ZImgDiskCacheEntryHeader{});
  if (bytes.size() < headerBytes) {
    return std::shared_ptr<ZImg>();
  }

  ZImgDiskCacheEntryHeader hdr{};
  readStructFromCompactMemory(hdr, bytes.data(), headerBytes);

  if (hdr.magic != expectedMagic) {
    return std::shared_ptr<ZImg>();
  }
  if (hdr.version != expectedVersion) {
    return std::shared_ptr<ZImg>();
  }
  if (hdr.w == 0 || hdr.h == 0 || hdr.d == 0 || hdr.numChannels == 0 || hdr.numTimes == 0) {
    return std::shared_ptr<ZImg>();
  }
  if (hdr.bytesPerVoxel == 0) {
    return std::shared_ptr<ZImg>();
  }
  if (hdr.voxelFormat < static_cast<uint32_t>(std::to_underlying(VoxelFormat::Unsigned)) ||
      hdr.voxelFormat > static_cast<uint32_t>(std::to_underlying(VoxelFormat::Float))) {
    return std::shared_ptr<ZImg>();
  }

  auto mulNoOverflow = [](uint64_t a, uint64_t b, uint64_t* out) -> bool {
    if (out == nullptr) {
      return false;
    }
    if (a == 0 || b == 0) {
      *out = 0;
      return true;
    }
    if (a > std::numeric_limits<uint64_t>::max() / b) {
      return false;
    }
    *out = a * b;
    return true;
  };

  uint64_t expectedBytes = 1;
  if (!mulNoOverflow(expectedBytes, hdr.w, &expectedBytes) || !mulNoOverflow(expectedBytes, hdr.h, &expectedBytes) ||
      !mulNoOverflow(expectedBytes, hdr.d, &expectedBytes) ||
      !mulNoOverflow(expectedBytes, hdr.numChannels, &expectedBytes) ||
      !mulNoOverflow(expectedBytes, hdr.numTimes, &expectedBytes) ||
      !mulNoOverflow(expectedBytes, hdr.bytesPerVoxel, &expectedBytes)) {
    return std::shared_ptr<ZImg>();
  }
  if (expectedBytes != hdr.rawBytes) {
    return std::shared_ptr<ZImg>();
  }

  if (hdr.rawBytes > std::numeric_limits<size_t>::max()) {
    return std::shared_ptr<ZImg>();
  }
  const size_t rawBytesSz = static_cast<size_t>(hdr.rawBytes);
  if (rawBytesSz == 0) {
    return std::shared_ptr<ZImg>();
  }

  if (hdr.payloadBytes > std::numeric_limits<size_t>::max()) {
    return std::shared_ptr<ZImg>();
  }
  const size_t payloadBytesSz = static_cast<size_t>(hdr.payloadBytes);
  if (payloadBytesSz == 0) {
    return std::shared_ptr<ZImg>();
  }

  if (headerBytes > bytes.size() - payloadBytesSz) {
    return std::shared_ptr<ZImg>();
  }

  ZImgInfo info;
  info.width = static_cast<size_t>(hdr.w);
  info.height = static_cast<size_t>(hdr.h);
  info.depth = static_cast<size_t>(hdr.d);
  info.numChannels = static_cast<size_t>(hdr.numChannels);
  info.numTimes = static_cast<size_t>(hdr.numTimes);
  info.bytesPerVoxel = static_cast<size_t>(hdr.bytesPerVoxel);
  info.voxelFormat = static_cast<VoxelFormat>(hdr.voxelFormat);
  info.validBitCount = static_cast<size_t>(hdr.validBitCount);
  info.createDefaultDescriptions();

  std::shared_ptr<ZImg> img;
  try {
    img = std::make_shared<ZImg>(info);
  } catch (...) {
    return std::shared_ptr<ZImg>();
  }

  const size_t timeBytes = img->timeByteNumber();
  if (timeBytes == 0) {
    return std::shared_ptr<ZImg>();
  }
  if (img->numTimes() == 0 || (timeBytes * img->numTimes()) != rawBytesSz) {
    return std::shared_ptr<ZImg>();
  }

  const uint8_t* payload = bytes.data() + headerBytes;

  if (hdr.codecType != 0) {
    std::unique_ptr<folly::compression::StreamCodec> codec;
    try {
      codec = folly::compression::getStreamCodec(static_cast<folly::compression::CodecType>(hdr.codecType));
    } catch (...) {
      codec.reset();
    }
    if (!codec) {
      return std::shared_ptr<ZImg>();
    }

    try {
      codec->resetStream(hdr.rawBytes);
    } catch (...) {
      return std::shared_ptr<ZImg>();
    }

    folly::ByteRange inRange(payload, payloadBytesSz);
    bool done = false;
    for (size_t ti = 0; ti < img->numTimes(); ++ti) {
      uint8_t* dstTime = img->timeData<uint8_t>(ti);
      if (dstTime == nullptr) {
        return std::shared_ptr<ZImg>();
      }

      folly::MutableByteRange outRange(dstTime, timeBytes);
      while (!outRange.empty() && !done) {
        const size_t inBefore = inRange.size();
        const size_t outBefore = outRange.size();
        bool thisDone = false;
        try {
          thisDone = codec->uncompressStream(inRange, outRange, folly::compression::StreamCodec::FlushOp::NONE);
        } catch (...) {
          return std::shared_ptr<ZImg>();
        }

        if (inRange.size() == inBefore && outRange.size() == outBefore) {
          return std::shared_ptr<ZImg>();
        }

        if (thisDone) {
          if ((ti + 1) != img->numTimes() || !outRange.empty()) {
            return std::shared_ptr<ZImg>();
          }
          done = true;
        }

        if (inRange.empty() && !done && !outRange.empty()) {
          return std::shared_ptr<ZImg>();
        }
      }
      if (!outRange.empty()) {
        return std::shared_ptr<ZImg>();
      }
    }

    if (!done || !inRange.empty()) {
      return std::shared_ptr<ZImg>();
    }

    return img;
  }

  // Uncompressed payload.
  if (payloadBytesSz != rawBytesSz) {
    return std::shared_ptr<ZImg>();
  }

  size_t offset = 0;
  for (size_t ti = 0; ti < img->numTimes(); ++ti) {
    uint8_t* dstTime = img->timeData<uint8_t>(ti);
    if (dstTime == nullptr) {
      return std::shared_ptr<ZImg>();
    }
    std::memcpy(dstTime, payload + offset, timeBytes);
    offset += timeBytes;
  }
  if (offset != rawBytesSz) {
    return std::shared_ptr<ZImg>();
  }
  return img;
}

} // namespace

size_t zimgDiskCacheEntryHeaderBytes()
{
  return compactSize(ZImgDiskCacheEntryHeader{});
}

ZSqliteLRUCache::Blob serializeZImgDiskCacheEntry(const ZImg& img,
                                                  const std::array<char, 8>& magic,
                                                  uint32_t version)
{
  if (std::endian::native != std::endian::little) {
    return {};
  }

  const ZImgInfo& info = img.info();

  if (img.byteNumber() > std::numeric_limits<uint64_t>::max()) {
    return {};
  }

  ZImgDiskCacheEntryHeader hdr{};
  hdr.magic = magic;
  hdr.version = version;
  hdr.w = static_cast<uint64_t>(info.width);
  hdr.h = static_cast<uint64_t>(info.height);
  hdr.d = static_cast<uint64_t>(info.depth);
  hdr.numChannels = static_cast<uint64_t>(info.numChannels);
  hdr.numTimes = static_cast<uint64_t>(info.numTimes);
  hdr.bytesPerVoxel = static_cast<uint32_t>(info.bytesPerVoxel);
  hdr.voxelFormat = static_cast<uint32_t>(std::to_underlying(info.voxelFormat));
  hdr.validBitCount = static_cast<uint32_t>(info.validBitCount);
  hdr.rawBytes = static_cast<uint64_t>(img.byteNumber());

  const size_t headerBytes = compactSize(ZImgDiskCacheEntryHeader{});
  const uint64_t rawBytes = hdr.rawBytes;
  if (rawBytes > std::numeric_limits<size_t>::max()) {
    return {};
  }

  const size_t rawBytesSz = static_cast<size_t>(rawBytes);
  if (rawBytesSz == 0) {
    return {};
  }

  const size_t timeBytes = img.timeByteNumber();
  if (timeBytes == 0) {
    return {};
  }
  if (img.numTimes() == 0 || static_cast<uint64_t>(timeBytes) * static_cast<uint64_t>(img.numTimes()) != rawBytes) {
    return {};
  }

  // Always attempt compression (design choice). If compression fails or doesn't
  // reduce size, fall back to uncompressed storage.
  {
    std::unique_ptr<folly::compression::StreamCodec> codec;
    try {
      codec = folly::compression::getStreamCodec(kImgDiskCacheCompression.codecType, kImgDiskCacheCompression.codecLevel);
    } catch (...) {
      codec.reset();
    }

    if (codec) {
      bool ok = true;
      try {
        codec->resetStream(rawBytes);
      } catch (...) {
        ok = false;
      }

      uint64_t bound64 = 0;
      if (ok) {
        try {
          bound64 = codec->maxCompressedLength(rawBytes);
        } catch (...) {
          ok = false;
        }
      }

      ZSqliteLRUCache::Blob compressed;
      if (ok) {
        if (bound64 == 0 || bound64 > std::numeric_limits<size_t>::max()) {
          ok = false;
        } else {
          try {
            compressed.resize(static_cast<size_t>(bound64));
          } catch (...) {
            ok = false;
          }
        }
      }

      if (ok && !compressed.empty()) {
        folly::MutableByteRange outRange(compressed.data(), compressed.size());
        try {
          for (size_t ti = 0; ti < img.numTimes(); ++ti) {
            const uint8_t* src = img.timeData<uint8_t>(ti);
            if (src == nullptr) {
              ok = false;
              break;
            }

            folly::ByteRange inRange(src, timeBytes);
            while (!inRange.empty()) {
              const size_t inBefore = inRange.size();
              const size_t outBefore = outRange.size();
              codec->compressStream(inRange, outRange, folly::compression::StreamCodec::FlushOp::NONE);
              if (inRange.size() == inBefore && outRange.size() == outBefore) {
                ok = false;
                break;
              }
              if (outRange.empty() && !inRange.empty()) {
                ok = false;
                break;
              }
            }
            if (!ok) {
              break;
            }
          }

          if (ok) {
            folly::ByteRange emptyIn;
            bool done = false;
            while (!done) {
              const size_t outBefore = outRange.size();
              done = codec->compressStream(emptyIn, outRange, folly::compression::StreamCodec::FlushOp::END);
              if (!done && outRange.size() == outBefore) {
                ok = false;
                break;
              }
              if (!done && outRange.empty()) {
                ok = false;
                break;
              }
            }
          }
        } catch (...) {
          ok = false;
        }

        if (ok) {
          const size_t produced = compressed.size() - outRange.size();
          compressed.resize(produced);

          if (!compressed.empty() && compressed.size() < rawBytesSz) {
            ZImgDiskCacheEntryHeader compressedHdr = hdr;
            compressedHdr.codecType = static_cast<uint32_t>(kImgDiskCacheCompression.codecType);
            compressedHdr.codecLevel = static_cast<int32_t>(kImgDiskCacheCompression.codecLevel);
            compressedHdr.payloadBytes = static_cast<uint64_t>(compressed.size());

            ZSqliteLRUCache::Blob out;
            try {
              out.resize(headerBytes + compressed.size());
            } catch (...) {
              out.clear();
            }
            if (!out.empty() && compactStructToMemory(out.data(), headerBytes, compressedHdr) == headerBytes) {
              std::memcpy(out.data() + headerBytes, compressed.data(), compressed.size());
              return out;
            }
          }
        }
      }
    }
  }

  // Uncompressed fallback.
  hdr.codecType = 0;
  hdr.codecLevel = 0;
  hdr.payloadBytes = hdr.rawBytes;

  if (headerBytes > std::numeric_limits<size_t>::max() - rawBytesSz) {
    return {};
  }

  ZSqliteLRUCache::Blob out;
  try {
    out.resize(headerBytes + rawBytesSz);
  } catch (...) {
    return {};
  }
  if (out.empty()) {
    return {};
  }
  if (compactStructToMemory(out.data(), headerBytes, hdr) != headerBytes) {
    return {};
  }

  size_t offset = headerBytes;
  for (size_t ti = 0; ti < img.numTimes(); ++ti) {
    const uint8_t* src = img.timeData<uint8_t>(ti);
    if (src == nullptr) {
      return {};
    }
    std::memcpy(out.data() + offset, src, timeBytes);
    offset += timeBytes;
  }
  if (offset != out.size()) {
    return {};
  }

  return out;
}

std::shared_ptr<ZImg> parseZImgDiskCacheEntry(std::span<const std::uint8_t> bytes,
                                              const std::array<char, 8>& expectedMagic,
                                              uint32_t expectedVersion)
{
  return parseZImgDiskCacheEntryImpl(bytes, expectedMagic, expectedVersion);
}

} // namespace nim

