#include "zsqlitelrucache.h"

#include "zlog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <vtksqlite/sqlite3.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

namespace nim {

namespace {

constexpr int kCacheSchemaVersion = 1;
constexpr std::chrono::seconds kPruneMinInterval{30};
constexpr double kPruneTargetFraction = 0.9;
constexpr int kDisableAfterConsecutiveFailures = 8;
constexpr std::chrono::seconds kTouchMinInterval{5};

constexpr std::string_view kMetaKeyTotalBytes = "total_bytes";

[[nodiscard]] int64_t defaultNowNs()
{
  using namespace std::chrono;
  return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] bool stepExpectDone(sqlite3_stmt* stmt)
{
  const int rc = sqlite3_step(stmt);
  return rc == SQLITE_DONE;
}

void resetStmt(sqlite3_stmt* stmt)
{
  if (!stmt) {
    return;
  }
  (void)sqlite3_reset(stmt);
  (void)sqlite3_clear_bindings(stmt);
}

} // namespace

ZSqliteLRUCache::ZSqliteLRUCache(QString dbPath, uint64_t maxBytes, NowNsFn nowNsFn, bool readOnly)
  : m_dbPath(std::move(dbPath))
  , m_maxBytes(maxBytes)
  , m_readOnly(readOnly)
  , m_nowNsFn(std::move(nowNsFn))
{
  if (!m_nowNsFn) {
    m_nowNsFn = &defaultNowNs;
  }

  std::lock_guard<std::mutex> lock(m_mu);
  (void)initDbLocked();
}

ZSqliteLRUCache::~ZSqliteLRUCache()
{
  std::lock_guard<std::mutex> lock(m_mu);
  closeDbLocked();
}

void ZSqliteLRUCache::recordSqlErrorLocked(int rc, const char* context)
{
  if (m_disabled) {
    return;
  }

  // Multi-process disk caches expect transient contention on the writer lock.
  // Treat BUSY/LOCKED as best-effort cache misses / dropped writes, not fatal errors.
  const int primaryRc = (rc & 0xFF);
  if (primaryRc == SQLITE_BUSY || primaryRc == SQLITE_LOCKED) {
    return;
  }

  ++m_consecutiveSqlFailures;
  if (m_consecutiveSqlFailures < kDisableAfterConsecutiveFailures) {
    return;
  }

  LOG(WARNING) << "SQLite cache disabled after " << m_consecutiveSqlFailures
               << " consecutive failures: dbPath=" << m_dbPath
               << " lastRc=" << rc
               << " context=" << (context ? context : "<null>");

  m_disabled = true;
  closeDbLocked();
}

void ZSqliteLRUCache::recordSqlSuccessLocked()
{
  m_consecutiveSqlFailures = 0;
}

bool ZSqliteLRUCache::isOpen() const
{
  std::lock_guard<std::mutex> lock(m_mu);
  return m_db != nullptr;
}

QString ZSqliteLRUCache::dbPath() const
{
  std::lock_guard<std::mutex> lock(m_mu);
  return m_dbPath;
}

uint64_t ZSqliteLRUCache::maxBytes() const
{
  std::lock_guard<std::mutex> lock(m_mu);
  return m_maxBytes;
}

void ZSqliteLRUCache::setMaxBytes(uint64_t maxBytes)
{
  std::lock_guard<std::mutex> lock(m_mu);
  m_maxBytes = maxBytes;
  if (m_db) {
    maybePruneLocked(nowNsLocked());
  }
}

std::optional<ZSqliteLRUCache::Blob> ZSqliteLRUCache::tryGet(std::span<const std::uint8_t> keyHash)
{
  std::lock_guard<std::mutex> lock(m_mu);
  CHECK(!m_readOnly);
  if (m_disabled || !m_db || !m_stmtSelectValue || !m_stmtTouch) {
    return std::nullopt;
  }
  if (keyHash.empty()) {
    return std::nullopt;
  }
  if (keyHash.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }

  resetStmt(m_stmtSelectValue);
  if (sqlite3_bind_blob(m_stmtSelectValue,
                        /*index=*/1,
                        keyHash.data(),
                        static_cast<int>(keyHash.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    recordSqlErrorLocked(sqlite3_errcode(m_db), "bind(select)");
    return std::nullopt;
  }

  const int rc = sqlite3_step(m_stmtSelectValue);
  if (rc == SQLITE_DONE) {
    recordSqlSuccessLocked();
    resetStmt(m_stmtSelectValue);
    return std::nullopt;
  }
  if (rc != SQLITE_ROW) {
    recordSqlErrorLocked(rc, "step(select)");
    resetStmt(m_stmtSelectValue);
    return std::nullopt;
  }

  const void* blobPtr = sqlite3_column_blob(m_stmtSelectValue, 0);
  const int blobBytes = sqlite3_column_bytes(m_stmtSelectValue, 0);
  const int64_t lastAccessNs = sqlite3_column_int64(m_stmtSelectValue, 1);
  if (blobBytes < 0) {
    recordSqlErrorLocked(sqlite3_errcode(m_db), "column_bytes(select)");
    resetStmt(m_stmtSelectValue);
    return std::nullopt;
  }

  Blob out;
  try {
    out.resize(static_cast<size_t>(blobBytes));
  } catch (...) {
    // Disk cache is best-effort; treat allocation failures as cache misses.
    resetStmt(m_stmtSelectValue);
    return std::nullopt;
  }
  if (blobBytes > 0) {
    if (blobPtr == nullptr) {
      resetStmt(m_stmtSelectValue);
      return std::nullopt;
    }
    std::memcpy(out.data(), blobPtr, static_cast<size_t>(blobBytes));
  }
  resetStmt(m_stmtSelectValue);
  recordSqlSuccessLocked();

  // Touch access time (best-effort).
  const int64_t nowNs = nowNsLocked();
  const int64_t touchMinIntervalNs =
    std::chrono::duration_cast<std::chrono::nanoseconds>(kTouchMinInterval).count();
  const int64_t last = std::max<int64_t>(0, lastAccessNs);
  if (nowNs >= last && (nowNs - last) >= touchMinIntervalNs) {
    resetStmt(m_stmtTouch);
    (void)sqlite3_bind_int64(m_stmtTouch, 1, nowNs);
    (void)sqlite3_bind_blob(m_stmtTouch, 2, keyHash.data(), static_cast<int>(keyHash.size()), SQLITE_TRANSIENT);
    (void)stepExpectDone(m_stmtTouch);
    resetStmt(m_stmtTouch);
  }

  return out;
}

std::optional<ZSqliteLRUCache::GetNoTouchResult> ZSqliteLRUCache::tryGetNoTouch(std::span<const std::uint8_t> keyHash)
{
  std::lock_guard<std::mutex> lock(m_mu);
  if (m_disabled || !m_db || !m_stmtSelectValue) {
    return std::nullopt;
  }
  if (keyHash.empty()) {
    return std::nullopt;
  }
  if (keyHash.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }

  resetStmt(m_stmtSelectValue);
  if (sqlite3_bind_blob(m_stmtSelectValue,
                        /*index=*/1,
                        keyHash.data(),
                        static_cast<int>(keyHash.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    recordSqlErrorLocked(sqlite3_errcode(m_db), "bind(select)");
    return std::nullopt;
  }

  const int rc = sqlite3_step(m_stmtSelectValue);
  if (rc == SQLITE_DONE) {
    recordSqlSuccessLocked();
    resetStmt(m_stmtSelectValue);
    return std::nullopt;
  }
  if (rc != SQLITE_ROW) {
    recordSqlErrorLocked(rc, "step(select)");
    resetStmt(m_stmtSelectValue);
    return std::nullopt;
  }

  const void* blobPtr = sqlite3_column_blob(m_stmtSelectValue, 0);
  const int blobBytes = sqlite3_column_bytes(m_stmtSelectValue, 0);
  const int64_t lastAccessNs = sqlite3_column_int64(m_stmtSelectValue, 1);
  if (blobBytes < 0) {
    recordSqlErrorLocked(sqlite3_errcode(m_db), "column_bytes(select)");
    resetStmt(m_stmtSelectValue);
    return std::nullopt;
  }

  Blob out;
  try {
    out.resize(static_cast<size_t>(blobBytes));
  } catch (...) {
    // Disk cache is best-effort; treat allocation failures as cache misses.
    resetStmt(m_stmtSelectValue);
    return std::nullopt;
  }
  if (blobBytes > 0) {
    if (blobPtr == nullptr) {
      resetStmt(m_stmtSelectValue);
      return std::nullopt;
    }
    std::memcpy(out.data(), blobPtr, static_cast<size_t>(blobBytes));
  }
  resetStmt(m_stmtSelectValue);
  recordSqlSuccessLocked();

  GetNoTouchResult res;
  res.value = std::move(out);
  res.lastAccessNs = lastAccessNs;
  return res;
}

void ZSqliteLRUCache::put(std::span<const std::uint8_t> keyHash, std::span<const std::uint8_t> value)
{
  std::lock_guard<std::mutex> lock(m_mu);
  CHECK(!m_readOnly);
  if (m_disabled || !m_db || !m_stmtUpsert) {
    return;
  }
  if (m_maxBytes == 0) {
    return;
  }
  if (keyHash.empty()) {
    return;
  }
  if (keyHash.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return;
  }
  if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return;
  }
  if (static_cast<uint64_t>(value.size()) > m_maxBytes) {
    // An entry larger than the entire budget would cause immediate thrash; skip caching.
    return;
  }

  const int64_t nowNs = nowNsLocked();

  // Transaction amortizes fsyncs; size accounting is handled by SQLite triggers.
  if (!execLocked("BEGIN IMMEDIATE")) {
    return;
  }

  resetStmt(m_stmtUpsert);
  (void)sqlite3_bind_blob(m_stmtUpsert, 1, keyHash.data(), static_cast<int>(keyHash.size()), SQLITE_TRANSIENT);
  (void)sqlite3_bind_int64(m_stmtUpsert, 2, static_cast<sqlite3_int64>(value.size()));
  (void)sqlite3_bind_int64(m_stmtUpsert, 3, nowNs);
  (void)sqlite3_bind_blob(m_stmtUpsert, 4, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);

  const bool upsertOk = stepExpectDone(m_stmtUpsert);
  resetStmt(m_stmtUpsert);

  if (!upsertOk) {
    recordSqlErrorLocked(sqlite3_errcode(m_db), "step(upsert)");
    (void)execLocked("ROLLBACK");
    return;
  }

  if (!execLocked("COMMIT")) {
    (void)execLocked("ROLLBACK");
    return;
  }
  recordSqlSuccessLocked();

  maybePruneLocked(nowNs);
}

void ZSqliteLRUCache::touch(std::span<const std::uint8_t> keyHash, int64_t nowNs)
{
  std::lock_guard<std::mutex> lock(m_mu);
  CHECK(!m_readOnly);
  if (m_disabled || !m_db || !m_stmtTouch) {
    return;
  }
  if (keyHash.empty()) {
    return;
  }
  if (keyHash.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return;
  }

  resetStmt(m_stmtTouch);
  (void)sqlite3_bind_int64(m_stmtTouch, 1, nowNs);
  (void)sqlite3_bind_blob(m_stmtTouch, 2, keyHash.data(), static_cast<int>(keyHash.size()), SQLITE_TRANSIENT);
  (void)stepExpectDone(m_stmtTouch);
  resetStmt(m_stmtTouch);
}

void ZSqliteLRUCache::erase(std::span<const std::uint8_t> keyHash)
{
  std::lock_guard<std::mutex> lock(m_mu);
  CHECK(!m_readOnly);
  if (m_disabled || !m_db || !m_stmtDelete) {
    return;
  }
  if (keyHash.empty()) {
    return;
  }
  if (keyHash.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return;
  }

  if (!execLocked("BEGIN IMMEDIATE")) {
    return;
  }

  resetStmt(m_stmtDelete);
  (void)sqlite3_bind_blob(m_stmtDelete, 1, keyHash.data(), static_cast<int>(keyHash.size()), SQLITE_TRANSIENT);
  const bool delOk = stepExpectDone(m_stmtDelete);
  resetStmt(m_stmtDelete);
  if (!delOk) {
    recordSqlErrorLocked(sqlite3_errcode(m_db), "step(delete)");
    (void)execLocked("ROLLBACK");
    return;
  }

  if (!execLocked("COMMIT")) {
    (void)execLocked("ROLLBACK");
    return;
  }
  recordSqlSuccessLocked();
}

void ZSqliteLRUCache::closeDbLocked()
{
  if (m_stmtSelectValue) {
    sqlite3_finalize(m_stmtSelectValue);
    m_stmtSelectValue = nullptr;
  }
  if (m_stmtTouch) {
    sqlite3_finalize(m_stmtTouch);
    m_stmtTouch = nullptr;
  }
  if (m_stmtUpsert) {
    sqlite3_finalize(m_stmtUpsert);
    m_stmtUpsert = nullptr;
  }
  if (m_stmtDelete) {
    sqlite3_finalize(m_stmtDelete);
    m_stmtDelete = nullptr;
  }
  if (m_stmtPruneSelect) {
    sqlite3_finalize(m_stmtPruneSelect);
    m_stmtPruneSelect = nullptr;
  }
  if (m_stmtSelectTotalBytes) {
    sqlite3_finalize(m_stmtSelectTotalBytes);
    m_stmtSelectTotalBytes = nullptr;
  }

  if (m_db) {
    sqlite3_close(m_db);
    m_db = nullptr;
  }
}

bool ZSqliteLRUCache::initDbLocked()
{
  closeDbLocked();

  m_dbPath = m_dbPath.trimmed();
  if (m_dbPath.isEmpty() || m_maxBytes == 0) {
    return false;
  }

  const QFileInfo fi(m_dbPath);
  const QString dirPath = fi.dir().absolutePath();
  if (m_readOnly) {
    // Read-only connections must not create directories. This avoids accidental
    // directory creation when a user points the cache at a removable volume
    // and it is missing/unmounted.
    if (!QDir(dirPath).exists()) {
      LOG(WARNING) << "SQLite cache disabled: directory does not exist for read-only open: " << dirPath;
      return false;
    }
  } else {
    if (!QDir().mkpath(dirPath)) {
      LOG(WARNING) << "SQLite cache disabled: failed to create directory " << dirPath;
      return false;
    }
  }

  const QByteArray pathBytes = QFile::encodeName(m_dbPath);
  const int openFlags = m_readOnly ? (SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX)
                                   : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX);
  const int rc = sqlite3_open_v2(pathBytes.constData(), &m_db, openFlags, /*zVfs=*/nullptr);
  if (rc != SQLITE_OK || !m_db) {
    LOG(WARNING) << "SQLite cache disabled: sqlite3_open_v2 failed for " << m_dbPath << " rc=" << rc;
    closeDbLocked();
    return false;
  }

  // Allow initial schema creation to wait briefly under multi-process startup races.
  sqlite3_busy_timeout(m_db, /*ms=*/250);

  if (m_readOnly) {
    // Read connections should not modify PRAGMAs or attempt schema creation.
    if (!prepareLocked("SELECT value,last_access_ns FROM entries WHERE key_hash=?1", &m_stmtSelectValue)) {
      LOG(WARNING) << "SQLite cache disabled: failed to prepare read-only statements for " << m_dbPath;
      closeDbLocked();
      return false;
    }

    // Operational policy: do not block for contention (best-effort cache semantics).
    sqlite3_busy_timeout(m_db, /*ms=*/0);
    return true;
  }

  // Cache-friendly defaults. WAL keeps writes sequential and avoids blocking readers.
  (void)execLocked("PRAGMA journal_mode=WAL");
  (void)execLocked("PRAGMA synchronous=NORMAL");
  (void)execLocked("PRAGMA temp_store=MEMORY");

  if (!ensureSchemaLocked()) {
    closeDbLocked();
    return false;
  }

  // Prepare hot statements.
  if (!prepareLocked("SELECT value,last_access_ns FROM entries WHERE key_hash=?1", &m_stmtSelectValue) ||
      !prepareLocked("UPDATE entries SET last_access_ns=?1 WHERE key_hash=?2", &m_stmtTouch) ||
      !prepareLocked("INSERT INTO entries(key_hash,size_bytes,last_access_ns,value) VALUES(?1,?2,?3,?4) "
                     "ON CONFLICT(key_hash) DO UPDATE SET "
                     "size_bytes=excluded.size_bytes,last_access_ns=excluded.last_access_ns,value=excluded.value",
                     &m_stmtUpsert) ||
      !prepareLocked("DELETE FROM entries WHERE key_hash=?1", &m_stmtDelete) ||
      !prepareLocked("SELECT key_hash,size_bytes FROM entries ORDER BY last_access_ns ASC LIMIT ?1", &m_stmtPruneSelect) ||
      !prepareLocked("SELECT value_int FROM meta WHERE key=?1", &m_stmtSelectTotalBytes)) {
    LOG(WARNING) << "SQLite cache disabled: failed to prepare statements for " << m_dbPath;
    closeDbLocked();
    return false;
  }

  // Operational policy: do not block for contention (best-effort cache semantics).
  sqlite3_busy_timeout(m_db, /*ms=*/0);

  return true;
}

bool ZSqliteLRUCache::ensureSchemaLocked()
{
  CHECK(m_db != nullptr);
  CHECK(!m_readOnly);

  // Read PRAGMA user_version.
  int userVersion = 0;
  sqlite3_stmt* stmt = nullptr;
  if (!prepareLocked("PRAGMA user_version", &stmt)) {
    return false;
  }
  if (stmt) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      userVersion = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }

  if (userVersion != 0 && userVersion != kCacheSchemaVersion) {
    // Best-effort reset for incompatible schema (cache is disposable).
    (void)execLocked("DROP TABLE IF EXISTS entries");
    (void)execLocked("DROP TABLE IF EXISTS meta");
    userVersion = 0;
  }

  if (!execLocked("CREATE TABLE IF NOT EXISTS meta("
                  "key TEXT PRIMARY KEY,"
                  "value_int INTEGER NOT NULL)") ||
      !execLocked("CREATE TABLE IF NOT EXISTS entries("
                  "key_hash BLOB PRIMARY KEY,"
                  "size_bytes INTEGER NOT NULL,"
                  "last_access_ns INTEGER NOT NULL,"
                  "value BLOB NOT NULL) WITHOUT ROWID") ||
      !execLocked("CREATE INDEX IF NOT EXISTS entries_last_access ON entries(last_access_ns)") ||
      !execLocked(fmt::format("INSERT OR IGNORE INTO meta(key,value_int) VALUES('{}',0)", std::string(kMetaKeyTotalBytes))
                    .c_str()) ||
      !execLocked("CREATE TRIGGER IF NOT EXISTS entries_total_bytes_insert "
                  "AFTER INSERT ON entries BEGIN "
                  "UPDATE meta SET value_int=value_int+NEW.size_bytes WHERE key='total_bytes'; "
                  "END") ||
      !execLocked("CREATE TRIGGER IF NOT EXISTS entries_total_bytes_delete "
                  "AFTER DELETE ON entries BEGIN "
                  "UPDATE meta SET value_int=value_int-OLD.size_bytes WHERE key='total_bytes'; "
                  "END") ||
      !execLocked("CREATE TRIGGER IF NOT EXISTS entries_total_bytes_update "
                  "AFTER UPDATE OF size_bytes ON entries BEGIN "
                  "UPDATE meta SET value_int=value_int+(NEW.size_bytes-OLD.size_bytes) WHERE key='total_bytes'; "
                  "END") ||
      !execLocked(fmt::format("PRAGMA user_version={}", kCacheSchemaVersion).c_str())) {
    return false;
  }

  return true;
}

int64_t ZSqliteLRUCache::nowNsLocked() const
{
  CHECK(m_nowNsFn);
  return m_nowNsFn();
}

void ZSqliteLRUCache::maybePruneLocked(int64_t nowNs)
{
  if (m_maxBytes == 0 || m_disabled || !m_db || !m_stmtSelectTotalBytes || !m_stmtPruneSelect || !m_stmtDelete) {
    return;
  }

  auto readTotalBytes = [&]() -> std::optional<uint64_t> {
    resetStmt(m_stmtSelectTotalBytes);
    if (sqlite3_bind_text(m_stmtSelectTotalBytes,
                          /*index=*/1,
                          kMetaKeyTotalBytes.data(),
                          static_cast<int>(kMetaKeyTotalBytes.size()),
                          SQLITE_STATIC) != SQLITE_OK) {
      recordSqlErrorLocked(sqlite3_errcode(m_db), "bind(select_total_bytes)");
      return std::nullopt;
    }
    const int rc = sqlite3_step(m_stmtSelectTotalBytes);
    if (rc == SQLITE_ROW) {
      const int64_t v = sqlite3_column_int64(m_stmtSelectTotalBytes, 0);
      resetStmt(m_stmtSelectTotalBytes);
      recordSqlSuccessLocked();
      if (v <= 0) {
        return 0ULL;
      }
      return static_cast<uint64_t>(v);
    }
    if (rc == SQLITE_DONE) {
      resetStmt(m_stmtSelectTotalBytes);
      recordSqlSuccessLocked();
      return 0ULL;
    }
    recordSqlErrorLocked(rc, "step(select_total_bytes)");
    resetStmt(m_stmtSelectTotalBytes);
    return std::nullopt;
  };

  const auto totalBytesOpt = readTotalBytes();
  if (!totalBytesOpt.has_value() || *totalBytesOpt <= m_maxBytes) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_lastPrune.time_since_epoch() != std::chrono::steady_clock::duration::zero() &&
      (now - m_lastPrune) < kPruneMinInterval) {
    return;
  }
  m_lastPrune = now;

  const uint64_t target = static_cast<uint64_t>(static_cast<double>(m_maxBytes) * kPruneTargetFraction);
  uint64_t bytes = *totalBytesOpt;
  size_t removed = 0;

  // Keep the prune transaction short; do batched deletes.
  if (!execLocked("BEGIN IMMEDIATE")) {
    return;
  }

  const auto txnTotalOpt = readTotalBytes();
  if (!txnTotalOpt.has_value()) {
    (void)execLocked("ROLLBACK");
    return;
  }
  bytes = *txnTotalOpt;
  if (bytes <= m_maxBytes) {
    (void)execLocked("ROLLBACK");
    return;
  }

  resetStmt(m_stmtPruneSelect);
  resetStmt(m_stmtDelete);

  while (bytes > target) {
    resetStmt(m_stmtPruneSelect);
    (void)sqlite3_bind_int(m_stmtPruneSelect, 1, 256);
    const int rc = sqlite3_step(m_stmtPruneSelect);
    if (rc != SQLITE_ROW) {
      break;
    }

    // The statement yields one row at a time; delete as we iterate, then continue stepping.
    while (true) {
      const void* keyPtr = sqlite3_column_blob(m_stmtPruneSelect, 0);
      const int keyBytes = sqlite3_column_bytes(m_stmtPruneSelect, 0);
      const int64_t sizeVal = sqlite3_column_int64(m_stmtPruneSelect, 1);
      if (!keyPtr || keyBytes <= 0) {
        break;
      }

      resetStmt(m_stmtDelete);
      (void)sqlite3_bind_blob(m_stmtDelete, 1, keyPtr, keyBytes, SQLITE_TRANSIENT);
      if (stepExpectDone(m_stmtDelete)) {
        if (sizeVal > 0) {
          bytes -= std::min<uint64_t>(bytes, static_cast<uint64_t>(sizeVal));
        }
        ++removed;
      }

      const int rcNext = sqlite3_step(m_stmtPruneSelect);
      if (rcNext != SQLITE_ROW) {
        break;
      }
      if (bytes <= target) {
        break;
      }
    }

    if (bytes <= target) {
      break;
    }
  }

  resetStmt(m_stmtPruneSelect);
  resetStmt(m_stmtDelete);

  const bool commitOk = execLocked("COMMIT");
  if (!commitOk) {
    (void)execLocked("ROLLBACK");
    return;
  }
  recordSqlSuccessLocked();

  if (removed > 0) {
    VLOG(1) << "SQLite cache pruned " << removed << " entries; totalBytes=" << bytes
            << " maxBytes=" << m_maxBytes << " nowNs=" << nowNs;
  }
}

bool ZSqliteLRUCache::execLocked(const char* sql)
{
  if (m_disabled || m_db == nullptr) {
    return false;
  }
  char* errMsg = nullptr;
  const int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    const std::string err = errMsg ? std::string(errMsg) : std::string(sqlite3_errmsg(m_db));
    if (errMsg) {
      sqlite3_free(errMsg);
    }
    VLOG(1) << "SQLite exec failed: rc=" << rc << " sql='" << sql << "' err='" << err << "'";
    recordSqlErrorLocked(rc, "exec");
    return false;
  }
  if (errMsg) {
    sqlite3_free(errMsg);
  }
  return true;
}

bool ZSqliteLRUCache::prepareLocked(const char* sql, sqlite3_stmt** outStmt)
{
  if (m_disabled || m_db == nullptr || outStmt == nullptr) {
    return false;
  }
  *outStmt = nullptr;
  const int rc = sqlite3_prepare_v2(m_db, sql, -1, outStmt, nullptr);
  if (rc != SQLITE_OK || *outStmt == nullptr) {
    VLOG(1) << "SQLite prepare failed: rc=" << rc << " sql='" << sql << "' err='" << sqlite3_errmsg(m_db) << "'";
    recordSqlErrorLocked(rc, "prepare");
    if (*outStmt) {
      sqlite3_finalize(*outStmt);
      *outStmt = nullptr;
    }
    return false;
  }
  return true;
}

} // namespace nim
