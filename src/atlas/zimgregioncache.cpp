#include "zimgregioncache.h"

#include "zcpuinfo.h"
#include "zdiskcacheutils.h"
#include "zlog.h"
#include "zsqlitediskcachebucket.h"
#include "zstructutils.h"

#include <gflags/gflags.h>

#include <boost/hash2/sha2.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include <folly/compression/Compression.h>

DEFINE_double(atlas_image_region_cache_memory_proportion,
              0.3,
              "Proportion of RAM that will be used for image region cache, default is 0.3");

DECLARE_uint64(atlas_disk_cache_imgregion_max_bytes);
DECLARE_uint64(atlas_disk_cache_imgregion_async_max_pending_bytes);

namespace nim {

namespace {

constexpr char kImgRegionDiskCacheDbName[] = "imgregion.sqlite";

constexpr uint32_t kImgRegionDiskCacheEntryVersion = 1;
constexpr char kImgRegionDiskCacheMagic[8] = {'A', 'T', 'L', 'S', 'I', 'R', 'C', '1'};

struct ImgRegionCompressionConfig
{
  folly::compression::CodecType codecType = folly::compression::CodecType::NO_COMPRESSION;
  int codecLevel = folly::compression::COMPRESSION_LEVEL_DEFAULT;
};

// Disk-cache entry compression policy for image-region payloads.
// Change this single constant to experiment with different codecs/levels.
constexpr ImgRegionCompressionConfig kImgRegionCompression{
  .codecType = folly::compression::CodecType::ZSTD,
  .codecLevel = folly::compression::COMPRESSION_LEVEL_DEFAULT,
};

using Blob = ZSqliteLRUCache::Blob;

constexpr char kImgRegionKeyTag[8] = {'I', 'M', 'G', 'R', 'E', 'G', 'N', '1'};

struct ImgRegionKeyBytes
{
  std::array<char, 8> tag{};
  std::array<std::uint8_t, 32> datasetFingerprint{};
  uint8_t sourceKind = 0;
  int64_t xyRatio = 0;
  int64_t zRatio = 0;
  int64_t sx = 0;
  int64_t sy = 0;
  int64_t sz = 0;
  uint64_t sc = 0;
  uint64_t t = 0;
  uint64_t w = 0;
  uint64_t h = 0;
  uint64_t d = 0;
  uint64_t numChannels = 0;
  uint64_t numTimes = 0;
  uint32_t bytesPerVoxel = 0;
  uint32_t voxelFormat = 0;
  uint32_t validBitCount = 0;
  double displayRangeMin = 0.0;
  double displayRangeMax = 0.0;
};

struct ImgRegionEntryHeader
{
  std::array<char, 8> magic{};
  uint32_t version = 0;
  uint32_t codecType = 0;   // folly::compression::CodecType (0 = none / raw)
  int32_t codecLevel = 0;   // informational (ignored on decompress)
  uint32_t reserved0 = 0;
  uint64_t w = 0;
  uint64_t h = 0;
  uint64_t d = 0;
  uint64_t numChannels = 0;
  uint64_t numTimes = 0;
  uint32_t bytesPerVoxel = 0;
  uint32_t voxelFormat = 0;
  uint32_t validBitCount = 0;
  uint64_t rawBytes = 0;      // uncompressed bytes
  uint64_t payloadBytes = 0;  // bytes stored after the header (compressed or raw)
};

[[nodiscard]] Blob sha256(const void* data, size_t bytes)
{
  boost::hash2::sha2_256 hash;
  if (bytes > 0) {
    CHECK(data != nullptr);
    hash.update(data, bytes);
  }
  const boost::hash2::sha2_256::result_type digest = hash.result();
  Blob out;
  out.resize(digest.size());
  CHECK(out.size() == digest.size());
  std::memcpy(out.data(), digest.data(), digest.size());
  return out;
}

[[nodiscard]] Blob sha256KeyHashFor(const ImageRegionCacheHashKeyType& key)
{
  if (std::endian::native != std::endian::little) {
    // Best-effort: disk cache disabled on unsupported platform endianness.
    return {};
  }

  const auto& fingerprint = std::get<0>(key);
  const ZImgRegionCacheSourceKind sourceKind = std::get<1>(key);
  if (sourceKind != ZImgRegionCacheSourceKind::File) {
    return {};
  }

  const index_t xyRatio = std::get<2>(key);
  const index_t zRatio = std::get<3>(key);
  const index_t sx = std::get<4>(key);
  const index_t sy = std::get<5>(key);
  const index_t sz = std::get<6>(key);
  const size_t sc = std::get<7>(key);
  const size_t t = std::get<8>(key);
  const size_t w = std::get<9>(key);
  const size_t h = std::get<10>(key);
  const size_t d = std::get<11>(key);
  const size_t numChannels = std::get<12>(key);
  const size_t numTimes = std::get<13>(key);
  const uint32_t bytesPerVoxel = std::get<14>(key);
  const uint32_t voxelFormatU = std::get<15>(key);
  const uint32_t validBitCount = std::get<16>(key);
  const double displayRangeMin = std::get<17>(key);
  const double displayRangeMax = std::get<18>(key);

  ImgRegionKeyBytes keyBytes{};
  std::memcpy(keyBytes.tag.data(), kImgRegionKeyTag, sizeof(kImgRegionKeyTag));
  keyBytes.datasetFingerprint = fingerprint;
  keyBytes.sourceKind = static_cast<uint8_t>(std::to_underlying(sourceKind));
  keyBytes.xyRatio = static_cast<int64_t>(xyRatio);
  keyBytes.zRatio = static_cast<int64_t>(zRatio);
  keyBytes.sx = static_cast<int64_t>(sx);
  keyBytes.sy = static_cast<int64_t>(sy);
  keyBytes.sz = static_cast<int64_t>(sz);
  keyBytes.sc = static_cast<uint64_t>(sc);
  keyBytes.t = static_cast<uint64_t>(t);
  keyBytes.w = static_cast<uint64_t>(w);
  keyBytes.h = static_cast<uint64_t>(h);
  keyBytes.d = static_cast<uint64_t>(d);
  keyBytes.numChannels = static_cast<uint64_t>(numChannels);
  keyBytes.numTimes = static_cast<uint64_t>(numTimes);
  keyBytes.bytesPerVoxel = bytesPerVoxel;
  keyBytes.voxelFormat = voxelFormatU;
  keyBytes.validBitCount = validBitCount;
  keyBytes.displayRangeMin = displayRangeMin;
  keyBytes.displayRangeMax = displayRangeMax;

  Blob compactKeyBytes;
  try {
    compactKeyBytes.resize(compactSize(keyBytes));
  } catch (...) {
    // Best-effort: treat allocation failures as "no disk cache".
    return {};
  }
  if (compactKeyBytes.empty()) {
    return {};
  }
  if (compactStructToMemory(compactKeyBytes.data(), compactKeyBytes.size(), keyBytes) != compactKeyBytes.size()) {
    return {};
  }

  return sha256(compactKeyBytes.data(), compactKeyBytes.size());
}

[[nodiscard]] Blob serializeImgRegionEntry(const ZImg& img)
{
  if (std::endian::native != std::endian::little) {
    return {};
  }

  const ZImgInfo& info = img.info();

  if (img.byteNumber() > std::numeric_limits<uint64_t>::max()) {
    return {};
  }

  ImgRegionEntryHeader hdr{};
  std::memcpy(hdr.magic.data(), kImgRegionDiskCacheMagic, sizeof(kImgRegionDiskCacheMagic));
  hdr.version = kImgRegionDiskCacheEntryVersion;
  hdr.w = static_cast<uint64_t>(info.width);
  hdr.h = static_cast<uint64_t>(info.height);
  hdr.d = static_cast<uint64_t>(info.depth);
  hdr.numChannels = static_cast<uint64_t>(info.numChannels);
  hdr.numTimes = static_cast<uint64_t>(info.numTimes);
  hdr.bytesPerVoxel = static_cast<uint32_t>(info.bytesPerVoxel);
  hdr.voxelFormat = static_cast<uint32_t>(std::to_underlying(info.voxelFormat));
  hdr.validBitCount = static_cast<uint32_t>(info.validBitCount);
  hdr.rawBytes = static_cast<uint64_t>(img.byteNumber());

  const size_t headerBytes = compactSize(hdr);
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
      codec = folly::compression::getStreamCodec(kImgRegionCompression.codecType, kImgRegionCompression.codecLevel);
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

      Blob compressed;
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
            ImgRegionEntryHeader compressedHdr = hdr;
            compressedHdr.codecType = static_cast<uint32_t>(kImgRegionCompression.codecType);
            compressedHdr.codecLevel = static_cast<int32_t>(kImgRegionCompression.codecLevel);
            compressedHdr.payloadBytes = static_cast<uint64_t>(compressed.size());

            Blob out;
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

  Blob out;
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

[[nodiscard]] std::shared_ptr<ZImg> parseImgRegionEntry(std::span<const std::uint8_t> bytes)
{
  if (std::endian::native != std::endian::little) {
    return std::shared_ptr<ZImg>();
  }

  const size_t headerBytes = compactSize(ImgRegionEntryHeader{});
  if (bytes.size() < headerBytes) {
    return std::shared_ptr<ZImg>();
  }

  ImgRegionEntryHeader hdr{};
  readStructFromCompactMemory(hdr, bytes.data(), headerBytes);

  if (std::memcmp(hdr.magic.data(), kImgRegionDiskCacheMagic, sizeof(kImgRegionDiskCacheMagic)) != 0) {
    return std::shared_ptr<ZImg>();
  }
  if (hdr.version != kImgRegionDiskCacheEntryVersion) {
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

class ImgRegionDiskBackend final : public ZParentImgRegionCache::DiskBackend
{
public:
  using DiskLookupResult = ZParentImgRegionCache::DiskLookupResult;

  ImgRegionDiskBackend(QString rootDir, uint64_t maxBytes)
    : m_rootDir(std::move(rootDir))
    , m_maxBytes(maxBytes)
  {
    m_rootDir = m_rootDir.trimmed();
    if (m_rootDir.isEmpty() || m_maxBytes == 0) {
      return;
    }

    m_bucket = std::make_unique<ZSqliteDiskCacheBucket>(m_rootDir,
                                                        QString::fromLatin1(kImgRegionDiskCacheDbName),
                                                        m_maxBytes,
                                                        FLAGS_atlas_disk_cache_imgregion_async_max_pending_bytes,
                                                        QStringLiteral("imgregion_disk_cache"));
    if (isEnabled()) {
      LOG(INFO) << "Image-region disk cache enabled: root='" << m_rootDir << "' maxBytes=" << m_maxBytes;
    }
  }

  ~ImgRegionDiskBackend() override = default;

  [[nodiscard]] bool isEnabled() const
  {
    return m_bucket && m_bucket->isEnabled();
  }

  [[nodiscard]] std::optional<DiskLookupResult> tryGet(const ImageRegionCacheHashKeyType& key) override
  {
    if (!isEnabled()) {
      return std::nullopt;
    }

    const Blob keyHash = sha256KeyHashFor(key);
    if (keyHash.size() != 32) {
      return std::nullopt;
    }

    auto getOpt = m_bucket->tryGetNoTouch(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()));
    if (!getOpt.has_value()) {
      return std::nullopt;
    }

    auto parsed = parseImgRegionEntry(std::span<const std::uint8_t>(getOpt->value.data(), getOpt->value.size()));
    if (!parsed) {
      // Corrupt entry; best-effort cleanup.
      m_bucket->tryEnqueueErase(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()));
      return std::nullopt;
    }

    const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    m_bucket->tryEnqueueTouchIfStale(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()),
                                     getOpt->lastAccessNs,
                                     nowNs,
                                     std::chrono::seconds(5));

    DiskLookupResult out;
    out.value = std::move(parsed);
    out.objSize = out.value->byteNumber();
    return out;
  }

  void put(const ImageRegionCacheHashKeyType& key, const std::shared_ptr<ZImg>& value, size_t objSize) override
  {
    CHECK(value);

    if (!isEnabled()) {
      return;
    }

    // Compute key hash outside the cache mutex to minimize contention with lookups.
    const Blob keyHash = sha256KeyHashFor(key);
    if (keyHash.size() != 32) {
      return;
    }

    const uint64_t headerBytes = static_cast<uint64_t>(compactSize(ImgRegionEntryHeader{}));
    if (static_cast<uint64_t>(objSize) > std::numeric_limits<uint64_t>::max() - headerBytes) {
      return;
    }
    const uint64_t estimatedBytes = headerBytes + static_cast<uint64_t>(objSize);

    m_bucket->tryEnqueuePutValueWithFactory(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()),
                                            estimatedBytes,
                                            [value]() -> Blob { return serializeImgRegionEntry(*value); });
  }

