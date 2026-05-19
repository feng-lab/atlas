#include "zlog.h"

#include "zioutils.h"
#include "zglmutils.h"
#include "zglogbridge.h"
#include "zimginterface.h"
#include "zvoxelcoordinate.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QPoint>
#include <absl/debugging/failure_signal_handler.h>
#include <absl/debugging/symbolize.h>
#include <absl/flags/flag.h>
#include <absl/log/globals.h>
#include <absl/log/log_sink_registry.h>
#include <absl/log/initialize.h>
#include <absl/time/time.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <utility>

ABSL_FLAG(bool,
          atlas_log_always_flush_files,
          true,
          "Flush Atlas file logs after every write. Set false to buffer INFO/VLOG file logs and flush on severity, "
          "size, time, explicit flush, or shutdown.");

namespace {

constinit std::atomic_bool g_logReady{false};

QString logFileRunToken(const QString& filenamePrefix)
{
  const QString logDirName = QFileInfo(filenamePrefix).dir().dirName();
  static const QString logDirSuffix = QStringLiteral("_LOG");
  if (logDirName.endsWith(logDirSuffix)) {
    QString token = logDirName;
    token.chop(logDirSuffix.size());
    if (!token.isEmpty()) {
      return token;
    }
  }
  return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss.zzz"));
}

__forceinline size_t writeUnlocked(std::FILE* file, const char* data, size_t size)
{
#if defined(_MSC_VER)
  return _fwrite_nolock(data, 1, size, file);
#elif defined(__GLIBC__)
  return ::fwrite_unlocked(data, 1, size, file);
#else
  return std::fwrite(data, 1, size, file);
#endif
}

__forceinline int flushUnlocked(std::FILE* file)
{
#if defined(_MSC_VER)
  return _fflush_nolock(file);
#elif defined(__GLIBC__)
  return ::fflush_unlocked(file);
#else
  return std::fflush(file);
#endif
}

std::string programDisplayName(std::string_view programNameOrPath)
{
  CHECK(!programNameOrPath.empty()) << "Logging program name/path must not be empty";

  const size_t separator = programNameOrPath.find_last_of("/\\");
  std::string_view basename =
    separator == std::string_view::npos ? programNameOrPath : programNameOrPath.substr(separator + 1);
  CHECK(!basename.empty()) << "Logging program name/path has an empty basename: " << programNameOrPath;

#ifdef _WIN32
  static constexpr std::string_view exeSuffix = ".exe";
  if (basename.size() > exeSuffix.size() &&
      std::equal(exeSuffix.rbegin(), exeSuffix.rend(), basename.rbegin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
      })) {
    basename.remove_suffix(exeSuffix.size());
  }
#endif

  return std::string(basename);
}

void myMessageOutput(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
  const char* file = context.file ? context.file : "Qt";
  const int line = context.line;
  switch (type) {
    case QtDebugMsg:
    case QtInfoMsg:
      LOG(INFO).AtLocation(file, line) << msg;
      break;
    case QtWarningMsg:
      LOG(WARNING).AtLocation(file, line) << msg;
      break;
    case QtCriticalMsg:
      LOG(ERROR).AtLocation(file, line) << msg;
      break;
    case QtFatalMsg:
      LOG(FATAL).AtLocation(file, line) << msg;
      break;
    default:
      break;
  }
}

} // namespace

namespace nim {

LogData::LogData(const absl::LogEntry& entry)
  : level(entry.log_severity())
  , time(absl::ToChronoTime(entry.timestamp()))
  , formatted(entry.text_message_with_prefix())
{}

class ConsoleLogSink : public absl::LogSink
{
  std::mutex m_mutex;

public:
  void Send(const absl::LogEntry& entry) override
  {
    if (entry.log_severity() >= absl::LogSeverity::kError) {
      return;
    }

    const absl::string_view message = entry.text_message_with_prefix_and_newline();
    std::scoped_lock lock(m_mutex);
    (void)writeUnlocked(stdout, message.data(), message.size());
    (void)flushUnlocked(stdout);
  }

