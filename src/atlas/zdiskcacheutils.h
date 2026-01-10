#pragma once

#include <QString>

namespace nim {

inline constexpr char kAtlasDiskCacheDirName[] = "atlas_disk_cache_v1";

// Returns the root directory for Atlas' persistent disk caches based on flags and
// system defaults. The returned path is NOT the bucket directory; use
// atlasDiskCacheDirFromRoot()/atlasDiskCacheDirFromFlags() for the v1 cache folder.
[[nodiscard]] QString atlasDiskCacheRootDirFromFlags();

// Returns "<root>/<atlas_disk_cache_v1>" (or empty if root is empty).
[[nodiscard]] QString atlasDiskCacheDirFromRoot(const QString& rootDir);

// Convenience wrapper: atlasDiskCacheDirFromRoot(atlasDiskCacheRootDirFromFlags()).
[[nodiscard]] QString atlasDiskCacheDirFromFlags();

} // namespace nim

