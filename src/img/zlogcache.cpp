#include "zlogcache.h"

#include <QTimer>

namespace nim {

ZLogCache& ZLogCache::instance()
{
  static ZLogCache cache;
  return cache;
}

void ZLogCache::send(google::LogSeverity severity,
                     const char* full_filename,
                     const char* base_filename,
                     int line,
                     const google::LogMessageTime& logmsgtime,
                     const char* message,
                     size_t message_len)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_logDatas.size() == m_maxNumItems) {
    m_logDatas.pop_front();
    if (m_unsendLogDataStart > 0) {
      --m_unsendLogDataStart;
    }
    ++m_logCounter;
    if (m_logCounter % 100000 == 0) {
      m_logDatas.shrink_to_fit();
    }
  }
  m_logDatas.emplace_back(severity,
                          full_filename,
                          base_filename,
                          line,
                          logmsgtime.tm(),
                          message,
                          message_len,
                          ToString(severity, base_filename, line, logmsgtime, message, message_len));
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
  std::lock_guard<std::mutex> lock(m_mutex);
  auto start = m_unsendLogDataStart;
  m_unsendLogDataStart = m_logDatas.size();
  if (m_unsendLogDataStart > start) {
    Q_EMIT logDataReady(&m_logDatas, start, m_unsendLogDataStart);
  }
}

} // namespace nim
