#include "zlog.h"

#include <QStandardPaths>
#include <QFile>
#include <QTextCodec>
#include <QTextStream>
#include <QPointF>
#include <cassert>

#ifndef _USE_QSLOG_

namespace nim {

void initLogging(const char* argv0, const QString &filename)
{
  google::SetLogDestination(google::GLOG_INFO, QFile::encodeName(filename).constData());
  google::SetLogDestination(google::GLOG_ERROR, "");
  google::SetLogDestination(google::GLOG_FATAL, "");
  google::SetLogDestination(google::GLOG_WARNING, "");

  // Set whether log messages go to stderr instead of logfiles
  FLAGS_logtostderr = false;
  // Set whether log messages go to stderr in addition to logfiles.
  FLAGS_alsologtostderr = false;
  // Set color messages logged to stderr (if supported by terminal).
  FLAGS_colorlogtostderr = false;
  // Log messages at a level >= this flag are automatically sent to
  // stderr in addition to log files.
  FLAGS_stderrthreshold = google::GLOG_INFO;
  // Set whether the log prefix should be prepended to each line of output.
  FLAGS_log_prefix = true;
  // Log messages at a level <= this flag are buffered.
  // Log messages at a higher level are flushed immediately.
  FLAGS_logbuflevel = google::GLOG_INFO - 1;
  // Sets the maximum number of seconds which logs may be buffered for.
  FLAGS_logbufsecs = 30;
  // Log suppression level: messages logged at a lower level than this
  // are suppressed.
  FLAGS_minloglevel = google::GLOG_INFO;
  // If specified, logfiles are written into this directory instead of the
  // default logging directory.
  //DECLARE_string(log_dir);
  // Set the log file mode.
  //DECLARE_int32(logfile_mode);
  // Sets the path of the directory into which to put additional links
  // to the log files.
  //DECLARE_string(log_link);

  // verbose
  FLAGS_v = 0;  // in vlog_is_on.cc
  // Sets the maximum log file size (in MB).
  FLAGS_max_log_size = 1800;
  // Sets whether to avoid logging to the disk if the disk is full.
  FLAGS_stop_logging_if_full_disk = true;

  google::InitGoogleLogging(argv0);
}

void shutdownLogging()
{
  google::ShutdownGoogleLogging();
}

LogData::LogData(LogSeverity severity, const char *full_filename, const char *base_filename, int line,
                 const tm *tm_time, const char *msg, size_t message_len)
  : level(severity)
  , fullFilename(full_filename)
  , baseFilename(base_filename)
  , line(line)
  , time(QDate(tm_time->tm_year + 1900, tm_time->tm_mon + 1, tm_time->tm_mday),
         QTime(tm_time->tm_hour, tm_time->tm_min, tm_time->tm_sec))
  , message(msg, message_len)
  //, formatted(QString::fromStdString(google::LogSink::ToString(severity, base_filename, line,
  //                                                             tm_time, msg, message_len)))
{
  // from glog source code, we move back message pointer to let it point to formatted text
  const char* m = msg - 2;
  assert(m[1] == ' ');
  int numSpace = 0;
  while (numSpace != 2) {
    --m;
    if (*m == ' ')
      ++numSpace;
  }
  m -= 21;
  while (*m != 'I' && *m != 'W' && *m != 'E' && *m != 'F') {
    --m;
  }
  formatted = QString::fromUtf8(m, (msg - m) + message_len);
}

class FileLogSink : public LogSink
{
  QFile m_file;
  QTextStream m_outputStream;
public:
  explicit FileLogSink(const QString& filename)
  {
    m_file.setFileName(filename);
    if (!m_file.open(QFile::WriteOnly | QFile::Text | QFile::Append)) {
        LOG(ERROR) << "glog: could not open log file: " << filename;
    } else {
      m_outputStream.setDevice(&m_file);
      m_outputStream.setCodec(QTextCodec::codecForName("UTF-8"));
    }
  }

