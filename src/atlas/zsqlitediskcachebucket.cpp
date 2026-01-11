#include "zsqlitediskcachebucket.h"

#include "zdiskcacheutils.h"
#include "zlog.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cstring>
#include <utility>

namespace nim {

ZSqliteDiskCacheBucket::ZSqliteDiskCacheBucket(QString rootDir,
                                               QString dbFileName,
                                               uint64_t maxBytes,
                                               uint64_t asyncMaxPendingBytes,
                                               QString debugName)
  : m_rootDir(std::move(rootDir))
  , m_maxBytes(maxBytes)
{
  m_rootDir = m_rootDir.trimmed();
  const QString fileName = dbFileName.trimmed();
  const QString name = debugName.trimmed();

  if (m_rootDir.isEmpty() || fileName.isEmpty() || m_maxBytes == 0) {
    return;
  }

  const QFileInfo rootFi(m_rootDir);
  if (!rootFi.exists() || !rootFi.isDir()) {
    LOG(WARNING) << "Disk cache bucket disabled: root directory does not exist or is not a directory: root=" << m_rootDir
                 << " bucket=" << name;
    return;
  }

  m_cacheDir = atlasDiskCacheDirFromRoot(m_rootDir);
  m_dbPath = QDir(m_cacheDir).filePath(fileName);

  {
    QDir dir;
    if (!dir.mkpath(m_cacheDir)) {
      LOG(WARNING) << "Disk cache bucket disabled: failed to create directory: dir=" << m_cacheDir
                   << " bucket=" << name;
      return;
    }
  }

  auto writeCache = std::make_unique<ZSqliteLRUCache>(m_dbPath, m_maxBytes);
  if (!writeCache->isOpen()) {
    LOG(WARNING) << "Disk cache bucket disabled: failed to open SQLite DB: dbPath=" << m_dbPath
                 << " bucket=" << name;
    return;
  }

  m_readPool = std::make_shared<ZSqliteReadOnlyPool>(m_dbPath, m_maxBytes, name + QStringLiteral("_read"));

  const uint64_t configuredPendingBytes = asyncMaxPendingBytes;
  const uint64_t maxPendingBytes = std::max<uint64_t>(configuredPendingBytes, kAtlasDiskCacheAsyncMinPendingBytes);
  if (configuredPendingBytes < kAtlasDiskCacheAsyncMinPendingBytes) {
    LOG(WARNING) << "Disk cache bucket: clamping async max pending bytes from " << configuredPendingBytes << " to "
                 << kAtlasDiskCacheAsyncMinPendingBytes << " bucket=" << name;
  }

  auto writeQueue =
    std::make_shared<ZSqliteAsyncWriteQueue>(std::move(writeCache), maxPendingBytes, name);
  if (!writeQueue->isEnabled()) {
    LOG(WARNING) << "Disk cache bucket disabled: failed to start async write queue: dbPath=" << m_dbPath
                 << " bucket=" << name;
    return;
  }

  m_writeQueue = std::move(writeQueue);
  m_enabled.store(true, std::memory_order_release);
}

ZSqliteDiskCacheBucket::~ZSqliteDiskCacheBucket()
{
  m_enabled.store(false, std::memory_order_release);
  if (m_writeQueue) {
    m_writeQueue->stop();
    m_writeQueue.reset();
  }
  m_readPool.reset();
}

bool ZSqliteDiskCacheBucket::isEnabled() const
{
  return m_enabled.load(std::memory_order_acquire);
}

QString ZSqliteDiskCacheBucket::dbPath() const
{
  return m_dbPath;
}

uint64_t ZSqliteDiskCacheBucket::maxBytes() const
{
  return m_maxBytes;
}

std::optional<ZSqliteLRUCache::GetNoTouchResult> ZSqliteDiskCacheBucket::tryGetNoTouch(
  std::span<const std::uint8_t> keyHash) const
{
  if (!isEnabled() || !m_readPool || !m_readPool->isEnabled()) {
    return std::nullopt;
  }
  return m_readPool->tryGetNoTouch(keyHash);
}

void ZSqliteDiskCacheBucket::tryEnqueueErase(std::span<const std::uint8_t> keyHash) const
{
  if (!isEnabled() || !m_writeQueue) {
    return;
  }
  const auto keyArrOpt = copyKeyHash32(keyHash);
  if (!keyArrOpt.has_value()) {
    return;
  }
  (void)m_writeQueue->tryEnqueue(
    [keyArr = *keyArrOpt](ZSqliteLRUCache& cache) {
      cache.erase(std::span<const std::uint8_t>(keyArr.data(), keyArr.size()));
    },
    /*estimatedBytes=*/sizeof(KeyHash32));
}

void ZSqliteDiskCacheBucket::tryEnqueueTouchIfStale(std::span<const std::uint8_t> keyHash,
                                                    int64_t lastAccessNs,
                                                    int64_t nowNs,
                                                    std::chrono::nanoseconds minInterval) const
{
  if (!isEnabled() || !m_writeQueue) {
    return;
  }
  const auto keyArrOpt = copyKeyHash32(keyHash);
  if (!keyArrOpt.has_value()) {
    return;
  }

  const int64_t last = std::max<int64_t>(0, lastAccessNs);
  if (nowNs < last) {
    return;
  }
  const int64_t minIntervalNs = std::max<int64_t>(0, minInterval.count());
  if ((nowNs - last) < minIntervalNs) {
    return;
  }

  (void)m_writeQueue->tryEnqueue(
    [keyArr = *keyArrOpt, nowNs](ZSqliteLRUCache& cache) {
      cache.touch(std::span<const std::uint8_t>(keyArr.data(), keyArr.size()), nowNs);
    },
    /*estimatedBytes=*/sizeof(KeyHash32));
}

bool ZSqliteDiskCacheBucket::drainWrites(std::chrono::milliseconds timeout) const
{
  if (!isEnabled() || !m_writeQueue) {
    return false;
  }
  return m_writeQueue->drain(timeout);
}

std::optional<ZSqliteDiskCacheBucket::KeyHash32> ZSqliteDiskCacheBucket::copyKeyHash32(
  std::span<const std::uint8_t> keyHash)
{
  if (keyHash.size() != KeyHash32{}.size()) {
    return std::nullopt;
  }
  KeyHash32 out{};
  if (!keyHash.empty()) {
    std::memcpy(out.data(), keyHash.data(), out.size());
  }
  return out;
}

} // namespace nim
