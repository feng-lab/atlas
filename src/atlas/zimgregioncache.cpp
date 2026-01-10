#include "zimgregioncache.h"

#include "zcpuinfo.h"
#include "zdiskcacheutils.h"
#include "zlog.h"
#include "zsqlitelrucache.h"
#include "zstructutils.h"

#include <gflags/gflags.h>

#include <QDir>
#include <QLockFile>

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

namespace nim {

namespace {

constexpr char kImgRegionDiskCacheDbName[] = "imgregion.sqlite";
constexpr char kImgRegionDiskCacheLockName[] = "imgregion.lock";

constexpr uint32_t kImgRegionDiskCacheEntryVersion = 1;
constexpr char kImgRegionDiskCacheMagic[8] = {'A', 'T', 'L', 'S', 'I', 'R', 'C', '1'};

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
  uint64_t w = 0;
  uint64_t h = 0;
  uint64_t d = 0;
  uint64_t numChannels = 0;
  uint64_t numTimes = 0;
  uint32_t bytesPerVoxel = 0;
  uint32_t voxelFormat = 0;
  uint32_t validBitCount = 0;
  uint64_t totalBytes = 0;
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
  CHECK(std::endian::native == std::endian::little);

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
  compactKeyBytes.resize(compactSize(keyBytes));
  CHECK(compactStructToMemory(compactKeyBytes.data(), compactKeyBytes.size(), keyBytes) == compactKeyBytes.size());

  return sha256(compactKeyBytes.data(), compactKeyBytes.size());
}

[[nodiscard]] Blob serializeImgRegionEntry(const ZImg& img)
{
  CHECK(std::endian::native == std::endian::little);

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
  hdr.totalBytes = static_cast<uint64_t>(img.byteNumber());

  const size_t headerBytes = compactSize(hdr);
  const uint64_t payloadBytes = hdr.totalBytes;
  if (payloadBytes > std::numeric_limits<size_t>::max()) {
    return {};
  }
  if (headerBytes > std::numeric_limits<size_t>::max() - static_cast<size_t>(payloadBytes)) {
    return {};
  }

  Blob out;
  out.resize(headerBytes + static_cast<size_t>(payloadBytes));
  CHECK(compactStructToMemory(out.data(), headerBytes, hdr) == headerBytes);
  if (payloadBytes > 0) {
    const size_t timeBytes = img.timeByteNumber();
    CHECK(timeBytes > 0);
    CHECK(static_cast<uint64_t>(timeBytes) * static_cast<uint64_t>(img.numTimes()) == payloadBytes);

    size_t offset = headerBytes;
    for (size_t ti = 0; ti < img.numTimes(); ++ti) {
      const uint8_t* src = img.timeData<uint8_t>(ti);
      CHECK(src != nullptr);
      std::memcpy(out.data() + offset, src, timeBytes);
      offset += timeBytes;
    }
    CHECK(offset == out.size());
  }
  return out;
}

[[nodiscard]] std::shared_ptr<ZImg> parseImgRegionEntry(std::span<const std::uint8_t> bytes)
{
  CHECK(std::endian::native == std::endian::little);

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
    CHECK(out != nullptr);
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
  if (expectedBytes != hdr.totalBytes) {
    return std::shared_ptr<ZImg>();
  }

  if (hdr.totalBytes > std::numeric_limits<size_t>::max()) {
    return std::shared_ptr<ZImg>();
  }
  const size_t totalBytes = static_cast<size_t>(hdr.totalBytes);
  if (headerBytes > bytes.size() - totalBytes) {
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

  auto img = std::make_shared<ZImg>(info);
  const uint8_t* src = bytes.data() + headerBytes;
  if (totalBytes > 0) {
    const size_t timeBytes = img->timeByteNumber();
    CHECK(timeBytes > 0);
    CHECK(timeBytes * img->numTimes() == totalBytes);

    size_t offset = 0;
    for (size_t ti = 0; ti < img->numTimes(); ++ti) {
      uint8_t* dstTime = img->timeData<uint8_t>(ti);
      CHECK(dstTime != nullptr);
      std::memcpy(dstTime, src + offset, timeBytes);
      offset += timeBytes;
    }
    CHECK(offset == totalBytes);
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

    m_cacheDir = atlasDiskCacheDirFromRoot(m_rootDir);
    m_dbPath = QDir(m_cacheDir).filePath(QString::fromLatin1(kImgRegionDiskCacheDbName));
    m_lockPath = QDir(m_cacheDir).filePath(QString::fromLatin1(kImgRegionDiskCacheLockName));

    {
      QDir mk;
      if (!mk.mkpath(m_cacheDir)) {
        LOG(WARNING) << "Image-region disk cache disabled: failed to create directory " << m_cacheDir;
        return;
      }
    }

    // Single-writer guard across processes (size accounting/pruning is best-effort but simpler with one writer).
    m_lock = std::make_unique<QLockFile>(m_lockPath);
    m_lock->setStaleLockTime(static_cast<int>(std::chrono::milliseconds(std::chrono::seconds(10)).count()));
    if (!m_lock->tryLock(/*timeout=*/0)) {
      VLOG(1) << "Image-region disk cache disabled: could not acquire lock at " << m_lockPath;
      m_lock.reset();
      return;
    }

    m_cache = std::make_unique<ZSqliteLRUCache>(m_dbPath, m_maxBytes);
    if (!m_cache->isOpen()) {
      LOG(WARNING) << "Image-region disk cache disabled: failed to open SQLite DB at " << m_dbPath;
      m_cache.reset();
      m_lock.reset();
      return;
    }

    m_enabled = true;
    LOG(INFO) << "Image-region disk cache enabled: root='" << m_rootDir << "' maxBytes=" << m_maxBytes;
  }

  [[nodiscard]] std::optional<DiskLookupResult> tryGet(const ImageRegionCacheHashKeyType& key) override
  {
    std::lock_guard<std::mutex> g(m_mu);
    if (!m_enabled || !m_cache) {
      return std::nullopt;
    }
    if (!m_cache->isOpen()) {
      m_cache.reset();
      m_lock.reset();
      m_enabled = false;
      return std::nullopt;
    }

    const Blob keyHash = sha256KeyHashFor(key);
    if (keyHash.empty()) {
      return std::nullopt;
    }

    auto valueOpt = m_cache->tryGet(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()));
    if (!valueOpt.has_value()) {
      return std::nullopt;
    }

    auto parsed = parseImgRegionEntry(std::span<const std::uint8_t>(valueOpt->data(), valueOpt->size()));
    if (!parsed) {
      // Corrupt entry; best-effort cleanup.
      m_cache->erase(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()));
      return std::nullopt;
    }

    DiskLookupResult out;
    out.value = std::move(parsed);
    out.objSize = out.value->byteNumber();
    return out;
  }

