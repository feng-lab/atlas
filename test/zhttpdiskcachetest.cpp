#include "zhttpdiskcache.h"
#include "zhttpclient.h"
#include "zdiskcacheutils.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <boost/hash2/sha2.hpp>

#include <chrono>
#include <limits>

#include <vtksqlite/sqlite3.h>

namespace nim {

namespace {

QString cacheDbPathForRoot(const QString& rootDir)
{
  const QString cacheDir = atlasDiskCacheDirFromRoot(rootDir);
  return QDir(cacheDir).filePath(QStringLiteral("http.sqlite"));
}

QByteArray cacheKeyHashFor(const ZHttpGetRequest& request)
{
  QByteArray keyBytes;
  keyBytes.append("GET\n", 4);
  keyBytes.append("partition=", 10);
  keyBytes.append(QByteArray(request.cachePartition.data(), static_cast<int>(request.cachePartition.size())));
  keyBytes.append('\n');
  keyBytes.append(QByteArray(request.url.data(), static_cast<int>(request.url.size())));
  keyBytes.append('\n');
  keyBytes.append("range=", 6);
  if (request.exactByteRange.has_value()) {
    const std::string rangeHeaderValue = formatHttpByteRangeHeaderValue(*request.exactByteRange);
    keyBytes.append(QByteArray(rangeHeaderValue.data(), static_cast<int>(rangeHeaderValue.size())));
  }
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

[[nodiscard]] ZHttpGetRequest makeRequest(std::string url, std::optional<ZHttpByteRange> exactByteRange = std::nullopt)
{
  return ZHttpGetRequest{.url = std::move(url),
                         .timeout = std::chrono::milliseconds(0),
                         .exactByteRange = exactByteRange};
}

ZHttpGetBytesResult makeResult(std::vector<uint8_t> body, uint16_t status = 200)
{
  ZHttpGetBytesResult out{};
  out.status = status;
  out.contentType = "application/octet-stream";
  out.contentEncoding = "identity";
  out.encodedBodyBytes = body.size();
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
  const ZHttpGetRequest request = makeRequest(url);
  cache.put(request, makeResult({1, 2, 3, 4}));
  ASSERT_TRUE(cache.drainWrites(std::chrono::seconds(5)));

  auto got = cache.tryGet(request);
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
  const ZHttpGetRequest hitRequest = makeRequest(url, ZHttpByteRange{.offset = 0, .length = 4});
  const ZHttpGetRequest missRequest = makeRequest(url, ZHttpByteRange{.offset = 4, .length = 4});
  cache.put(hitRequest, makeResult({1, 2, 3, 4}, /*status=*/206));
  ASSERT_TRUE(cache.drainWrites(std::chrono::seconds(5)));

  auto hit = cache.tryGet(hitRequest);
  ASSERT_TRUE(hit.has_value());

  auto miss = cache.tryGet(missRequest);
  EXPECT_FALSE(miss.has_value());
}

TEST(ZHttpDiskCache, CachePartitionDifferentiatesEntries)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  ZHttpDiskCache cache(tmp.path(), /*maxBytes=*/1024 * 1024);
  ASSERT_TRUE(cache.isEnabled());

  const std::string url = "https://example.invalid/private.bin";
  ZHttpGetRequest partitionA = makeRequest(url, ZHttpByteRange{.offset = 0, .length = 4});
  partitionA.cachePartition = "s3:bucket:us-east-1:ACCESS_A";
  ZHttpGetRequest partitionB = partitionA;
  partitionB.cachePartition = "s3:bucket:us-east-1:ACCESS_B";

  cache.put(partitionA, makeResult({1, 2, 3, 4}, /*status=*/206));
  ASSERT_TRUE(cache.drainWrites(std::chrono::seconds(5)));

  auto hit = cache.tryGet(partitionA);
  ASSERT_TRUE(hit.has_value());

  auto miss = cache.tryGet(partitionB);
  EXPECT_FALSE(miss.has_value());
}

TEST(ZHttpDiskCache, RejectsStatus200RangeEntry)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  ZHttpDiskCache cache(tmp.path(), /*maxBytes=*/1024 * 1024);
  ASSERT_TRUE(cache.isEnabled());

  const std::string url = "https://example.invalid/data.bin";
  const ZHttpGetRequest request = makeRequest(url, ZHttpByteRange{.offset = 7, .length = 3});
  cache.put(request, makeResult({1, 2, 3, 4}, /*status=*/200));
  ASSERT_TRUE(cache.drainWrites(std::chrono::seconds(5)));

  EXPECT_FALSE(cache.tryGet(request).has_value());
  EXPECT_EQ(countCacheEntries(tmp.path()), 0u);
}

TEST(ZHttpDiskCache, RejectsMismatched206RangeEntry)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  ZHttpDiskCache cache(tmp.path(), /*maxBytes=*/1024 * 1024);
  ASSERT_TRUE(cache.isEnabled());

  const std::string url = "https://example.invalid/data.bin";
  const ZHttpGetRequest request = makeRequest(url, ZHttpByteRange{.offset = 7, .length = 3});
  cache.put(request, makeResult({1, 2}, /*status=*/206));
  ASSERT_TRUE(cache.drainWrites(std::chrono::seconds(5)));

  EXPECT_FALSE(cache.tryGet(request).has_value());
  EXPECT_EQ(countCacheEntries(tmp.path()), 0u);
}

TEST(ZHttpDiskCache, CorruptEntryIsMissAndRemoved)
{
  QTemporaryDir tmp;
  ASSERT_TRUE(tmp.isValid());

  ZHttpDiskCache cache(tmp.path(), /*maxBytes=*/1024 * 1024);
  ASSERT_TRUE(cache.isEnabled());

  const std::string url = "https://example.invalid/data.bin";
  const ZHttpGetRequest request = makeRequest(url, ZHttpByteRange{.offset = 0, .length = 4});
  cache.put(request, makeResult({1, 2, 3, 4}, /*status=*/206));
  ASSERT_TRUE(cache.drainWrites(std::chrono::seconds(5)));

  const QByteArray keyHash = cacheKeyHashFor(request);
  ASSERT_TRUE(hasEntry(tmp.path(), keyHash));
  ASSERT_TRUE(overwriteEntryPrefix(tmp.path(), keyHash, QByteArray("BADCACHE")));

  auto miss = cache.tryGet(request);
  EXPECT_FALSE(miss.has_value());
  ASSERT_TRUE(cache.drainWrites(std::chrono::seconds(5)));
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
  const ZHttpGetRequest request1 = makeRequest(url1);
  const ZHttpGetRequest request2 = makeRequest(url2);

  cache.put(request1, makeResult(std::vector<uint8_t>(600, 0x11)));
  ASSERT_TRUE(cache.drainWrites(std::chrono::seconds(5)));
  const QByteArray key1 = cacheKeyHashFor(request1);
  ASSERT_TRUE(hasEntry(tmp.path(), key1));
  ASSERT_TRUE(setLastAccessNs(tmp.path(), key1, /*lastAccessNs=*/1));

  cache.put(request2, makeResult(std::vector<uint8_t>(600, 0x22)));
  ASSERT_TRUE(cache.drainWrites(std::chrono::seconds(5)));
  const QByteArray key2 = cacheKeyHashFor(request2);
  ASSERT_TRUE(hasEntry(tmp.path(), key2));

  // After pruning, only the newest entry should remain.
  EXPECT_FALSE(hasEntry(tmp.path(), key1));
  EXPECT_TRUE(hasEntry(tmp.path(), key2));
  EXPECT_EQ(countCacheEntries(tmp.path()), 1u);
}

} // namespace nim
