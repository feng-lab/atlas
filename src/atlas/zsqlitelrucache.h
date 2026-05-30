#pragma once

#include <QString>

#include <cstdint>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace nim {

// SQLite-backed key/value cache with best-effort LRU eviction.
//
// Design goals:
// - Fast point lookups by key hash.
// - Bounded disk usage via LRU-ish eviction (oldest last-access time first).
// - Best-effort behavior: any I/O/DB failure degrades to cache-miss semantics.
class ZSqliteLRUCache
{
public:
  using Blob = std::vector<std::uint8_t>;
  using NowNsFn = std::function<int64_t()>;

  struct GetNoTouchResult
  {
    Blob value;
    int64_t lastAccessNs = 0;
  };

  ZSqliteLRUCache(QString dbPath, uint64_t maxBytes, NowNsFn nowNsFn = nullptr, bool readOnly = false);
  ~ZSqliteLRUCache();

  ZSqliteLRUCache(const ZSqliteLRUCache&) = delete;
  ZSqliteLRUCache& operator=(const ZSqliteLRUCache&) = delete;

  [[nodiscard]] bool isOpen() const;

  [[nodiscard]] QString dbPath() const;

  [[nodiscard]] uint64_t maxBytes() const;

  // Updates the max byte budget. Does not immediately prune unless the new budget is exceeded.
  void setMaxBytes(uint64_t maxBytes);

  // Returns the cached value if present.
  [[nodiscard]] std::optional<Blob> tryGet(std::span<const std::uint8_t> keyHash);

  // Returns the cached value if present, without mutating the DB (no access-time touch).
  // Callers that want LRU-ish behavior should schedule touch() out-of-band.
  [[nodiscard]] std::optional<GetNoTouchResult> tryGetNoTouch(std::span<const std::uint8_t> keyHash);

  // Inserts or replaces an entry.
  void put(std::span<const std::uint8_t> keyHash, std::span<const std::uint8_t> value);

  // Best-effort access-time update. No-op on failure.
  void touch(std::span<const std::uint8_t> keyHash, int64_t nowNs);

  // Removes an entry if present.
  void erase(std::span<const std::uint8_t> keyHash);

private:
  void closeDbLocked();

  [[nodiscard]] bool initDbLocked();
  [[nodiscard]] bool ensureSchemaLocked();

  // Tracks persistent SQLite failures and disables the cache (by closing the DB)
  // after repeated errors. This avoids repeatedly paying I/O/SQL overhead when
  // the filesystem/DB is unhealthy (disk full, corruption, I/O errors, etc.).
  void recordSqlErrorLocked(int rc, std::string_view context);
  void recordSqlSuccessLocked();

  [[nodiscard]] int64_t nowNsLocked() const;

  void maybePruneLocked(int64_t nowNs);

  [[nodiscard]] bool execLocked(std::string_view sql);

  [[nodiscard]] bool prepareLocked(std::string_view sql, sqlite3_stmt** outStmt);

private:
  QString m_dbPath;
  uint64_t m_maxBytes = 0;
  bool m_readOnly = false;

  NowNsFn m_nowNsFn;

  sqlite3* m_db = nullptr;

  sqlite3_stmt* m_stmtSelectValue = nullptr;
  sqlite3_stmt* m_stmtTouch = nullptr;
  sqlite3_stmt* m_stmtUpsert = nullptr;
  sqlite3_stmt* m_stmtDelete = nullptr;
  sqlite3_stmt* m_stmtPruneSelect = nullptr;
  sqlite3_stmt* m_stmtSelectTotalBytes = nullptr;

  std::chrono::steady_clock::time_point m_lastPrune{};
  int m_consecutiveSqlFailures = 0;
  bool m_disabled = false;
  mutable std::mutex m_mu;
};

} // namespace nim
