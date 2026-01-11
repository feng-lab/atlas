#pragma once

#include <QString>

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>

#include <folly/ThreadLocal.h>

#include "zsqlitelrucache.h"

namespace nim {

// ZSqliteReadOnlyPool provides a per-thread, read-only SQLite connection pool.
//
// Motivation:
// - Disk-cache reads are synchronous, but must not serialize across threads.
// - SQLite connections (and prepared statements) are not safely shareable without
//   locks; using one connection per thread allows concurrent cache hits without
//   blocking unrelated readers.
//
// Best-effort behavior:
// - If the DB cannot be opened (permissions, missing disk, etc.), the pool
//   disables itself and all calls become cache misses.
class ZSqliteReadOnlyPool
{
public:
  ZSqliteReadOnlyPool(QString dbPath, uint64_t maxBytes, QString debugName);
  ~ZSqliteReadOnlyPool();

  ZSqliteReadOnlyPool(const ZSqliteReadOnlyPool&) = delete;
  ZSqliteReadOnlyPool& operator=(const ZSqliteReadOnlyPool&) = delete;

  [[nodiscard]] bool isEnabled() const;

  [[nodiscard]] std::optional<ZSqliteLRUCache::GetNoTouchResult> tryGetNoTouch(std::span<const std::uint8_t> keyHash);

private:
  [[nodiscard]] ZSqliteLRUCache* getOrCreateCacheForThisThread();

  void disable();

private:
  QString m_dbPath;
  uint64_t m_maxBytes = 0;
  QString m_debugName;

  std::atomic<bool> m_enabled{false};
  folly::ThreadLocalPtr<ZSqliteLRUCache> m_tlsCache;
};

} // namespace nim
