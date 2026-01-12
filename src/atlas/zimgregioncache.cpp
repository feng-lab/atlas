#include "zimgregioncache.h"

#include "zcpuinfo.h"
#include "zdiskcacheutils.h"
#include "zimgdiskcacheentry.h"
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

DEFINE_double(atlas_image_region_cache_memory_proportion,
              0.3,
              "Proportion of RAM that will be used for image region cache, default is 0.3");

DECLARE_uint64(atlas_disk_cache_imgregion_max_bytes);
DECLARE_uint64(atlas_disk_cache_imgregion_async_max_pending_bytes);

namespace nim {

namespace {

constexpr char kImgRegionDiskCacheDbName[] = "imgregion.sqlite";

constexpr uint32_t kImgRegionDiskCacheEntryVersion = 1;
constexpr std::array<char, 8> kImgRegionDiskCacheMagic = {'A', 'T', 'L', 'S', 'I', 'R', 'C', '1'};

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

[[nodiscard]] Blob serializeImgRegionEntry(const ZImg& img)
{
  return serializeZImgDiskCacheEntry(img, kImgRegionDiskCacheMagic, kImgRegionDiskCacheEntryVersion);
}

[[nodiscard]] std::shared_ptr<ZImg> parseImgRegionEntry(std::span<const std::uint8_t> bytes)
{
  return parseZImgDiskCacheEntry(bytes, kImgRegionDiskCacheMagic, kImgRegionDiskCacheEntryVersion);
}

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

    const uint64_t headerBytes = static_cast<uint64_t>(zimgDiskCacheEntryHeaderBytes());
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