  void Flush() override
  {
    std::scoped_lock lock(m_mutex);
    (void)flushUnlocked(stdout);
  }
};

enum class FileOpenMode
{
  Immediate,
  Lazy
};

using FileHandle = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

class FileLogWriter
{
  static constexpr size_t kBufferedFlushBytes = 1'000'000;
  static constexpr std::chrono::seconds kBufferedFlushInterval{30};

  QString m_filename;
  FileHandle m_file{nullptr, std::fclose};
  FileOpenMode m_openMode;
  size_t m_bytesSinceFlush = 0;
  std::chrono::steady_clock::time_point m_nextFlushTime;
  bool m_openFailed = false;

public:
  explicit FileLogWriter(const QString& filename, FileOpenMode openMode = FileOpenMode::Immediate)
    : m_filename(filename)
    , m_openMode(openMode)
  {
    if (m_openMode == FileOpenMode::Immediate) {
      ensureOpen(std::chrono::steady_clock::now());
    }
  }

  FileLogWriter(const FileLogWriter&) = delete;
  FileLogWriter& operator=(const FileLogWriter&) = delete;

  [[nodiscard]] bool isValid() const
  {
    return !m_openFailed && (m_openMode == FileOpenMode::Lazy || m_file != nullptr);
  }

  bool write(absl::string_view message, bool forceFlush, std::chrono::steady_clock::time_point now)
  {
    if (!ensureOpen(now)) {
      return false;
    }

    const size_t bytesWritten = writeUnlocked(m_file.get(), message.data(), message.size());
    if (bytesWritten != message.size()) {
      return false;
    }

    m_bytesSinceFlush += bytesWritten;
    if (forceFlush || m_bytesSinceFlush >= kBufferedFlushBytes || now >= m_nextFlushTime) {
      return flush(now);
    }
    return true;
  }

  bool flush(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
  {
    if (m_file == nullptr) {
      return true;
    }

    if (flushUnlocked(m_file.get()) != 0) {
      return false;
    }
    m_bytesSinceFlush = 0;
    m_nextFlushTime = now + kBufferedFlushInterval;
    return true;
  }

private:
  bool ensureOpen(std::chrono::steady_clock::time_point now)
  {
    if (m_file != nullptr) {
      return true;
    }
    if (m_openFailed) {
      return false;
    }

    try {
      m_file = openFile(m_filename, "wb");
    }
    catch (const ZException& e) {
      m_openFailed = true;
      const std::string path = m_filename.toStdString();
      std::fprintf(stderr, "Could not write log file %s, error: %s\n", path.c_str(), e.what());
      return false;
    }

    m_openFailed = m_file == nullptr;
    m_nextFlushTime = now + kBufferedFlushInterval;
    return !m_openFailed;
  }
};

class SingleFileLogSink : public absl::LogSink
{
  FileLogWriter m_file;
  std::mutex m_mutex;
  absl::LogSeverity m_minSeverity;
  bool m_alwaysFlushFiles;

public:
  explicit SingleFileLogSink(const QString& filename, absl::LogSeverity minSeverity = absl::LogSeverity::kInfo)
    : m_file(filename)
    , m_minSeverity(minSeverity)
    , m_alwaysFlushFiles(absl::GetFlag(FLAGS_atlas_log_always_flush_files))
  {}

  [[nodiscard]] bool isValid() const
  {
    return m_file.isValid();
  }

  void Send(const absl::LogEntry& entry) override
  {
    if (entry.log_severity() < m_minSeverity) {
      return;
    }

    const absl::string_view message = entry.text_message_with_prefix_and_newline();
    const bool forceFlush = m_alwaysFlushFiles || entry.log_severity() >= absl::LogSeverity::kWarning;
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(m_mutex);
    m_file.write(message, forceFlush, now);
  }

