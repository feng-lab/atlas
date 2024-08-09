#pragma once

#include "zlog.h"
#include <QObject>
#include <deque>
#include <limits>
#include <mutex>

class QTimer;

namespace nim {

class ZLogCache
  : public QObject
  , public google::LogSink
{
  Q_OBJECT

public:
  static ZLogCache& instance();

  // remove copy and move constructors and assign operators
  ZLogCache(const ZLogCache&) = delete; // Copy construct
  ZLogCache(ZLogCache&&) = delete; // Move construct
  ZLogCache& operator=(const ZLogCache&) = delete; // Copy assign
  ZLogCache& operator=(ZLogCache&&) = delete; // Move assign

  // LogSink interface
  void send(google::LogSeverity severity,
            const char* full_filename,
            const char* base_filename,
            int line,
            const google::LogMessageTime& logmsgtime,
            const char* message,
            size_t message_len) override;

  // receiver must be in ZLogCache's thread, which is the main gui thread
  template<typename Func1>
  QMetaObject::Connection receiveLogMessages(typename QtPrivate::FunctionPointer<Func1>::Object* receiver,
                                             Func1 slot,
                                             bool receiveOldMessages = true) const
  {
    CHECK(this->thread() == receiver->thread()) << "receiver must be in main gui thread";
    if (receiveOldMessages && m_unsendLogDataStart > 0) {
      (receiver->*slot)(&m_logDatas, 0, m_unsendLogDataStart);
    }
    return QObject::connect(this, &ZLogCache::logDataReady, receiver, slot, Qt::DirectConnection);
  }

Q_SIGNALS:
  // send the list and valid range [start, end), end is always larger than start
  void logDataReady(const std::deque<LogData>* messages, size_t start, size_t end);

protected:
  explicit ZLogCache(size_t maxNumItems = 1 << 20);

private:
  void sendLogData();

private:
  std::deque<LogData> m_logDatas;
  std::mutex m_mutex;
  size_t m_maxNumItems;
  QTimer* m_timer;
  size_t m_unsendLogDataStart = 0;
};

} // namespace nim
