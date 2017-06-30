#pragma once

#ifdef _WIN32
#undef ERROR
#endif

#define GOOGLE_STRIP_LOG 0
#include <glog/logging.h>
#include <QString>
#include <QDateTime>
#include <memory>
#include <functional>

namespace nim {

void initLogging(const char* argv0, const QString& filename);

void shutdownLogging();

using LogSink = google::LogSink;
using LogSinkPtr = std::shared_ptr<google::LogSink>;
using LogSeverity = google::LogSeverity;
const LogSeverity InfoLevel = google::GLOG_INFO;
const LogSeverity WarningLevel = google::GLOG_WARNING;
const LogSeverity ErrorLevel = google::GLOG_ERROR;
const LogSeverity FatalLevel = google::GLOG_FATAL;
const LogSeverity OffLevel = google::GLOG_FATAL + 1;

struct LogData
{
  LogData(LogSeverity severity, const char* full_filename,
          const char* base_filename, int line_,
          const ::tm* tm_time,
          const char* msg, size_t prefix_len, size_t message_len)
    : level(severity)
    , fullFilename(full_filename)
    , baseFilename(base_filename)
    , line(line_)
    , time(QDate(tm_time->tm_year + 1900, tm_time->tm_mon + 1, tm_time->tm_mday),
           QTime(tm_time->tm_hour, tm_time->tm_min, tm_time->tm_sec))
    , message(msg + prefix_len, message_len - prefix_len)
    , formatted(QString::fromUtf8(msg, message_len))
  {}

  LogSeverity level;
  QByteArray fullFilename;
  QByteArray baseFilename;
  int line;
  QDateTime time;
  QByteArray message; // main log message
  QString formatted; // formatted log message with level, time, threadid, filename, line, and message
};

using LogFunction = std::function<void(const LogData&)>;

// might return nullptr
LogSinkPtr createFileLogSink(const QString& filename);

LogSinkPtr createFunctorLogSink(LogFunction f);

inline void addLogSink(LogSink* sink)
{ if (sink) google::AddLogSink(sink); }

inline void addLogSink(LogSinkPtr sink)
{ if (sink) google::AddLogSink(sink.get()); }

inline void removeLogSink(LogSink* sink)
{ if (sink) google::RemoveLogSink(sink); }

inline void removeLogSink(LogSinkPtr sink)
{ if (sink) google::RemoveLogSink(sink.get()); }

QString levelToString(LogSeverity theLevel);

#define LINFOF(file, line) google::LogMessage(file, line, google::GLOG_INFO).stream()
#define LWARNF(file, line) google::LogMessage(file, line, google::GLOG_WARNING).stream()
#define LERRORF(file, line) google::LogMessage(file, line, google::GLOG_ERROR).stream()
#define LFATALF(file, line) google::LogMessage(file, line, google::GLOG_FATAL).stream()

inline std::ostream& operator<<(std::ostream& s, const QByteArray& q)
{ return (s << q.constData()); }

inline std::ostream& operator<<(std::ostream& s, const QString& q)
{ return (s << q.toUtf8().constData()); }

inline std::ostream& operator<<(std::ostream& s, const QStringRef& q)
{ return (s << q.toUtf8().constData()); }

} // namespace nim