  void erase(const ImageRegionCacheHashKeyType& key) override
  {
    const Blob keyHash = sha256KeyHashFor(key);
    if (keyHash.size() != 32) {
      return;
    }
    if (!isEnabled()) {
      return;
    }
    m_bucket->tryEnqueueErase(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()));
  }

private:
  QString m_rootDir;

  uint64_t m_maxBytes = 0;
  std::unique_ptr<ZSqliteDiskCacheBucket> m_bucket;
};

} // namespace

ZImgRegionCache& ZImgRegionCache::instance()
{
  static ZImgRegionCache imgRegionCache(false);
  return imgRegionCache;
}

ZImgRegionCache::ZImgRegionCache(bool canSkipDestructor)
  : ZParentImgRegionCache(ZCpuInfo::instance().nPhysicalRAM * FLAGS_atlas_image_region_cache_memory_proportion,
                          ZCpuInfo::instance().nLogicalCores * 2,
                          canSkipDestructor)
{
  if (FLAGS_atlas_disk_cache_imgregion_max_bytes == 0) {
    return;
  }

  const QString rootDir = atlasDiskCacheRootDirFromFlags();
  setDiskBackend(std::make_unique<ImgRegionDiskBackend>(rootDir, FLAGS_atlas_disk_cache_imgregion_max_bytes));
}

} // namespace nim