  inline bool isValid() const { return m_file.isOpen(); }

  // LogSink interface
public:
  virtual void send(LogSeverity severity, const char *, const char *base_filename, int line,
                    const tm *tm_time, const char *message, size_t message_len) override
  {
    if (isValid()) {
      m_outputStream << google::LogSink::ToString(severity, base_filename, line,
                                                  tm_time, message, message_len).c_str()
                     << endl;
      m_outputStream.flush();
    }
  }
};

class FunctionLogSink : public LogSink
{
  LogFunction m_logFunction;
public:
  explicit FunctionLogSink(const LogFunction& f)
    : m_logFunction(f)
  {}
  inline bool isValid() const { return m_logFunction.operator bool(); }

  // LogSink interface
public:
  virtual void send(LogSeverity severity, const char *full_filename, const char *base_filename, int line,
                    const tm *tm_time, const char *message, size_t message_len) override
  {
    if (isValid()) {
      m_logFunction(LogData(severity, full_filename, base_filename, line, tm_time, message, message_len));
    }
  }
};

LogSinkPtr createFileLogSink(const QString &filename)
{
  auto res = std::make_shared<FileLogSink>(filename);
  return res->isValid() ? res : LogSinkPtr();
}

LogSinkPtr createFunctorLogSink(LogFunction f)
{
  auto res = std::make_shared<FunctionLogSink>(f);
  return res->isValid() ? res : LogSinkPtr();
}

void addLogSink(LogSinkPtr sink)
{
  if (sink)
    google::AddLogSink(sink.get());
}

void removeLogSink(LogSinkPtr sink)
{
  if (sink)
    google::RemoveLogSink(sink.get());
}

QString levelToString(LogSeverity theLevel)
{
  switch (theLevel) {
  case INFO:
    return QObject::tr("Info");
  case WARNING:
    return QObject::tr("Warning");
  case ERROR:
    return QObject::tr("Error");
  case FATAL:
    return QObject::tr("Fatal");
  default:
    return QString();
  }
}

} // namespace nim

std::ostream &operator <<(std::ostream &s, const QPointF &v)
{
  return (s << qtTypeToQByteArray(v).constData());
}

#else

namespace nim {

void initLogging(const char*, const QString &filename)
{
  QsLogging::Logger& logger = QsLogging::Logger::instance();
  QsLogging::DestinationPtr fileDestination(
        QsLogging::DestinationFactory::MakeFileDestination(filename + "_log.txt", QsLogging::EnableLogRotation,
                                                           QsLogging::MaxSizeBytes(1e9), QsLogging::MaxOldLogCount(20)));
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

LogSinkPtr createFileLogSink(const QString &filename)
{
  QsLogging::DestinationPtr fileDestination;
  if (!filename.isEmpty()) {
    fileDestination = QsLogging::DestinationFactory::MakeFileDestination(filename);
  }
  return fileDestination->isValid() ? fileDestination : QsLogging::DestinationPtr();
}

LogSinkPtr createFunctorLogSink(QsLogging::Destination::LogFunction f)
{
  return QsLogging::DestinationFactory::MakeFunctorDestination(f);
}

void addLogSink(LogSinkPtr sink)
{
  if (sink->isValid())
    QsLogging::Logger::instance().addDestination(sink);
}

void removeLogSink(const LogSinkPtr &sink)
{
  if (sink->isValid())
    QsLogging::Logger::instance().removeDestination(sink);
}

QString levelToString(LogSeverity theLevel)
{
  return LocalizedLevelName(theLevel);
}

} // namespace nim

// support std string
QDebug operator << (QDebug s, const std::string& m)
{
  s.nospace() << m.c_str();
  return s.space();
}

QDebug operator << (QDebug s, const std::basic_string<wchar_t>& m)
{
  s.nospace() << QString::fromStdWString(m);
  return s.space();
}

#endif
