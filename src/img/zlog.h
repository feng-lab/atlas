#ifndef ZLOG_H
#define ZLOG_H

#include <QsLog.h>
#include <deque>

namespace nim {

void initLogging(const QString &filename);

typedef QsLogging::Destination LogSink;
typedef QsLogging::DestinationPtr LogSinkPtr;
typedef QsLogging::LogMessage LogMessage;
typedef QsLogging::Level LogSeverity;
const LogSeverity FatalLevel   = QsLogging::FatalLevel;
const LogSeverity ErrorLevel   = QsLogging::ErrorLevel;
const LogSeverity WarnLevel    = QsLogging::WarnLevel;
const LogSeverity InfoLevel    = QsLogging::InfoLevel;
const LogSeverity DebugLevel   = QsLogging::DebugLevel;
const LogSeverity TraceLevel   = QsLogging::TraceLevel;
const LogSeverity OffLevel   = QsLogging::OffLevel;

LogSinkPtr addFileLogSink(const QString &filename);
LogSinkPtr createFunctorLogSink(QsLogging::Destination::LogFunction f);
void addLogSink(LogSinkPtr sink);
void removeLogSink(const LogSinkPtr& sink);

// container
template<class IteratorType>
void logContainer(QsLogging::Level severity, const IteratorType &begin, const IteratorType &end,
                  int numberPerLine = 10, const QString &name = "", const QString &delimiter = "")
{
  if (QsLogging::Logger::instance().loggingLevel() > severity)
    return;
  if (severity == InfoLevel)
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
  if (severity == InfoLevel)
    LINFO() << "End container" << name;
  else
    LOG(severity) << "End container" << name ;
}

} // namespace nim

#endif // ZLOG_H
