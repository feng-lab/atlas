#include "zhttpdiskcache.h"
#include "zproxygenhttpclient.h"
#include "zdiskcacheutils.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <boost/hash2/sha2.hpp>

#include <limits>

#include <vtksqlite/sqlite3.h>

namespace nim {

namespace {

QString cacheDbPathForRoot(const QString& rootDir)
{
  const QString cacheDir = atlasDiskCacheDirFromRoot(rootDir);
  return QDir(cacheDir).filePath(QStringLiteral("http.sqlite"));
}

QByteArray cacheKeyHashFor(const std::string& url, const std::string& rangeHeaderValue)
{
  QByteArray keyBytes;
  keyBytes.append("GET\n", 4);
  keyBytes.append(QByteArray(url.data(), static_cast<int>(url.size())));
  keyBytes.append('\n');
  keyBytes.append("range=", 6);
  keyBytes.append(QByteArray(rangeHeaderValue.data(), static_cast<int>(rangeHeaderValue.size())));
  keyBytes.append('\n');

  boost::hash2::sha2_256 hash;
  if (!keyBytes.isEmpty()) {
    hash.update(keyBytes.constData(), static_cast<size_t>(keyBytes.size()));
  }
  const boost::hash2::sha2_256::result_type digest = hash.result();
  CHECK(digest.size() <= static_cast<size_t>(std::numeric_limits<int>::max()));
  return QByteArray(reinterpret_cast<const char*>(digest.data()), static_cast<int>(digest.size()));
}

[[nodiscard]] sqlite3* openDb(const QString& dbPath, int flags)
{
  sqlite3* db = nullptr;
  const QByteArray pathBytes = QFile::encodeName(dbPath);
  if (sqlite3_open_v2(pathBytes.constData(), &db, flags, /*zVfs=*/nullptr) != SQLITE_OK) {
    if (db) {
      sqlite3_close(db);
    }
    return nullptr;
  }
  return db;
}

size_t countCacheEntries(const QString& rootDir)
{
  const QString dbPath = cacheDbPathForRoot(rootDir);
  sqlite3* db = openDb(dbPath, SQLITE_OPEN_READONLY);
  if (!db) {
    return 0;
  }

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT count(*) FROM entries", -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
    if (stmt) {
      sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return 0;
  }

  size_t count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const sqlite3_int64 v = sqlite3_column_int64(stmt, 0);
    if (v > 0) {
      count = static_cast<size_t>(v);
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return count;
}

bool hasEntry(const QString& rootDir, const QByteArray& keyHash)
{
  const QString dbPath = cacheDbPathForRoot(rootDir);
  sqlite3* db = openDb(dbPath, SQLITE_OPEN_READONLY);
  if (!db) {
    return false;
  }

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM entries WHERE key_hash=?1", -1, &stmt, nullptr) != SQLITE_OK || !stmt) {
    if (stmt) {
      sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return false;
  }

  (void)sqlite3_bind_blob(stmt, 1, keyHash.constData(), keyHash.size(), SQLITE_TRANSIENT);
  const bool ok = (sqlite3_step(stmt) == SQLITE_ROW);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return ok;
}

bool overwriteEntryPrefix(const QString& rootDir, const QByteArray& keyHash, const QByteArray& prefix)
{
  const QString dbPath = cacheDbPathForRoot(rootDir);
  sqlite3* db = openDb(dbPath, SQLITE_OPEN_READWRITE);
  if (!db) {
    return false;
  }

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "UPDATE entries SET value=?1 || substr(value, ?2) WHERE key_hash=?3",
                         -1,
                         &stmt,
                         nullptr) != SQLITE_OK ||
      !stmt) {
    if (stmt) {
      sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return false;
  }

  const int startIndex = prefix.size() + 1; // sqlite substr is 1-indexed
  (void)sqlite3_bind_blob(stmt, 1, prefix.constData(), prefix.size(), SQLITE_TRANSIENT);
  (void)sqlite3_bind_int(stmt, 2, startIndex);
  (void)sqlite3_bind_blob(stmt, 3, keyHash.constData(), keyHash.size(), SQLITE_TRANSIENT);
  const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return ok;
}

bool setLastAccessNs(const QString& rootDir, const QByteArray& keyHash, sqlite3_int64 lastAccessNs)
{
  const QString dbPath = cacheDbPathForRoot(rootDir);
  sqlite3* db = openDb(dbPath, SQLITE_OPEN_READWRITE);
  if (!db) {
    return false;
  }

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "UPDATE entries SET last_access_ns=?1 WHERE key_hash=?2", -1, &stmt, nullptr) != SQLITE_OK ||
      !stmt) {
    if (stmt) {
      sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return false;
  }

  (void)sqlite3_bind_int64(stmt, 1, lastAccessNs);
  (void)sqlite3_bind_blob(stmt, 2, keyHash.constData(), keyHash.size(), SQLITE_TRANSIENT);
  const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return ok;
}

ZHttpGetBytesResult makeResult(std::vector<uint8_t> body)
{
  ZHttpGetBytesResult out{};
  out.status = 200;
  out.contentType = "application/octet-stream";
  out.contentEncoding = "identity";
  out.body = std::move(body);
  return out;
}

} // namespace

