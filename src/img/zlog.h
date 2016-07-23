#ifndef ZLOG_H
#define ZLOG_H

#include <QsLog.h>
#include <deque>

namespace nim {

void initLogging(const QString &filename);

typedef QsLogging::DestinationPtr LogSinkType;
typedef QsLogging::LogMessage LogMessageType;
LogSinkType logModelSinkInstance();
const std::deque<QsLogging::LogMessage>& logMessages();

LogSinkType addFileLogSink(const QString &filename);
LogSinkType createFunctorLogSink(QsLogging::Destination::LogFunction f);
void addLogSink(LogSinkType sink);
void removeLogSink(const LogSinkType& sink);

// container
template<class IteratorType>
void logContainer(QsLogging::Level severity, const IteratorType &begin, const IteratorType &end,
                  int numberPerLine = 10, const QString &name = "", const QString &delimiter = "")
{
  if (QsLogging::Logger::instance().loggingLevel() > severity)
    return;
  if (severity == INFO)
    LINFO() << "Start container" << name;
  else
    LOG(severity) << "Start container" << name;
  IteratorType it = begin;
  while (it != end) {
    QsLogging::Logger::Helper helper(severity);
    int num = 0;
    while (num < numberPerLine && it != end) {
      helper.stream() << qPrintable(QString("%1%2").arg(*it).arg(delimiter));
      ++it;
      ++num;
    }
  }
  if (severity == INFO)
    LINFO() << "End container" << name;
  else
    LOG(severity) << "End container" << name ;
}

} // namespace nim

#endif // ZLOG_H
