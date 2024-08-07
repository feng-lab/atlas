#include "zlog.h"

#include <QFile>
#include <utility>

namespace nim {

void initLogging(const char* argv0, const QString& filename)
{
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

  google::InstallFailureSignalHandler();

  google::InitGoogleLogging(argv0);
}

void shutdownLogging()
{
  google::ShutdownGoogleLogging();
}

class FileLogSink : public google::LogSink
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

  [[nodiscard]] bool isValid() const
  {
    return m_file.isOpen();
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
      auto str = ToString(severity, base_filename, line, time, message, message_len);
      m_file.write(str.c_str(), str.length());
      m_file.putChar('\n');
      m_file.flush();
    }
  }
};

class FunctionLogSink : public google::LogSink
{
  LogFunction m_logFunction;

public:
  explicit FunctionLogSink(LogFunction f)
    : m_logFunction(std::move(f))
  {}

  [[nodiscard]] bool isValid() const
  {
    return m_logFunction.operator bool();
  }

  // LogSink interface

public:
  void send(google::LogSeverity severity,
            const char* /*full_filename*/,
            const char* base_filename,
            int line,
            const google::LogMessageTime& logmsgtime,
            const char* message,
            size_t message_len) override
  {
    if (isValid()) {
      m_logFunction(
        LogData(severity, logmsgtime, ToString(severity, base_filename, line, logmsgtime, message, message_len)));
    }
  }
};

std::shared_ptr<google::LogSink> createFileLogSink(const QString& filename)
{
  auto res = std::make_shared<FileLogSink>(filename);
  return res->isValid() ? res : std::shared_ptr<google::LogSink>();
}

std::shared_ptr<google::LogSink> createFunctorLogSink(const LogFunction& f)
{
  auto res = std::make_shared<FunctionLogSink>(f);
  return res->isValid() ? res : std::shared_ptr<google::LogSink>();
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
static_assert(is_formattable<QByteArray>(), "QByteArray should be formattable");
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
static_assert(is_formattable<QStringView>(), "QStringView should be formattable");
static_assert(IsUtf8ArrayType<QByteArrayView>, "QByteArrayView should satisfy IsUtf8ArrayType");
static_assert(IsUtf8ArrayType<QUtf8StringView>, "QUtf8StringView should satisfy IsUtf8ArrayType");
static_assert(is_formattable<QByteArrayView>(), "QByteArrayView should be formattable");
static_assert(is_formattable<QUtf8StringView>(), "QUtf8StringView should be formattable");
#else
static_assert(is_formattable<QStringRef>(), "QStringRef should be formattable");
#endif

// Static assertions for std::ostream operator
static_assert(is_streamable<QString>(), "QString should be streamable");
static_assert(is_streamable<QByteArray>(), "QByteArray should be streamable");
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
static_assert(is_streamable<QStringView>(), "QStringView should be streamable");
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
