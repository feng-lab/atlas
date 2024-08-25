#include "zlog.h"

#include "zioutils.h"
#include "zlogcache.h"
#include <QFile>
#include <absl/log/log_sink_registry.h>
#include <absl/log/initialize.h>
#include <iostream>
#include <utility>

namespace nim {

class BridgeFromABSLLogging final : public absl::LogSink
{
public:
  static BridgeFromABSLLogging& instance()
  {
    static BridgeFromABSLLogging bfal;
    return bfal;
  }

  void Send(const absl::LogEntry& entry) override
  {
    switch (entry.log_severity()) {
      case absl::LogSeverity::kInfo:
        LOG(INFO) << entry.text_message_with_prefix();
        break;
      case absl::LogSeverity::kWarning:
        LOG(WARNING) << entry.text_message_with_prefix();
        break;
      case absl::LogSeverity::kError:
        LOG(ERROR) << entry.text_message_with_prefix();
        break;
      case absl::LogSeverity::kFatal:
        LOG(FATAL) << entry.text_message_with_prefix();
        break;
    }
  }
};

const ZLogInit& ZLogInit::instance(std::string appName, const QString& filename)
{
  static ZLogInit logInit(std::move(appName), filename);
  return logInit;
}

ZLogInit::ZLogInit(std::string appName, const QString& filename)
  : m_appName(std::move(appName))
{
  if (google::IsGoogleLoggingInitialized()) {
    LOG(WARNING) << "glog already initialized, will shutdown and reinitialize";
    google::ShutdownGoogleLogging();
  }

  if (filename.isEmpty()) {
    google::SetLogDestination(google::GLOG_INFO, "");
    google::SetLogDestination(google::GLOG_ERROR, "");
    google::SetLogDestination(google::GLOG_FATAL, "");
    google::SetLogDestination(google::GLOG_WARNING, "");
  } else {
    google::SetLogDestination(google::GLOG_INFO, QFile::encodeName(filename + "_info_").constData());
    google::SetLogDestination(google::GLOG_ERROR, QFile::encodeName(filename + "_error_").constData());
    google::SetLogDestination(google::GLOG_FATAL, QFile::encodeName(filename + "_fatal_").constData());
    google::SetLogDestination(google::GLOG_WARNING, QFile::encodeName(filename + "_warning_").constData());
  }

  google::SetLogFilenameExtension("_log.txt");

  // Set whether log messages go to stderr instead of logfiles
  FLAGS_logtostderr = filename.isEmpty();
  // Log messages at a level <= this flag are buffered.
  // Log messages at a higher level are flushed immediately.
  FLAGS_logbuflevel = google::GLOG_INFO - 1;
  //
  FLAGS_alsologtostderr = true;
  // Sets the maximum log file size (in MB).
  FLAGS_max_log_size = 1800;
  // Sets whether to avoid logging to the disk if the disk is full.
  FLAGS_stop_logging_if_full_disk = true;
  //
  FLAGS_log_utc_time = true;

  google::InstallFailureSignalHandler();

  google::InitGoogleLogging(m_appName.c_str());

  LOG(INFO) << fmt::format("--- {} Log Start ---", m_appName);

  absl::InitializeLog();
  absl::AddLogSink(&BridgeFromABSLLogging::instance());
}

ZLogInit::~ZLogInit()
{
  absl::RemoveLogSink(&BridgeFromABSLLogging::instance());

  google::ShutdownGoogleLogging();
}

class FileLogSink : public google::LogSink
{
  std::ofstream m_fileStream;

public:
  explicit FileLogSink(const QString& filename)
  {
    try {
      openOFStream(m_fileStream, filename, std::ios_base::out);
    }
    catch (const ZException& e) {
      LOG(ERROR) << fmt::format("glog: could not write log file {}, error: {}", filename, e.what());
    }
  }

  [[nodiscard]] bool isValid() const
  {
    return m_fileStream.is_open() && m_fileStream;
  }

