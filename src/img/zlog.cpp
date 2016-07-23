#include "zlog.h"

#include <QsLogDest.h>
#include <QsLogDestModel.h>
#include <cassert>
#include <QStandardPaths>

namespace nim {

void initLogging(const QString &filename)
{
  QsLogging::Logger& logger = QsLogging::Logger::instance();
  const QString sLogPath(filename);
  QsLogging::DestinationPtr fileDestination(
        QsLogging::DestinationFactory::MakeFileDestination(sLogPath, QsLogging::EnableLogRotation,
                                                           QsLogging::MaxSizeBytes(1e7), QsLogging::MaxOldLogCount(20)));
  QsLogging::DestinationPtr debugOutputDestination(
        QsLogging::DestinationFactory::MakeDebugOutputDestination());
  logger.addDestination(debugOutputDestination);
  logger.addDestination(fileDestination);
  logger.addDestination(logModelSinkInstance());
#if defined _DEBUG_
  logger.setLoggingLevel(QsLogging::DebugLevel);
#else
  logger.setLoggingLevel(QsLogging::InfoLevel);
#endif
}

LogSinkType logModelSinkInstance()
{
  static QsLogging::DestinationPtr modelDestination(new QsLogging::ModelDestination());
  return modelDestination;
}

const std::deque<QsLogging::LogMessage>& logMessages()
{
  QsLogging::ModelDestination *md = dynamic_cast<QsLogging::ModelDestination*>(logModelSinkInstance().data());
  assert(md);
  return md->logMessages();
}

LogSinkType addFileLogSink(const QString &filename)
{
  QsLogging::DestinationPtr fileDestination;
  if (!filename.isEmpty()) {
    fileDestination = QsLogging::DestinationFactory::MakeFileDestination(filename);
    QsLogging::Logger::instance().addDestination(fileDestination);
  }
  return fileDestination;
}

LogSinkType createFunctorLogSink(QsLogging::Destination::LogFunction f)
{
  return QsLogging::DestinationFactory::MakeFunctorDestination(f);
}

void addLogSink(LogSinkType sink)
{
  QsLogging::Logger::instance().addDestination(sink);
}

void removeLogSink(const LogSinkType &sink)
{
  QsLogging::Logger::instance().removeDestination(sink);
}

} // namespace nim