  void put(const ImageRegionCacheHashKeyType& key, const std::shared_ptr<ZImg>& value, size_t objSize) override
  {
    CHECK(value);

    std::lock_guard<std::mutex> g(m_mu);
    if (!m_enabled || !m_cache) {
      return;
    }
    if (!m_cache->isOpen()) {
      m_cache.reset();
      m_lock.reset();
      m_enabled = false;
      return;
    }
    if (static_cast<uint64_t>(objSize) > m_maxBytes) {
      // An entry larger than the entire budget would cause immediate thrash; skip caching.
      return;
    }

    const Blob keyHash = sha256KeyHashFor(key);
    if (keyHash.empty()) {
      return;
    }

    const Blob entryBytes = serializeImgRegionEntry(*value);
    if (entryBytes.empty()) {
      return;
    }

    m_cache->put(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()),
                 std::span<const std::uint8_t>(entryBytes.data(), entryBytes.size()));
  }

  void erase(const ImageRegionCacheHashKeyType& key) override
  {
    std::lock_guard<std::mutex> g(m_mu);
    if (!m_enabled || !m_cache) {
      return;
    }
    if (!m_cache->isOpen()) {
      m_cache.reset();
      m_lock.reset();
      m_enabled = false;
      return;
    }
    const Blob keyHash = sha256KeyHashFor(key);
    if (keyHash.empty()) {
      return;
    }
    m_cache->erase(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()));
  }

private:
  QString m_rootDir;
  QString m_cacheDir;
  QString m_dbPath;
  QString m_lockPath;

  uint64_t m_maxBytes = 0;
  bool m_enabled = false;

  std::mutex m_mu;
  std::unique_ptr<QLockFile> m_lock;
  std::unique_ptr<ZSqliteLRUCache> m_cache;
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
