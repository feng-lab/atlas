#pragma once

#include <QString>

#include <chrono>
#include <cstdint>

namespace nim {

inline constexpr char kAtlasDiskCacheDirName[] = "atlas_disk_cache_v1";

// Disk cache SQLite writes are async (best-effort). To avoid tiny queues that
// immediately thrash/dropped writes, we clamp per-bucket async budgets to a
// minimum. This is intentionally conservative: Atlas disk caches are only
// useful when they can absorb bursts without blocking critical threads.
inline constexpr uint64_t kAtlasDiskCacheAsyncMinPendingBytes = 256ULL * 1024ULL * 1024ULL; // 256 MiB

// Returns the root directory for Atlas' persistent disk caches based on flags and
// system defaults. The returned path is NOT the bucket directory; use
// atlasDiskCacheDirFromRoot()/atlasDiskCacheDirFromFlags() for the v1 cache folder.
[[nodiscard]] QString atlasDiskCacheRootDirFromFlags();

// Returns "<root>/<atlas_disk_cache_v1>" (or empty if root is empty).
[[nodiscard]] QString atlasDiskCacheDirFromRoot(const QString& rootDir);

// Convenience wrapper: atlasDiskCacheDirFromRoot(atlasDiskCacheRootDirFromFlags()).
[[nodiscard]] QString atlasDiskCacheDirFromFlags();

// Minimum wall-clock interval between persistent LRU access-time touches for a
// single key. The interval is shared by all SQLite-backed Atlas disk caches.
[[nodiscard]] std::chrono::seconds atlasDiskCacheTouchMinInterval();

} // namespace nim