  void Flush() override
  {
    std::scoped_lock lock(m_mutex);
    m_file.flush();
  }
};

size_t severityFileIndex(absl::LogSeverity severity)
{
  switch (severity) {
    case absl::LogSeverity::kInfo:
      return 0;
    case absl::LogSeverity::kWarning:
      return 1;
    case absl::LogSeverity::kError:
      return 2;
    case absl::LogSeverity::kFatal:
      return 3;
  }
  return 0;
}

class SeverityFileLogSink : public absl::LogSink
{
  struct SeverityFileSpec
  {
    const char* severityName;
    FileOpenMode openMode;
  };

  static constexpr std::array<SeverityFileSpec, 4> severityFiles = {
    {
     {"info", FileOpenMode::Immediate},
     {"warning", FileOpenMode::Lazy},
     {"error", FileOpenMode::Lazy},
     {"fatal", FileOpenMode::Lazy},
     }
  };

  std::array<std::unique_ptr<FileLogWriter>, severityFiles.size()> m_files;
  std::mutex m_mutex;
  bool m_alwaysFlushFiles;

public:
  explicit SeverityFileLogSink(const QString& filenamePrefix)
    : m_alwaysFlushFiles(absl::GetFlag(FLAGS_atlas_log_always_flush_files))
  {
    const QString runToken = logFileRunToken(filenamePrefix);
    for (size_t i = 0; i < severityFiles.size(); ++i) {
      const SeverityFileSpec& spec = severityFiles[i];
      const QString severityName = QString::fromLatin1(spec.severityName);
      const QString logFile = filenamePrefix + '_' + severityName + '_' + runToken + QStringLiteral("_log.txt");
      m_files[i] = std::make_unique<FileLogWriter>(logFile, spec.openMode);
      if (spec.openMode == FileOpenMode::Immediate) {
        CHECK(m_files[i]->isValid()) << "Failed to create Atlas log sink: " << logFile;
      }
    }
  }

  void Send(const absl::LogEntry& entry) override
  {
    const absl::string_view message = entry.text_message_with_prefix_and_newline();
    const size_t maxIndex = severityFileIndex(entry.log_severity());
    const bool forceFlush = m_alwaysFlushFiles || entry.log_severity() >= absl::LogSeverity::kWarning;
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(m_mutex);
    for (size_t i = 0; i <= maxIndex; ++i) {
      m_files[i]->write(message, forceFlush, now);
    }
  }

  void Flush() override
  {
    std::scoped_lock lock(m_mutex);
    for (const auto& file : m_files) {
      file->flush();
    }
  }
};

const ZLogInit& ZLogInit::instance(std::string programNameOrPath, const QString& filename)
{
  static ZLogInit logInit(std::move(programNameOrPath), filename);
  return logInit;
}

bool ZLogInit::isInitialized()
{
  return g_logReady.load(std::memory_order_acquire);
}

ZLogInit::ZLogInit(std::string programNameOrPath, const QString& filename)
{
  const std::string appName = programDisplayName(programNameOrPath);

  absl::SetMinLogLevel(absl::LogSeverityAtLeast::kInfo);
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kError);

  absl::InitializeSymbolizer(programNameOrPath.c_str());
  absl::FailureSignalHandlerOptions failureOptions;
  absl::InstallFailureSignalHandler(failureOptions);

  absl::InitializeLog();

  auto consoleSink = std::make_shared<ConsoleLogSink>();
  addLogSink(consoleSink);
  m_logSinks.push_back(std::move(consoleSink));

  if (!filename.isEmpty()) {
    auto fileSink = std::make_shared<SeverityFileLogSink>(filename);
    addLogSink(fileSink);
    m_logSinks.push_back(std::move(fileSink));
  }

  installGlogToAbseilLogBridge();

  LOG(INFO) << fmt::format("--- {} Log Start ---", appName);

  // handle qt message
  qInstallMessageHandler(myMessageOutput);