  // LogSink interface

public:
  void send(google::LogSeverity severity,
            const char*,
            const char* base_filename,
            int line,
            const google::LogMessageTime& time,
            const char* message,
            size_t message_len) override
  {
    if (isValid()) {
      m_fileStream << formatLogMessage(severity, base_filename, line, time, message, message_len) << std::endl;
    }
  }
};

std::shared_ptr<google::LogSink> createFileLogSink(const QString& filename)
{
  if (filename.isEmpty()) {
    return {};
  }
  auto res = std::make_shared<FileLogSink>(filename);
  return res->isValid() ? res : std::shared_ptr<google::LogSink>();
}

void myMessageOutput(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
  switch (type) {
    case QtDebugMsg:
    case QtInfoMsg:
      google::LogMessage(context.file ? context.file : "QtFile", context.line, google::GLOG_INFO).stream() << msg;
      break;
    case QtWarningMsg:
      google::LogMessage(context.file ? context.file : "QtFile", context.line, google::GLOG_WARNING).stream() << msg;
      break;
    case QtCriticalMsg:
      google::LogMessage(context.file ? context.file : "QtFile", context.line, google::GLOG_ERROR).stream() << msg;
      break;
    case QtFatalMsg:
      google::LogMessage(context.file ? context.file : "QtFile", context.line, google::GLOG_FATAL).stream() << msg;
      break;
    default:
      break;
  }
}

void relayQtMessageToLog()
{
  qInstallMessageHandler(myMessageOutput);
}

void relayLogToQtGUI()
{
  addLogSink(&ZLogCache::instance());
}

// test code:

// Helper function to check if a type is formattable with fmt
template<typename T>
constexpr bool is_formattable()
{
  return fmt::is_formattable<T>::value;
}

// Helper function to check if a type is streamable to std::ostream
template<typename T>
constexpr bool is_streamable()
{
  return std::is_convertible_v<decltype(std::declval<std::ostream&>() << std::declval<T>()), std::ostream&>;
}

// Static assertions for fmt::formatter
static_assert(IsUtf8ArrayType<QByteArray>, "QByteArray should satisfy IsUtf8ArrayType");
static_assert(is_formattable<QString>(), "QString should be formattable");
static_assert(is_formattable<QStringView>(), "QStringView should be formattable");
static_assert(is_formattable<QByteArray>(), "QByteArray should be formattable");
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
static_assert(IsUtf8ArrayType<QByteArrayView>, "QByteArrayView should satisfy IsUtf8ArrayType");
static_assert(IsUtf8ArrayType<QUtf8StringView>, "QUtf8StringView should satisfy IsUtf8ArrayType");
static_assert(is_formattable<QByteArrayView>(), "QByteArrayView should be formattable");
static_assert(is_formattable<QUtf8StringView>(), "QUtf8StringView should be formattable");
#else
static_assert(is_formattable<QStringRef>(), "QStringRef should be formattable");
#endif

// Static assertions for std::ostream operator
static_assert(is_streamable<QString>(), "QString should be streamable");
static_assert(is_streamable<QStringView>(), "QStringView should be streamable");
static_assert(is_streamable<QByteArray>(), "QByteArray should be streamable");
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
static_assert(is_streamable<QByteArrayView>(), "QByteArrayView should be streamable");
static_assert(is_streamable<QUtf8StringView>(), "QUtf8StringView should be streamable");
#else
static_assert(is_streamable<QStringRef>(), "QStringRef should be streamable");
#endif
static_assert(is_streamable<QPoint>(), "QPoint should be streamable");
static_assert(is_streamable<QPointF>(), "QPointF should be streamable");
static_assert(is_streamable<QRect>(), "QRect should be streamable");
static_assert(is_streamable<QRectF>(), "QRectF should be streamable");
static_assert(is_streamable<QSize>(), "QSize should be streamable");
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
static_assert(is_streamable<QKeyCombination>(), "QKeyCombination should be streamable");
#endif
static_assert(is_streamable<QList<int>>(), "QList<int> should be streamable");
static_assert(is_streamable<QContiguousCache<int>>(), "QContiguousCache<int> should be streamable");
static_assert(is_streamable<QSharedPointer<int>>(), "QSharedPointer<int> should be streamable");
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
static_assert(is_streamable<QTaggedPointer<int, int>>(), "QTaggedPointer<int, int> should be streamable");
#endif

// Example QFlags for testing
enum class TestFlag
{
  Flag1 = 0x1,
  Flag2 = 0x2,
  Flag3 = 0x4
};
Q_DECLARE_FLAGS(TestFlags, TestFlag)

static_assert(is_streamable<TestFlags>(), "TestFlags (QFlags) should be streamable");

} // namespace nim
