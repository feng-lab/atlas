//
// Created by Linqing Feng on 8/23/16.
//

#pragma once

#include "zlog.h"

#include <QObject>
#include <QMutexLocker>

#include <limits>
#include <QList>

class QTimer;

namespace nim {

class ZLogCache : public QObject, public LogSink
{
Q_OBJECT
public:
  static ZLogCache& instance();

  // remove copy and move constructors and assign operators
  ZLogCache(const ZLogCache&) = delete;             // Copy construct
  ZLogCache(ZLogCache&&) = delete;                  // Move construct
  ZLogCache& operator=(const ZLogCache&) = delete;  // Copy assign
  ZLogCache& operator=(ZLogCache&&) = delete;      // Move assign

  // LogSink interface
  virtual void send(LogSeverity severity, const char* full_filename, const char* base_filename, int line,
                    const tm* tm_time, const char* message, size_t prefix_len, size_t message_len) override;

  // receiver must be in ZLogCache's thread, which is the main gui thread
  template<typename Func1>
  QMetaObject::Connection
  receiveLogMessages(typename QtPrivate::FunctionPointer<Func1>::Object* receiver, Func1 slot) const
  {
    CHECK(this->thread() == receiver->thread()) << "receiver must be in main gui thread";
    QMutexLocker lock(&m_mutex);
    if (m_unsendLogDataStart > 0)
      (receiver->*slot)(&m_logDatas, 0, m_unsendLogDataStart);
    return QObject::connect(this, &ZLogCache::logDataReady, receiver, slot, Qt::DirectConnection);
  }

signals:
  // send the list and valid range [start, end), end is always larger than start
  void logDataReady(const QList<LogData>* messages, int start, int end);

protected:
  explicit ZLogCache(int maxNumItems = 1000000);

private:
  void sendLogData();

private:
  QList<LogData> m_logDatas;
  mutable QMutex m_mutex;
  int m_maxNumItems;
  QTimer* m_timer;
  int m_unsendLogDataStart = 0;
};

} // namespace nim