  g_logReady.store(true, std::memory_order_release);
}

ZLogInit::~ZLogInit()
{
  g_logReady.store(false, std::memory_order_release);

  qInstallMessageHandler(nullptr);

  uninstallGlogToAbseilLogBridge();

  for (const auto& sink : m_logSinks) {
    if (sink) {
      sink->Flush();
    }
    removeLogSink(sink);
  }
}

std::shared_ptr<absl::LogSink> createFileLogSink(const QString& filename)
{
  if (filename.isEmpty()) {
    return {};
  }
  auto res = std::make_shared<SingleFileLogSink>(filename);
  return res->isValid() ? res : std::shared_ptr<absl::LogSink>();
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
static_assert(is_formattable<QPoint>(), "QPoint should be formattable");
static_assert(is_formattable<QPointF>(), "QPointF should be formattable");
static_assert(is_formattable<QSize>(), "QSize should be formattable");
#endif
static_assert(is_formattable<QRect>(), "QRect should be formattable");
static_assert(is_formattable<QRectF>(), "QRectF should be formattable");
static_assert(is_formattable<QStringList>(), "QStringList should be formattable");
static_assert(is_formattable<QContiguousCache<int>>(), "QContiguousCache<int> should be formattable");
static_assert(is_formattable<QSharedPointer<int>>(), "QSharedPointer<int> should be formattable");
static_assert(is_formattable<col4>(), "col4 should be formattable");
static_assert(is_formattable<ZVoxelCoordinate>(), "ZVoxelCoordinate should be formattable");
static_assert(is_formattable<glm::mat4>(), "glm::mat4 should be formattable");
static_assert(is_formattable<glm::vec3>(), "glm::vec3 should be formattable");
static_assert(is_formattable<glm::quat>(), "glm::quat should be formattable");
static_assert(is_formattable<std::vector<double>>(), "std::vector<double> should be formattable");

// Static assertions for std::ostream operator
static_assert(is_streamable<QString>(), "QString should be streamable");
static_assert(is_streamable<QStringView>(), "QStringView should be streamable");
static_assert(is_streamable<QByteArray>(), "QByteArray should be streamable");
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
static_assert(is_streamable<QByteArrayView>(), "QByteArrayView should be streamable");
static_assert(is_streamable<QUtf8StringView>(), "QUtf8StringView should be streamable");
static_assert(is_streamable<QPoint>(), "QPoint should be streamable");
static_assert(is_streamable<QPointF>(), "QPointF should be streamable");
static_assert(is_streamable<QSize>(), "QSize should be streamable");
#endif
static_assert(is_streamable<QRect>(), "QRect should be streamable");
static_assert(is_streamable<QRectF>(), "QRectF should be streamable");
static_assert(is_streamable<QStringList>(), "QStringList should be streamable");
static_assert(is_streamable<QContiguousCache<int>>(), "QContiguousCache<int> should be streamable");
static_assert(is_streamable<QSharedPointer<int>>(), "QSharedPointer<int> should be streamable");
static_assert(is_streamable<col4>(), "col4 should be streamable");
static_assert(is_streamable<ZVoxelCoordinate>(), "ZVoxelCoordinate should be streamable");
static_assert(is_streamable<glm::mat4>(), "glm::mat4 should be streamable");
static_assert(is_streamable<glm::vec3>(), "glm::vec3 should be streamable");
static_assert(is_streamable<glm::quat>(), "glm::quat should be streamable");
static_assert(is_streamable<std::vector<double>>(), "std::vector<double> should be streamable");

// Example QFlags for testing
enum class TestFlag
{
  Flag1 = 0x1,
  Flag2 = 0x2,
  Flag3 = 0x4
};
Q_DECLARE_FLAGS(TestFlags, TestFlag)

static_assert(is_formattable<TestFlags>(), "TestFlags (QFlags) should be formattable");
static_assert(is_streamable<TestFlags>(), "TestFlags (QFlags) should be streamable");

} // namespace nim
