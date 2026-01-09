#include "zhttpdiskcache.h"
#include "zproxygenhttpclient.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace nim {

namespace {

QString cacheEntryPathFor(const QString& rootDir, const std::string& url, const std::string& rangeHeaderValue)
{
  QByteArray keyBytes;
  keyBytes.append("GET\n", 4);
  keyBytes.append(QByteArray(url.data(), static_cast<int>(url.size())));
  keyBytes.append('\n');
  keyBytes.append("range=", 6);
  keyBytes.append(QByteArray(rangeHeaderValue.data(), static_cast<int>(rangeHeaderValue.size())));
  keyBytes.append('\n');

  const QByteArray hexHash = QCryptographicHash::hash(keyBytes, QCryptographicHash::Sha256).toHex();
  const QString subdir = QString::fromLatin1(hexHash.left(2));
  const QString filename = QString::fromLatin1(hexHash) + QStringLiteral(".bin");
  return QDir(rootDir).filePath(QStringLiteral("atlas_http_disk_cache_v1/entries/%1/%2").arg(subdir, filename));
}

size_t countCacheEntries(const QString& rootDir)
{
  const QString entriesDir = QDir(rootDir).filePath(QStringLiteral("atlas_http_disk_cache_v1/entries"));
  size_t count = 0;
  QDirIterator it(entriesDir, QStringList() << "*.bin", QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    ++count;
  }
  return count;
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

  const QString entryPath = cacheEntryPathFor(tmp.path(), url, "bytes=0-3");
  ASSERT_TRUE(QFileInfo::exists(entryPath));

  QFile f(entryPath);
  ASSERT_TRUE(f.open(QIODevice::ReadWrite));
  ASSERT_GE(f.size(), 8);
  ASSERT_TRUE(f.seek(0));
  ASSERT_EQ(f.write("BADCACHE", 8), 8);
  f.close();

  auto miss = cache.tryGet(url, /*requestHeaders=*/{{"range", "bytes=0-3"}});
  EXPECT_FALSE(miss.has_value());
  EXPECT_FALSE(QFileInfo::exists(entryPath));
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
  const QString path1 = cacheEntryPathFor(tmp.path(), url1, /*range=*/"");
  ASSERT_TRUE(QFileInfo::exists(path1));
  QFile f1(path1);
  ASSERT_TRUE(f1.open(QIODevice::ReadOnly));
  ASSERT_TRUE(f1.setFileTime(QDateTime::currentDateTimeUtc().addSecs(-3600), QFileDevice::FileModificationTime));
  f1.close();

  cache.put(url2, /*requestHeaders=*/{}, makeResult(std::vector<uint8_t>(600, 0x22)));
  const QString path2 = cacheEntryPathFor(tmp.path(), url2, /*range=*/"");
  ASSERT_TRUE(QFileInfo::exists(path2));

  // After pruning, only the newest entry should remain.
  EXPECT_FALSE(QFileInfo::exists(path1));
  EXPECT_TRUE(QFileInfo::exists(path2));
  EXPECT_EQ(countCacheEntries(tmp.path()), 1u);
}

} // namespace nim
