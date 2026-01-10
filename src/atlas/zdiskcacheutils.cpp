#include "zdiskcacheutils.h"

#include "zsysteminfo.h"

#include <gflags/gflags.h>

#include <QDir>

#include <cstdint>

namespace {

constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;

constexpr uint64_t kDefaultHttpDiskCacheMaxBytes = 10ULL * kGiB;
constexpr uint64_t kDefaultImgRegionDiskCacheMaxBytes = 20ULL * kGiB;

} // namespace

DEFINE_string(atlas_disk_cache_dir,
              "",
              "Root directory for the persistent Atlas disk cache (default: auto-chosen Atlas cache directory).");

DEFINE_uint64(atlas_disk_cache_http_max_bytes,
              kDefaultHttpDiskCacheMaxBytes,
              "Maximum size in bytes for the persistent HTTP disk cache (0 disables; default 10 GiB).");

DEFINE_uint64(atlas_disk_cache_imgregion_max_bytes,
              kDefaultImgRegionDiskCacheMaxBytes,
              "Maximum size in bytes for the persistent image-region disk cache (0 disables; default 20 GiB).");

namespace nim {

QString atlasDiskCacheRootDirFromFlags()
{
  if (!FLAGS_atlas_disk_cache_dir.empty()) {
    return QString::fromStdString(FLAGS_atlas_disk_cache_dir).trimmed();
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

} // namespace nim