TEST(ZHttpDiskCache, StoreAndHitNoRange)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  ZHttpDiskCache cache(tmp.path(), /*maxBytes=*/1024 * 1024);
  ASSERT_TRUE(cache.isEnabled());

  const std::string url = "https://example.invalid/data.bin";
  cache.put(url, /*requestHeaders=*/{}, makeResult({1, 2, 3, 4}));

  auto got = cache.tryGet(url, /*requestHeaders=*/{});
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->status, 200);
  EXPECT_EQ(got->contentType, "application/octet-stream");
  ASSERT_EQ(got->body.size(), 4u);
  EXPECT_EQ(got->body[0], 1);
  EXPECT_EQ(got->body[1], 2);
  EXPECT_EQ(got->body[2], 3);
  EXPECT_EQ(got->body[3], 4);
}

TEST(ZHttpDiskCache, RangeDifferentiatesEntries)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  ZHttpDiskCache cache(tmp.path(), /*maxBytes=*/1024 * 1024);
  ASSERT_TRUE(cache.isEnabled());

  const std::string url = "https://example.invalid/data.bin";
  cache.put(url, /*requestHeaders=*/{{"Range", "bytes=0-3"}}, makeResult({1, 2, 3, 4}));

  auto hit = cache.tryGet(url, /*requestHeaders=*/{{"range", "bytes=0-3"}});
  ASSERT_TRUE(hit.has_value());

  auto miss = cache.tryGet(url, /*requestHeaders=*/{{"range", "bytes=4-7"}});
  EXPECT_FALSE(miss.has_value());
}

TEST(ZHttpDiskCache, CorruptEntryIsMissAndRemoved)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  ZHttpDiskCache cache(tmp.path(), /*maxBytes=*/1024 * 1024);
  ASSERT_TRUE(cache.isEnabled());

  const std::string url = "https://example.invalid/data.bin";
  cache.put(url, /*requestHeaders=*/{{"range", "bytes=0-3"}}, makeResult({1, 2, 3, 4}));

  const QByteArray keyHash = cacheKeyHashFor(url, "bytes=0-3");
  ASSERT_TRUE(hasEntry(tmp.path(), keyHash));
  ASSERT_TRUE(overwriteEntryPrefix(tmp.path(), keyHash, QByteArray("BADCACHE")));

  auto miss = cache.tryGet(url, /*requestHeaders=*/{{"range", "bytes=0-3"}});
  EXPECT_FALSE(miss.has_value());
  EXPECT_FALSE(hasEntry(tmp.path(), keyHash));
}

TEST(ZHttpDiskCache, PrunesOldestEntries)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  // Small budget to force pruning.
  ZHttpDiskCache cache(tmp.path(), /*maxBytes=*/1000);
  ASSERT_TRUE(cache.isEnabled());

  const std::string url1 = "https://example.invalid/first.bin";
  const std::string url2 = "https://example.invalid/second.bin";

  cache.put(url1, /*requestHeaders=*/{}, makeResult(std::vector<uint8_t>(600, 0x11)));
  const QByteArray key1 = cacheKeyHashFor(url1, /*range=*/"");
  ASSERT_TRUE(hasEntry(tmp.path(), key1));
  ASSERT_TRUE(setLastAccessNs(tmp.path(), key1, /*lastAccessNs=*/1));

  cache.put(url2, /*requestHeaders=*/{}, makeResult(std::vector<uint8_t>(600, 0x22)));
  const QByteArray key2 = cacheKeyHashFor(url2, /*range=*/"");
  ASSERT_TRUE(hasEntry(tmp.path(), key2));

  // After pruning, only the newest entry should remain.
  EXPECT_FALSE(hasEntry(tmp.path(), key1));
  EXPECT_TRUE(hasEntry(tmp.path(), key2));
  EXPECT_EQ(countCacheEntries(tmp.path()), 1u);
}

} // namespace nim
