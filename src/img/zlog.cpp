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
#include <absl/log/globals.h>
#include <absl/log/log_sink_registry.h>
#include <absl/log/initialize.h>
#include <absl/time/time.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <utility>

namespace nim {

namespace {

std::atomic_bool g_logInitialized = false;

QString fallbackLogFileRunToken()
{
  return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss.zzz"));
}

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
  return fallbackLogFileRunToken();
}

QString severityLogFilePath(const QString& filenamePrefix, const QString& severityName, const QString& runToken)
{
  CHECK(!filenamePrefix.isEmpty());
  CHECK(!severityName.isEmpty());
  CHECK(!runToken.isEmpty());

  return filenamePrefix + '_' + severityName + '_' + runToken + QStringLiteral("_log.txt");
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

std::shared_ptr<absl::LogSink> createConsoleLogSink();
std::shared_ptr<absl::LogSink> createSeverityFileLogSink(const QString& filenamePrefix);

} // namespace

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

const ZLogInit& ZLogInit::instance(std::string programNameOrPath, const QString& filename)
{
  static ZLogInit logInit(std::move(programNameOrPath), filename);
  return logInit;
}

bool ZLogInit::isInitialized()
{
  return g_logInitialized.load(std::memory_order_acquire);
}

ZLogInit::ZLogInit(std::string programNameOrPath, const QString& filename)
{
  const bool wasInitialized = g_logInitialized.exchange(true, std::memory_order_acq_rel);
  CHECK(!wasInitialized) << "ZLogInit must be initialized exactly once";

  const std::string appName = programDisplayName(programNameOrPath);

  absl::SetMinLogLevel(absl::LogSeverityAtLeast::kInfo);
  absl::SetStderrThreshold(absl::LogSeverityAtLeast::kError);

  absl::InitializeSymbolizer(programNameOrPath.c_str());
  absl::FailureSignalHandlerOptions failureOptions;
  absl::InstallFailureSignalHandler(failureOptions);

  absl::InitializeLog();

  auto consoleSink = createConsoleLogSink();
  CHECK(consoleSink) << "Failed to create Atlas console log sink";
  addLogSink(consoleSink);
  m_logSinks.push_back(std::move(consoleSink));

  if (!filename.isEmpty()) {
    auto fileSink = createSeverityFileLogSink(filename);
    CHECK(fileSink) << "Failed to create Atlas file log sink: " << filename;
    addLogSink(fileSink);
    m_logSinks.push_back(std::move(fileSink));
  }

  installGlogToAbseilLogBridge();

  LOG(INFO) << fmt::format("--- {} Log Start ---", appName);

  // handle qt message
  qInstallMessageHandler(myMessageOutput);
}

ZLogInit::~ZLogInit()
{
  qInstallMessageHandler(nullptr);

  uninstallGlogToAbseilLogBridge();

  for (const auto& sink : m_logSinks) {
    removeLogSink(sink);
  }
  g_logInitialized.store(false, std::memory_order_release);
}

LogData::LogData(const absl::LogEntry& entry)
  : level(entry.log_severity())
  , time(absl::ToChronoTime(entry.timestamp()))
  , formatted(entry.text_message_with_prefix())
{}

namespace {

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
    std::fwrite(message.data(), 1, message.size(), stdout);
  }

  void Flush() override
  {
    std::scoped_lock lock(m_mutex);
    std::fflush(stdout);
  }
};

std::shared_ptr<absl::LogSink> createConsoleLogSink()
{
  return std::make_shared<ConsoleLogSink>();
}

enum class FileOpenMode
{
  Immediate,
  Lazy
};

class FileLogWriter
{
  QString m_filename;
  std::ofstream m_fileStream;
  FileOpenMode m_openMode;
  bool m_openFailed = false;

public:
  explicit FileLogWriter(const QString& filename, FileOpenMode openMode = FileOpenMode::Immediate)
    : m_filename(filename)
    , m_openMode(openMode)
  {
    if (m_openMode == FileOpenMode::Immediate) {
      ensureOpen();
    }
  }

  [[nodiscard]] bool isValid() const
  {
    return !m_openFailed && (m_openMode == FileOpenMode::Lazy || (m_fileStream.is_open() && m_fileStream));
  }

  bool write(absl::string_view message)
  {
    if (m_openMode == FileOpenMode::Lazy && !ensureOpen()) {
      return false;
    }

    m_fileStream.write(message.data(), static_cast<std::streamsize>(message.size()));
    m_fileStream.flush();
    return static_cast<bool>(m_fileStream);
  }

  void flush()
  {
    if (!m_fileStream.is_open() || !m_fileStream) {
      return;
    }

    m_fileStream.flush();
  }

private:
  bool ensureOpen()
  {
    if (m_fileStream.is_open()) {
      return static_cast<bool>(m_fileStream);
    }
    if (m_openFailed) {
      return false;
    }

    try {
      openOFStream(m_fileStream, m_filename, std::ios_base::out | std::ios_base::binary);
    }
    catch (const ZException& e) {
      m_openFailed = true;
      const std::string path = m_filename.toStdString();
      std::fprintf(stderr, "Could not write log file %s, error: %s\n", path.c_str(), e.what());
      return false;
    }

    m_openFailed = !m_fileStream;
    return !m_openFailed;
  }
};

class SingleFileLogSink : public absl::LogSink
{
  FileLogWriter m_file;
  std::mutex m_mutex;
  absl::LogSeverity m_minSeverity;

public:
  explicit SingleFileLogSink(const QString& filename, absl::LogSeverity minSeverity = absl::LogSeverity::kInfo)
    : m_file(filename)
    , m_minSeverity(minSeverity)
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
    std::scoped_lock lock(m_mutex);
    m_file.write(message);
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

public:
  explicit SeverityFileLogSink(const QString& filenamePrefix)
  {
    const QString runToken = logFileRunToken(filenamePrefix);
    for (size_t i = 0; i < severityFiles.size(); ++i) {
      const SeverityFileSpec& spec = severityFiles[i];
      const QString logFile = severityLogFilePath(filenamePrefix, QString::fromLatin1(spec.severityName), runToken);
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
    std::scoped_lock lock(m_mutex);
    for (size_t i = 0; i <= maxIndex; ++i) {
      m_files[i]->write(message);
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

std::shared_ptr<absl::LogSink> createSeverityFileLogSink(const QString& filenamePrefix)
{
  return filenamePrefix.isEmpty() ? nullptr : std::make_shared<SeverityFileLogSink>(filenamePrefix);
}

} // namespace

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
