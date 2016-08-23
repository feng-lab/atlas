//
// Created by Linqing Feng on 8/23/16.
//

#include "zlogcache.h"

#include <QTimer>

namespace nim {

ZLogCache& ZLogCache::instance()
{
  static ZLogCache cache;
  return cache;
}

void ZLogCache::send(LogSeverity severity, const char* full_filename, const char* base_filename,
                     int line, const tm* tm_time, const char* message, size_t prefix_len, size_t message_len)
{
  QWriteLocker lock(&m_messagesLock);
  if (m_logDatas.size() == m_maxNumItems) {
    m_logDatas.pop_front();
    --m_unsendLogDataStart;
  }
  m_logDatas.push_back(LogData(severity, full_filename, base_filename, line,
                               tm_time, message, prefix_len, message_len));
}

ZLogCache::ZLogCache(int maxNumItems)
  : QObject(nullptr)
  , m_maxNumItems(maxNumItems)
  , m_timer(new QTimer(this))
{
  connect(m_timer, &QTimer::timeout, this, &ZLogCache::sendLogData);
  // QTimer can not be started from another thread so we use a repetitive one here
  m_timer->start(300);
}

void ZLogCache::sendLogData()
{
  QReadLocker lock(&m_messagesLock);
  int start = m_unsendLogDataStart;
  m_unsendLogDataStart = m_logDatas.size();
  if (m_unsendLogDataStart > start) {
    emit logDataReady(&m_logDatas, start, m_unsendLogDataStart);
  }
}

} // namespace nim
