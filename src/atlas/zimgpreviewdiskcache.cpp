#include "zimgpreviewdiskcache.h"

#include "zdiskcacheutils.h"
#include "zimg.h"
#include "zimgdiskcacheentry.h"
#include "zlog.h"
#include "zsqlitediskcachebucket.h"
#include "zstructutils.h"

#include "zcommandlineflags.h"

#include <boost/hash2/sha2.hpp>

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>

ABSL_DECLARE_FLAG(uint64_t, atlas_disk_cache_imgpreview_max_bytes);
ABSL_DECLARE_FLAG(uint64_t, atlas_disk_cache_imgpreview_async_max_pending_bytes);

namespace nim {

namespace {

using Blob = ZSqliteLRUCache::Blob;

constexpr char kImgPreviewDiskCacheDbName[] = "imgpreview.sqlite";

constexpr uint32_t kImgPreviewDiskCacheEntryVersion = 1;
constexpr std::array<char, 8> kImgPreviewDiskCacheMagic = {'A', 'T', 'L', 'S', 'P', 'V', 'C', '1'};

constexpr std::array<char, 8> kImgPreviewKeyTag = {'I', 'M', 'G', 'P', 'R', 'E', 'V', '1'};

struct ImgPreviewKeyBytes
{
  std::array<char, 8> tag{};
  std::array<std::uint8_t, 32> datasetFingerprint{};
  uint64_t t = 0;
  uint64_t w = 0;
  uint64_t h = 0;
  uint64_t d = 0;
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

[[nodiscard]] Blob sha256KeyHashForFilePreview(const std::array<std::uint8_t, 32>& datasetFingerprint,
                                               size_t width,
                                               size_t height,
                                               size_t depth,
                                               size_t t)
{
  if (std::endian::native != std::endian::little) {
    // Best-effort: disk cache disabled on unsupported platform endianness.
    return {};
  }

  ImgPreviewKeyBytes keyBytes{};
  keyBytes.tag = kImgPreviewKeyTag;
  keyBytes.datasetFingerprint = datasetFingerprint;
  keyBytes.t = static_cast<uint64_t>(t);
  keyBytes.w = static_cast<uint64_t>(width);
  keyBytes.h = static_cast<uint64_t>(height);
  keyBytes.d = static_cast<uint64_t>(depth);

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

[[nodiscard]] int64_t nowSystemClockNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
    .count();
}

} // namespace

struct ZImgPreviewDiskCache::Impl
{
  QString rootDir;
  uint64_t maxBytes = 0;

  std::unique_ptr<ZSqliteDiskCacheBucket> bucket;

  [[nodiscard]] bool isEnabled() const
  {
    return bucket && bucket->isEnabled();
  }
};

ZImgPreviewDiskCache& ZImgPreviewDiskCache::instance()
{
  static ZImgPreviewDiskCache cache;
  return cache;
}

ZImgPreviewDiskCache::ZImgPreviewDiskCache()
  : m_impl(std::make_unique<Impl>())
{
  const uint64_t maxBytes = absl::GetFlag(FLAGS_atlas_disk_cache_imgpreview_max_bytes);
  if (maxBytes == 0) {
    return;
  }

  QString rootDir = atlasDiskCacheRootDirFromFlags();
  rootDir = rootDir.trimmed();
  if (rootDir.isEmpty()) {
    return;
  }

  auto bucket =
    std::make_unique<ZSqliteDiskCacheBucket>(rootDir,
                                             QString::fromLatin1(kImgPreviewDiskCacheDbName),
                                             maxBytes,
                                             absl::GetFlag(FLAGS_atlas_disk_cache_imgpreview_async_max_pending_bytes),
                                             QStringLiteral("imgpreview_disk_cache"));
  if (!bucket || !bucket->isEnabled()) {
    return;
  }

  m_impl->rootDir = std::move(rootDir);
  m_impl->maxBytes = maxBytes;
  m_impl->bucket = std::move(bucket);

  LOG(INFO) << "Image-preview disk cache enabled: root='" << m_impl->rootDir << "' maxBytes=" << m_impl->maxBytes
            << " dbPath=" << m_impl->bucket->dbPath();
}

ZImgPreviewDiskCache::~ZImgPreviewDiskCache() = default;

std::shared_ptr<ZImg> ZImgPreviewDiskCache::tryGetFilePreview(const std::array<std::uint8_t, 32>& datasetFingerprint,
                                                              size_t width,
                                                              size_t height,
                                                              size_t depth,
                                                              size_t t) const
{
  if (!m_impl || !m_impl->isEnabled()) {
    return std::shared_ptr<ZImg>();
  }

  const Blob keyHash = sha256KeyHashForFilePreview(datasetFingerprint, width, height, depth, t);
  if (keyHash.size() != 32) {
    return std::shared_ptr<ZImg>();
  }

  auto getOpt = m_impl->bucket->tryGetNoTouch(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()));
  if (!getOpt.has_value()) {
    return std::shared_ptr<ZImg>();
  }

  auto parsed = parseZImgDiskCacheEntry(std::span<const std::uint8_t>(getOpt->value.data(), getOpt->value.size()),
                                        kImgPreviewDiskCacheMagic,
                                        kImgPreviewDiskCacheEntryVersion);
  if (!parsed) {
    // Corrupt entry; best-effort cleanup.
    m_impl->bucket->tryEnqueueErase(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()));
    return std::shared_ptr<ZImg>();
  }

  const int64_t nowNs = nowSystemClockNs();
  m_impl->bucket->tryEnqueueTouchIfStale(std::span<const std::uint8_t>(keyHash.data(), keyHash.size()),
                                         getOpt->lastAccessNs,
                                         nowNs,
                                         atlasDiskCacheTouchMinInterval());

  return parsed;
}

void ZImgPreviewDiskCache::tryPutFilePreview(const std::array<std::uint8_t, 32>& datasetFingerprint,
                                             size_t width,
                                             size_t height,
                                             size_t depth,
                                             size_t t,
                                             std::shared_ptr<const ZImg> img)
{
  CHECK(img);

  if (!m_impl || !m_impl->isEnabled()) {
    return;
  }

  const Blob keyHash = sha256KeyHashForFilePreview(datasetFingerprint, width, height, depth, t);
  if (keyHash.size() != 32) {
    return;
  }

  const uint64_t headerBytes = static_cast<uint64_t>(zimgDiskCacheEntryHeaderBytes());
  if (img->byteNumber() > std::numeric_limits<uint64_t>::max() - headerBytes) {
    return;
  }
  const uint64_t estimatedBytes = headerBytes + static_cast<uint64_t>(img->byteNumber());

  // Best-effort async write; can drop under memory pressure (bounded by a configurable pending-bytes budget).
  m_impl->bucket->tryEnqueuePutValueWithFactory(
    std::span<const std::uint8_t>(keyHash.data(), keyHash.size()),
    estimatedBytes,
    [img = std::move(img)]() -> Blob {
      try {
        return serializeZImgDiskCacheEntry(*img, kImgPreviewDiskCacheMagic, kImgPreviewDiskCacheEntryVersion);
      } catch (...) {
        return {};
      }
    });
}

} // namespace nim
