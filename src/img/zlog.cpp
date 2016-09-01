#include "zlog.h"

#include <QFile>

namespace nim {

void initLogging(const char* argv0, const QString& filename)
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

  google::InstallFailureSignalHandler();

  google::InitGoogleLogging(argv0);
}

void shutdownLogging()
{
  google::ShutdownGoogleLogging();
}

class FileLogSink : public LogSink
{
  QFile m_file;
public:
  explicit FileLogSink(const QString& filename)
  {
    m_file.setFileName(filename);
    if (!m_file.open(QFile::WriteOnly | QFile::Text)) {
      LOG(ERROR) << "glog: could not open log file: " << filename;
    }
  }

  inline bool isValid() const
  { return m_file.isOpen(); }

  // LogSink interface
public:
  virtual void send(LogSeverity /*severity*/, const char* /*full_filename*/, const char* /*base_filename*/,
                    int /*line*/, const ::tm* /*tm_time*/, const char* message,
                    size_t /*prefix_len*/, size_t message_len) override
  {
    if (isValid()) {
      m_file.write(message, message_len + 1);  // glog: after message_len is '\n'
      m_file.flush();
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

  inline bool isValid() const
  { return m_logFunction.operator bool(); }

  // LogSink interface
public:
  virtual void send(LogSeverity severity, const char* full_filename, const char* base_filename, int line,
                    const tm* tm_time, const char* message, size_t prefix_len, size_t message_len) override
  {
    if (isValid()) {
      m_logFunction(LogData(severity, full_filename, base_filename, line, tm_time, message, prefix_len, message_len));
    }
  }
};

LogSinkPtr createFileLogSink(const QString& filename)
{
  auto res = std::make_shared<FileLogSink>(filename);
  return res->isValid() ? res : LogSinkPtr();
}

LogSinkPtr createFunctorLogSink(LogFunction f)
{
  auto res = std::make_shared<FunctionLogSink>(f);
  return res->isValid() ? res : LogSinkPtr();
}

QString levelToString(LogSeverity theLevel)
{
  switch (theLevel) {
    case google::GLOG_INFO:
      return QObject::tr("Info");
    case google::GLOG_WARNING:
      return QObject::tr("Warning");
    case google::GLOG_ERROR:
      return QObject::tr("Error");
    case google::GLOG_FATAL:
      return QObject::tr("Fatal");
    default:
      return QString("Unknown");
  }
}

} // namespace nim
