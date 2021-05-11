#include "zlogcache.h"

#include <QTimer>

namespace nim {

ZLogCache& ZLogCache::instance()
{
  static ZLogCache cache;
  return cache;
}

void ZLogCache::send(LogSeverity severity, const char* full_filename, const char* base_filename,
                     int line, const tm* tm_time, const char* message, size_t message_len, int32_t /*usecs*/,
                     size_t prefix_len)
{
  QMutexLocker lock(&m_mutex);
  if (m_logDatas.size() == m_maxNumItems) {
    m_logDatas.pop_front();
    if (m_unsendLogDataStart > 0) {
      --m_unsendLogDataStart;
    }
  }
  m_logDatas.emplace_back(severity, full_filename, base_filename, line,
                          tm_time, message, message_len, prefix_len);
}

ZLogCache::ZLogCache(size_t maxNumItems)
  : m_maxNumItems(maxNumItems)
  , m_timer(new QTimer(this))
{
  connect(m_timer, &QTimer::timeout, this, &ZLogCache::sendLogData);
  // QTimer can not be started from another thread so we use a repetitive one here
  m_timer->start(300);
}

void ZLogCache::sendLogData()
{
  QMutexLocker lock(&m_mutex);
  auto start = m_unsendLogDataStart;
  m_unsendLogDataStart = m_logDatas.size();
  if (m_unsendLogDataStart > start) {
    Q_EMIT logDataReady(&m_logDatas, start, m_unsendLogDataStart);
  }
}

} // namespace nim
