#include "zlogcache.h"

#include <QTimer>

namespace nim {

ZLogCache& ZLogCache::instance()
{
  static ZLogCache cache;
  return cache;
}

void ZLogCache::Send(const absl::LogEntry& entry)
{
  std::scoped_lock lock(m_mutex);
  if (m_logDatas.size() == m_maxNumItems) {
    m_logDatas.shrink_to_fit();
    auto numItemsToErase = m_maxNumItems >> 4;
    m_logDatas.erase(m_logDatas.begin(), m_logDatas.begin() + numItemsToErase);
    m_unsendLogDataStart = m_unsendLogDataStart <= numItemsToErase ? 0 : m_unsendLogDataStart - numItemsToErase;
  }
  m_logDatas.emplace_back(entry);
}

ZLogCache::ZLogCache(size_t maxNumItems)
  : m_maxNumItems(maxNumItems)
  , m_timer(new QTimer(this))
{
  connect(m_timer, &QTimer::timeout, this, &ZLogCache::sendLogData);
  // QTimer can not be started from another thread so we use a repetitive one here
  m_timer->start(1000);
}

void ZLogCache::sendLogData()
{
  if (m_logDatas.size() > m_unsendLogDataStart) {
    std::scoped_lock lock(m_mutex);
    auto start = m_unsendLogDataStart;
    m_unsendLogDataStart = m_logDatas.size();
    Q_EMIT logDataReady(&m_logDatas, start, m_unsendLogDataStart);
  }
}

} // namespace nim
