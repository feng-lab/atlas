#include "zdiskcacheutils.h"

#include "zsysteminfo.h"

#include "zcommandlineflags.h"

#include <QDir>

#include <algorithm>
#include <cstdint>

namespace {

constexpr uint64_t kKiB = 1024ULL;
constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMiB = 1024ULL * 1024ULL;

constexpr uint64_t kDefaultHttpDiskCacheMaxBytes = 20ULL * kGiB;
constexpr uint64_t kDefaultImgRegionDiskCacheMaxBytes = 20ULL * kGiB;
constexpr uint64_t kDefaultImgPreviewDiskCacheMaxBytes = 5ULL * kGiB;
constexpr uint64_t kDefaultDiskCacheSqliteReaderCacheBytes = 32ULL * kMiB;
constexpr uint64_t kDefaultDiskCacheSqliteWriterCacheBytes = 16ULL * kMiB;
constexpr uint64_t kDefaultDiskCacheSqliteMmapBytes = 1024ULL * kMiB;
constexpr int64_t kDefaultDiskCacheSqliteJournalSizeLimitBytes = 256LL * static_cast<int64_t>(kMiB);
constexpr int32_t kDefaultDiskCacheSqliteTouchMinIntervalSeconds = 60;
constexpr uint64_t kDefaultDiskCacheSqlitePageSize = 16ULL * kKiB;

// Async write queue limits:
// - Writes are best-effort and may be dropped if the queue is full.
// - Keep defaults conservative to avoid unbounded memory growth when disk I/O is slow.
constexpr uint64_t kDefaultHttpDiskCacheAsyncMaxPendingBytes = 256ULL * kMiB;
constexpr uint64_t kDefaultImgRegionDiskCacheAsyncMaxPendingBytes = 256ULL * kMiB;
constexpr uint64_t kDefaultImgPreviewDiskCacheAsyncMaxPendingBytes = 1024ULL * kMiB;

} // namespace

ABSL_FLAG(std::string,
          atlas_disk_cache_dir,
          "",
          "Root directory for the persistent Atlas disk cache (default: auto-chosen Atlas cache directory).");

ABSL_FLAG(uint64_t,
          atlas_disk_cache_http_max_bytes,
          kDefaultHttpDiskCacheMaxBytes,
          "Maximum size in bytes for the persistent HTTP disk cache (0 disables; default 20 GiB).");

ABSL_FLAG(uint64_t,
          atlas_disk_cache_http_async_max_pending_bytes,
          kDefaultHttpDiskCacheAsyncMaxPendingBytes,
          "Maximum queued bytes for async HTTP disk-cache SQLite writes (min 256 MiB; default 256 MiB).");

ABSL_FLAG(uint64_t,
          atlas_disk_cache_imgregion_max_bytes,
          kDefaultImgRegionDiskCacheMaxBytes,
          "Maximum size in bytes for the persistent image-region disk cache (0 disables; default 20 GiB).");

ABSL_FLAG(uint64_t,
          atlas_disk_cache_imgregion_async_max_pending_bytes,
          kDefaultImgRegionDiskCacheAsyncMaxPendingBytes,
          "Maximum queued bytes for async image-region disk-cache SQLite writes (min 256 MiB; default 256 MiB).");

ABSL_FLAG(uint64_t,
          atlas_disk_cache_imgpreview_max_bytes,
          kDefaultImgPreviewDiskCacheMaxBytes,
          "Maximum size in bytes for the persistent image-preview disk cache (0 disables; default 5 GiB).");

ABSL_FLAG(uint64_t,
          atlas_disk_cache_imgpreview_async_max_pending_bytes,
          kDefaultImgPreviewDiskCacheAsyncMaxPendingBytes,
          "Maximum queued bytes for async image-preview disk-cache SQLite writes (min 256 MiB; default 1024 MiB).");

ABSL_FLAG(uint64_t,
          atlas_disk_cache_sqlite_reader_cache_bytes,
          kDefaultDiskCacheSqliteReaderCacheBytes,
          "Approximate SQLite pager-cache budget in bytes for each read-only Atlas disk-cache connection "
          "(0 leaves SQLite's default).");

ABSL_FLAG(uint64_t,
          atlas_disk_cache_sqlite_writer_cache_bytes,
          kDefaultDiskCacheSqliteWriterCacheBytes,
          "Approximate SQLite pager-cache budget in bytes for each writer Atlas disk-cache connection "
          "(0 leaves SQLite's default).");

ABSL_FLAG(uint64_t,
          atlas_disk_cache_sqlite_mmap_bytes,
          kDefaultDiskCacheSqliteMmapBytes,
          "Maximum SQLite memory-mapped I/O window in bytes for Atlas disk-cache connections (0 disables mmap).");

ABSL_FLAG(int64_t,
          atlas_disk_cache_sqlite_journal_size_limit_bytes,
          kDefaultDiskCacheSqliteJournalSizeLimitBytes,
          "SQLite WAL journal size limit in bytes for Atlas disk-cache writer connections (-1 leaves SQLite's "
          "default).");

ABSL_FLAG(int32_t,
          atlas_disk_cache_sqlite_touch_min_interval_seconds,
          kDefaultDiskCacheSqliteTouchMinIntervalSeconds,
          "Minimum wall-clock interval between persistent LRU touch-on-read updates for Atlas SQLite disk caches.");

ABSL_FLAG(uint64_t,
          atlas_disk_cache_sqlite_page_size,
          kDefaultDiskCacheSqlitePageSize,
          "SQLite page size in bytes for newly created Atlas disk-cache databases (0 leaves SQLite's default; "
          "existing DBs keep their current page size until deleted/recreated).");

namespace nim {

QString atlasDiskCacheRootDirFromFlags()
{
  if (!absl::GetFlag(FLAGS_atlas_disk_cache_dir).empty()) {
    return QString::fromStdString(absl::GetFlag(FLAGS_atlas_disk_cache_dir)).trimmed();
  }

  // Prefer the Atlas image cache root (may select a non-system volume with enough space).
  QString root = ZSystemInfo::imgCachePath(/*requiredSpaceInBytes=*/0);
  if (!root.isEmpty()) {
    return root;
  }

  // Fall back to the app-local config/data directory.
  return ZSystemInfo::configDir().absolutePath();
}

QString atlasDiskCacheDirFromRoot(const QString& rootDir)
{
  const QString root = rootDir.trimmed();
  if (root.isEmpty()) {
    return {};
  }
  return QDir(root).filePath(QString::fromLatin1(kAtlasDiskCacheDirName));
}

QString atlasDiskCacheDirFromFlags()
{
  return atlasDiskCacheDirFromRoot(atlasDiskCacheRootDirFromFlags());
}

std::chrono::seconds atlasDiskCacheTouchMinInterval()
{
  return std::chrono::seconds(std::max(0, absl::GetFlag(FLAGS_atlas_disk_cache_sqlite_touch_min_interval_seconds)));
}

} // namespace nim
