#include "zsqlitereadonlypool.h"

#include "zlog.h"
#include "zsqlitelrucache.h"

#include <utility>

namespace nim {

ZSqliteReadOnlyPool::ZSqliteReadOnlyPool(QString dbPath, uint64_t maxBytes, QString debugName)
  : m_dbPath(std::move(dbPath))
  , m_maxBytes(maxBytes)
  , m_debugName(std::move(debugName))
{
  m_dbPath = m_dbPath.trimmed();
  m_debugName = m_debugName.trimmed();
  if (m_debugName.isEmpty()) {
    m_debugName = QStringLiteral("<unnamed>");
  }

  if (m_dbPath.isEmpty() || m_maxBytes == 0) {
    return;
  }

  m_enabled.store(true, std::memory_order_release);
}

ZSqliteReadOnlyPool::~ZSqliteReadOnlyPool()
{
  disable();
}

bool ZSqliteReadOnlyPool::isEnabled() const
{
  return m_enabled.load(std::memory_order_acquire);
}

std::optional<ZSqliteLRUCache::GetNoTouchResult> ZSqliteReadOnlyPool::tryGetNoTouch(std::span<const std::uint8_t> keyHash)
{
  if (!isEnabled()) {
    return std::nullopt;
  }

  ZSqliteLRUCache* cache = getOrCreateCacheForThisThread();
  if (!cache) {
    return std::nullopt;
  }

  const auto res = cache->tryGetNoTouch(keyHash);

  // If the DB became unhealthy and the connection closed itself, disable the pool
  // to avoid repeated overhead on every thread.
  if (!cache->isOpen()) {
    disable();
  }

  return res;
}

ZSqliteLRUCache* ZSqliteReadOnlyPool::getOrCreateCacheForThisThread()
{
  if (!isEnabled()) {
    return nullptr;
  }

  ZSqliteLRUCache* cache = m_tlsCache.get();
  if (cache) {
    return cache;
  }

  auto newCache = std::make_unique<ZSqliteLRUCache>(m_dbPath, m_maxBytes, /*nowNsFn=*/nullptr, /*readOnly=*/true);
  if (!newCache->isOpen()) {
    LOG(WARNING) << "SQLite read pool disabled: failed to open read-only cache DB: cache=" << m_debugName
                 << " dbPath=" << m_dbPath;
    disable();
    return nullptr;
  }

  m_tlsCache.reset(std::move(newCache));
  return m_tlsCache.get();
}

void ZSqliteReadOnlyPool::disable()
{
  m_enabled.store(false, std::memory_order_release);
  // ThreadLocalPtr will clean up per-thread instances in its destructor.
}

} // namespace nim

