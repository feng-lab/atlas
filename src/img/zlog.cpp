#include "zlog.h"

#include <cassert>
#include <QStandardPaths>

namespace nim {

void initLogging(const QString &filename)
{
  QsLogging::Logger& logger = QsLogging::Logger::instance();
  QsLogging::DestinationPtr fileDestination(
        QsLogging::DestinationFactory::MakeFileDestination(filename, QsLogging::EnableLogRotation,
                                                           QsLogging::MaxSizeBytes(1e7), QsLogging::MaxOldLogCount(20)));
  QsLogging::DestinationPtr debugOutputDestination(
        QsLogging::DestinationFactory::MakeDebugOutputDestination());
  logger.addDestination(debugOutputDestination);
  logger.addDestination(fileDestination);
#if defined _DEBUG_
  logger.setLoggingLevel(QsLogging::DebugLevel);
#else
  logger.setLoggingLevel(QsLogging::InfoLevel);
#endif
}

LogSinkPtr addFileLogSink(const QString &filename)
{
  QsLogging::DestinationPtr fileDestination;
  if (!filename.isEmpty()) {
    fileDestination = QsLogging::DestinationFactory::MakeFileDestination(filename);
    QsLogging::Logger::instance().addDestination(fileDestination);
  }
  return fileDestination;
}

LogSinkPtr createFunctorLogSink(QsLogging::Destination::LogFunction f)
{
  return QsLogging::DestinationFactory::MakeFunctorDestination(f);
}

void addLogSink(LogSinkPtr sink)
{
  QsLogging::Logger::instance().addDestination(sink);
}

void removeLogSink(const LogSinkPtr &sink)
{
  QsLogging::Logger::instance().removeDestination(sink);
}

} // namespace nim
