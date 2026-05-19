#include "zbioformatsbridgeclient.h"

#include "bioformats_bridge.grpc.pb.h"
#include "bioformats_bridge.pb.h"
#include "zexception.h"
#include "zlog.h"

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <grpcpp/grpcpp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include "zcommandlineflags.h"
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

ABSL_FLAG(std::optional<std::string>,
          atlas_bioformats_java_xmx,
          std::nullopt,
          "Optional maximum Java heap for the persistent Bio-Formats bridge process. Empty means no -Xmx argument.");
ABSL_FLAG(int32_t,
          atlas_bioformats_bridge_io_timeout_ms,
          0,
          "Timeout for Bio-Formats bridge operations in milliseconds. 0 means wait indefinitely.");
ABSL_FLAG(bool,
          atlas_bioformats_bridge_diagnostics,
          false,
          "Write structured Java-side Bio-Formats bridge diagnostics to a temporary log file.");

namespace nim {

namespace {

namespace proto = atlas::bioformats::bridge;

constexpr uint32_t kMaxFrameBytes = 512_u32 * 1024_u32 * 1024_u32;
constexpr uint32_t kBridgeProtocolVersion = 1;
constexpr const char* kBioFormatsJar = "bioformats_package.jar";
constexpr const char* kAtlasBioFormatsBridgeJar = "atlas-bioformats-bridge.jar";
constexpr const char* kAtlasBioFormatsBridgeMainClass = "org.fenglab.atlas.bioformats.AtlasBioFormatsBridge";
// A cold JVM can take several seconds to answer under CI load, especially
// while many tests start at once.
constexpr int kJavaVersionProbeTimeoutMs = 30000;

struct BioFormatsRuntimeConfig
{
  QString javaExecutablePath;
  QString bridgeJarPath;
  QString bioFormatsJarPath;
};

struct JavaCandidate
{
  QString description;
  QString program;
  bool requireExecutableFile = false;
};

struct JavaCheckResult
{
  bool ok = false;
  QString detail;
};

class ZBioFormatsBridgeProcessFailure final : public ZException
{
public:
  using ZException::ZException;
};

bool bioFormatsBridgeDiagnosticsEnabled()
{
  return absl::GetFlag(FLAGS_atlas_bioformats_bridge_diagnostics);
}

std::atomic<uint64_t>& bioFormatsDiagnosticsFileCounter()
{
  static std::atomic<uint64_t> counter = 1;
  return counter;
}

QString createBioFormatsDiagnosticsFilePath()
{
  const uint64_t id = bioFormatsDiagnosticsFileCounter().fetch_add(1, std::memory_order_relaxed);
  return QDir::temp().filePath(QStringLiteral("atlas-bioformats-bridge-%1.diag").arg(id));
}

QString prepareBioFormatsDiagnosticsFile()
{
  if (!bioFormatsBridgeDiagnosticsEnabled()) {
    return {};
  }

  const QString diagnosticsFilePath = createBioFormatsDiagnosticsFilePath();
  QFile file(diagnosticsFilePath);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    throw ZException(
      fmt::format("failed to create Bio-Formats diagnostics file '{}': {}", diagnosticsFilePath, file.errorString()));
  }
  file.close();
  return diagnosticsFilePath;
}

void appendBioFormatsJavaBridgeArgs(QStringList& args, const QString& diagnosticsFilePath)
{
  if (bioFormatsBridgeDiagnosticsEnabled()) {
    args << "--verbose";
    if (!diagnosticsFilePath.isEmpty()) {
      args << (QStringLiteral("--diagnostics-file=") + diagnosticsFilePath);
    }
  }
}

template<typename Fn>
decltype(auto) retryAfterBridgeProcessRestart(std::string_view operation, Fn&& fn)
{
  try {
    return fn();
  }
  catch (const ZBioFormatsBridgeProcessFailure& e) {
    LOG(WARNING) << "Bio-Formats bridge process failed during " << operation
                 << "; restarting on demand and retrying once: " << e.what();
    return fn();
  }
}

std::mutex& bioFormatsRuntimeConfigMutex()
{
  static std::mutex mutex;
  return mutex;
}

BioFormatsRuntimeConfig& bioFormatsRuntimeConfig()
{
  static BioFormatsRuntimeConfig config;
  return config;
}

BioFormatsRuntimeConfig bioFormatsRuntimeConfigSnapshot()
{
  std::lock_guard<std::mutex> lock(bioFormatsRuntimeConfigMutex());
  return bioFormatsRuntimeConfig();
}

QStringList repeatedStringsToQStringList(const google::protobuf::RepeatedPtrField<std::string>& values)
{
  QStringList result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.push_back(QString::fromStdString(value));
  }
  return result;
}

std::string responseCaseName(const proto::Response& response)
{
  switch (response.result_case()) {
    case proto::Response::kRuntimeInfo:
      return "runtime_info";
    case proto::Response::kListFormats:
      return "list_formats";
    case proto::Response::kProbe:
      return "probe";
    case proto::Response::kDatasetInfo:
      return "dataset_info";
    case proto::Response::kPixelChunk:
      return "pixel_chunk";
    case proto::Response::kShutdown:
      return "shutdown";
    case proto::Response::kThumbnailChunk:
      return "thumbnail_chunk";
    case proto::Response::RESULT_NOT_SET:
      return "unset";
  }
  CHECK(false);
  return "unknown";
}

std::optional<int> parseJavaMajorVersion(const QString& versionOutput)
{
  const QRegularExpression quotedVersion("\"([0-9][^\"]*)\"");
  const QRegularExpressionMatch match = quotedVersion.match(versionOutput);
  if (!match.hasMatch()) {
    return std::nullopt;
  }

  const QStringList parts = match.captured(1).split('.');
  bool ok = false;
  const int first = parts.value(0).toInt(&ok);
  if (!ok) {
    return std::nullopt;
  }
  if (first == 1) {
    const int second = parts.value(1).toInt(&ok);
    if (!ok) {
      return std::nullopt;
    }
    return second;
  }
  return first;
}

QString combinedProcessOutput(QProcess& process)
{
  QString output = QString::fromUtf8(process.readAllStandardError());
  const QString stdoutText = QString::fromUtf8(process.readAllStandardOutput());
  if (!stdoutText.isEmpty()) {
    if (!output.isEmpty()) {
      output += '\n';
    }
    output += stdoutText;
  }
  return output.trimmed();
}

JavaCheckResult checkJavaCandidate(const JavaCandidate& candidate)
{
  if (candidate.requireExecutableFile && !QFileInfo(candidate.program).isExecutable()) {
    return {false, QString("%1: executable not found: %2").arg(candidate.description, candidate.program)};
  }

  QProcess process;
  process.setProgram(candidate.program);
  process.setArguments({"-version"});
  process.setProcessChannelMode(QProcess::SeparateChannels);
  process.start();
  if (!process.waitForStarted(kJavaVersionProbeTimeoutMs)) {
    return {
      false,
      QString("%1: failed to start '%2': %3").arg(candidate.description, candidate.program, process.errorString())};
  }
  if (!process.waitForFinished(kJavaVersionProbeTimeoutMs)) {
    process.kill();
    process.waitForFinished(1000);
    return {false,
            QString("%1: timed out running '%2 -version' after %3 ms")
              .arg(candidate.description, candidate.program)
              .arg(kJavaVersionProbeTimeoutMs)};
  }

  const QString output = combinedProcessOutput(process);
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    return {false,
            QString("%1: '%2 -version' exited with code %3%4")
              .arg(candidate.description, candidate.program)
              .arg(process.exitCode())
              .arg(output.isEmpty() ? QString() : QString(": ") + output)};
  }

  const std::optional<int> major = parseJavaMajorVersion(output);
  if (!major) {
    return {false,
            QString("%1: could not parse Java version from '%2 -version'%3")
              .arg(candidate.description, candidate.program)
              .arg(output.isEmpty() ? QString() : QString(": ") + output)};
  }
  if (*major < 11) {
    return {
      false,
      QString("%1: Java %2 is too old; Bio-Formats requires Java 11 or newer").arg(candidate.description).arg(*major)};
  }

  return {true, QString("%1: Java %2").arg(candidate.description).arg(*major)};
}

QString resolveJavaExecutable()
{
  const QString javaExecutablePath = bioFormatsRuntimeConfigSnapshot().javaExecutablePath;
  if (javaExecutablePath.isEmpty()) {
    throw ZException("Bio-Formats requires Java 11 or newer, but no Java executable is configured");
  }

  const JavaCandidate candidate{
    "configured Java",
    javaExecutablePath,
    true,
  };
  const JavaCheckResult check = checkJavaCandidate(candidate);
  if (check.ok) {
    return candidate.program;
  }
  throw ZException(fmt::format("Bio-Formats requires Java 11 or newer. {}", check.detail));
}

QString checkedJavaExecutablePath(const QString& javaExecutablePath)
{
  if (javaExecutablePath.isEmpty()) {
    throw ZException("Java executable path must not be empty");
  }
  const QFileInfo javaFile(javaExecutablePath);
  if (!javaFile.isExecutable()) {
    throw ZException(fmt::format("invalid Java executable path: {}", javaExecutablePath));
  }
  return javaFile.absoluteFilePath();
}

QString checkedJarPath(const QString& jarPath, const char* description)
{
  if (jarPath.isEmpty()) {
    throw ZException(fmt::format("{} path must not be empty", description));
  }

  const QFileInfo jarFile(jarPath);
  if (!jarFile.exists() || !jarFile.isFile()) {
    throw ZException(fmt::format("invalid {} path: {}", description, jarPath));
  }
  return jarFile.absoluteFilePath();
}

QString bridgeClasspath()
{
  const BioFormatsRuntimeConfig config = bioFormatsRuntimeConfigSnapshot();
#ifdef _WIN32
  constexpr QChar separator(';');
#else
  constexpr QChar separator(':');
#endif
  CHECK(!config.bioFormatsJarPath.isEmpty());
  CHECK(!config.bridgeJarPath.isEmpty());
  return config.bioFormatsJarPath + separator + config.bridgeJarPath;
}

ZBioFormatsReaderFormat convertReaderFormat(const proto::ReaderFormat& format)
{
  ZBioFormatsReaderFormat result;
  result.formatName = QString::fromStdString(format.format_name());
  result.readerClass = QString::fromStdString(format.reader_class());
  result.suffixes = repeatedStringsToQStringList(format.suffixes());
  result.domains = repeatedStringsToQStringList(format.domains());
  result.hasCompanionFiles = format.has_companion_files();
  return result;
}

ZBioFormatsResolutionInfo convertResolutionInfo(const proto::ResolutionInfo& resolution)
{
  ZBioFormatsResolutionInfo result;
  result.resolution = resolution.resolution();
  result.sizeX = resolution.size_x();
  result.sizeY = resolution.size_y();
  result.sizeZ = resolution.size_z();
  result.effectiveSizeC = resolution.effective_size_c();
  result.sizeT = resolution.size_t();
  result.imageCount = resolution.image_count();
  result.optimalTileWidth = resolution.optimal_tile_width();
  result.optimalTileHeight = resolution.optimal_tile_height();
  return result;
}

ZBioFormatsSeriesInfo convertSeriesInfo(const proto::SeriesInfo& series)
{
  ZBioFormatsSeriesInfo result;
  result.series = series.series();
  result.sizeX = series.size_x();
  result.sizeY = series.size_y();
  result.sizeZ = series.size_z();
  result.effectiveSizeC = series.effective_size_c();
  result.sizeT = series.size_t();
  result.rgbChannelCount = std::max<uint32_t>(1, series.rgb_channel_count());
  result.bytesPerPixel = series.bytes_per_pixel();
  result.pixelType = QString::fromStdString(series.pixel_type());
  result.littleEndian = series.little_endian();
  result.dimensionOrder = QString::fromStdString(series.dimension_order());
  result.resolutionCount = series.resolution_count();
  result.optimalTileWidth = series.optimal_tile_width();
  result.optimalTileHeight = series.optimal_tile_height();
  result.hasPhysicalSizeX = series.has_physical_size_x();
  result.hasPhysicalSizeY = series.has_physical_size_y();
  result.hasPhysicalSizeZ = series.has_physical_size_z();
  result.physicalSizeXUm = series.physical_size_x_um();
  result.physicalSizeYUm = series.physical_size_y_um();
  result.physicalSizeZUm = series.physical_size_z_um();
  result.channelNames = repeatedStringsToQStringList(series.channel_names());
  result.usedFiles = repeatedStringsToQStringList(series.used_files());
  result.channelColorsRgba.reserve(series.channel_colors_rgba_size());
  for (const uint32_t color : series.channel_colors_rgba()) {
    result.channelColorsRgba.push_back(color);
  }
  result.metadata.reserve(series.metadata_size());
  for (const auto& entry : series.metadata()) {
    result.metadata.push_back({entry.key(), entry.value()});
  }
  result.resolutions.reserve(series.resolutions_size());
  for (const auto& resolution : series.resolutions()) {
    result.resolutions.push_back(convertResolutionInfo(resolution));
  }
  if (result.resolutions.empty()) {
    result.resolutions.push_back({0,
                                  result.sizeX,
                                  result.sizeY,
                                  result.sizeZ,
                                  result.effectiveSizeC,
                                  result.sizeT,
                                  0,
                                  result.optimalTileWidth,
                                  result.optimalTileHeight});
  }
  return result;
}

ZBioFormatsDatasetInfo convertDatasetInfo(const proto::DatasetInfoResponse& dataset)
{
  ZBioFormatsDatasetInfo result;
  result.path = QString::fromStdString(dataset.path());
  result.formatName = QString::fromStdString(dataset.format_name());
  result.readerClass = QString::fromStdString(dataset.reader_class());
  result.usedFiles = repeatedStringsToQStringList(dataset.used_files());
  result.series.reserve(dataset.series_size());
  for (const auto& series : dataset.series()) {
    result.series.push_back(convertSeriesInfo(series));
  }
  return result;
}

} // namespace

class ZBioFormatsGrpcBridgeProcess
{
public:
  ZBioFormatsGrpcBridgeProcess()
  {
    LOG(INFO) << "Bio-Formats bridge gRPC backend enabled with one stateless Java bridge process"
              << " (--atlas_bioformats_java_xmx="
              << absl::GetFlag(FLAGS_atlas_bioformats_java_xmx).value_or(std::string{}) << ")";
  }

  ~ZBioFormatsGrpcBridgeProcess()
  {
    try {
      if (m_stub) {
        proto::Request request;
        request.set_request_id(nextRequestId());
        request.mutable_shutdown();
        (void)transact(request, "shutdown Bio-Formats gRPC bridge");
      }
      if (m_process.state() == QProcess::Running) {
        m_process.waitForFinished(5000);
      }
    }
    catch (...) {
    }
    if (m_process.state() != QProcess::NotRunning) {
      m_process.kill();
      m_process.waitForFinished(1000);
    }
  }

  [[nodiscard]] std::vector<ZBioFormatsReaderFormat> listFormats()
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    if (!m_cachedFormats.empty()) {
      return m_cachedFormats;
    }

    QElapsedTimer timer;
    timer.start();
    proto::Request request;
    request.mutable_list_formats();
    proto::Response response = transact(request, "list Bio-Formats readers");
    requireOk(response, "list Bio-Formats readers");
    if (!response.has_list_formats()) {
      throw ZException(
        fmt::format("Bio-Formats gRPC bridge returned '{}' for list_formats", responseCaseName(response)));
    }
    m_cachedFormats.reserve(response.list_formats().formats_size());
    for (const auto& format : response.list_formats().formats()) {
      m_cachedFormats.push_back(convertReaderFormat(format));
    }
    VLOG(1) << "Bio-Formats gRPC bridge listFormats took " << timer.elapsed() << " ms for " << m_cachedFormats.size()
            << " formats";
    return m_cachedFormats;
  }

  void warmUp()
  {
    QElapsedTimer timer;
    timer.start();
    retryAfterBridgeProcessRestart("warm up Bio-Formats gRPC bridge", [this]() {
      ensureProcess();
      (void)listFormats();
    });
    LOG(INFO) << "Bio-Formats gRPC bridge warmup completed in " << timer.elapsed() << " ms";
  }

  [[nodiscard]] bool canRead(const QString& filename)
  {
    QElapsedTimer timer;
    timer.start();
    proto::Request request;
    auto* probe = request.mutable_probe();
    probe->set_path(filename.toStdString());
    probe->set_grouping_policy(proto::FILE_GROUPING_POLICY_DEFAULT);
    probe->set_metadata_filtered(true);

    proto::Response response = transact(request, "probe Bio-Formats reader");
    requireOk(response, "probe Bio-Formats reader");
    if (!response.has_probe()) {
      throw ZException(fmt::format("Bio-Formats gRPC bridge returned '{}' for probe", responseCaseName(response)));
    }
    VLOG(1) << "Bio-Formats gRPC bridge probe took " << timer.elapsed() << " ms for " << filename;
    return response.probe().can_read();
  }

  [[nodiscard]] ZBioFormatsDatasetInfo readDatasetInfo(const QString& filename, bool metadataFiltered)
  {
    QElapsedTimer timer;
    timer.start();
    proto::Request request;
    auto* datasetInfo = request.mutable_dataset_info();
    datasetInfo->set_path(filename.toStdString());
    datasetInfo->set_grouping_policy(proto::FILE_GROUPING_POLICY_DEFAULT);
    datasetInfo->set_metadata_filtered(metadataFiltered);

    proto::Response response = transact(request, "read Bio-Formats dataset info");
    requireOk(response, "read Bio-Formats dataset info");
    if (!response.has_dataset_info()) {
      throw ZException(
        fmt::format("Bio-Formats gRPC bridge returned '{}' for dataset_info", responseCaseName(response)));
    }
    ZBioFormatsDatasetInfo result = convertDatasetInfo(response.dataset_info());
    VLOG(1) << "Bio-Formats gRPC bridge readDatasetInfo took " << timer.elapsed() << " ms for " << filename
            << " metadataFiltered=" << metadataFiltered;
    return result;
  }

  [[nodiscard]] std::vector<uint8_t> readRegion(const QString& filename, size_t scene, const ZImgRegion& region)
  {
    return readRegion(filename, scene, 0, region);
  }

  [[nodiscard]] std::vector<uint8_t>
  readRegion(const QString& filename, size_t scene, uint32_t resolution, const ZImgRegion& region)
  {
    QElapsedTimer timer;
    timer.start();

    proto::Request request;
    const uint64_t requestId = nextRequestId();
    request.set_request_id(requestId);
    auto* readRegion = request.mutable_read_region();
    readRegion->set_path(filename.toStdString());
    readRegion->set_grouping_policy(proto::FILE_GROUPING_POLICY_DEFAULT);
    readRegion->set_metadata_filtered(true);
    readRegion->set_series(static_cast<uint32_t>(scene));
    readRegion->set_resolution(resolution);
    readRegion->set_x(static_cast<uint64_t>(region.start.x));
    readRegion->set_y(static_cast<uint64_t>(region.start.y));
    readRegion->set_z(static_cast<uint64_t>(region.start.z));
    readRegion->set_c(static_cast<uint64_t>(region.start.c));
    readRegion->set_t(static_cast<uint64_t>(region.start.t));
    readRegion->set_width(static_cast<uint64_t>(region.end.x - region.start.x));
    readRegion->set_height(static_cast<uint64_t>(region.end.y - region.start.y));
    readRegion->set_depth(static_cast<uint64_t>(region.end.z - region.start.z));
    readRegion->set_channel_count(static_cast<uint64_t>(region.end.c - region.start.c));
    readRegion->set_time_count(static_cast<uint64_t>(region.end.t - region.start.t));

    std::vector<uint8_t> result;
    uint32_t expectedSequenceIndex = 0;
    for (const proto::Response& response : execute(request, "read Bio-Formats image region")) {
      if (response.request_id() != requestId) {
        throw ZException(fmt::format("Bio-Formats gRPC bridge returned response id {} while waiting for {}",
                                     response.request_id(),
                                     requestId));
      }
      requireOk(response, "read Bio-Formats image region");
      if (!response.has_pixel_chunk()) {
        throw ZException(
          fmt::format("Bio-Formats gRPC bridge returned '{}' while streaming pixels", responseCaseName(response)));
      }

      const auto& chunk = response.pixel_chunk();
      if (chunk.sequence_index() != expectedSequenceIndex) {
        throw ZException(fmt::format("Bio-Formats gRPC bridge pixel chunk sequence mismatch: expected {}, got {}",
                                     expectedSequenceIndex,
                                     chunk.sequence_index()));
      }
      ++expectedSequenceIndex;

      const std::string& data = chunk.data();
      result.insert(result.end(), data.begin(), data.end());
      if (chunk.total_bytes() != result.size()) {
        throw ZException(
          fmt::format("Bio-Formats gRPC bridge pixel byte count mismatch: bridge reported {}, received {}",
                      chunk.total_bytes(),
                      result.size()));
      }
      if (chunk.final_chunk()) {
        VLOG(1) << "Bio-Formats gRPC bridge readRegion took " << timer.elapsed() << " ms for " << filename
                << " bytes=" << result.size();
        return result;
      }
    }
    throw ZException("Bio-Formats gRPC bridge ended readRegion stream without a final chunk");
  }

  [[nodiscard]] ZBioFormatsThumbnail readThumbnail(const QString& filename, size_t scene, size_t z, size_t t)
  {
    QElapsedTimer timer;
    timer.start();

    proto::Request request;
    const uint64_t requestId = nextRequestId();
    request.set_request_id(requestId);
    auto* readThumbnail = request.mutable_read_thumbnail();
    readThumbnail->set_path(filename.toStdString());
    readThumbnail->set_grouping_policy(proto::FILE_GROUPING_POLICY_DEFAULT);
    readThumbnail->set_metadata_filtered(true);
    readThumbnail->set_series(static_cast<uint32_t>(scene));
    readThumbnail->set_z(static_cast<uint64_t>(z));
    readThumbnail->set_t(static_cast<uint64_t>(t));

    ZBioFormatsThumbnail result;
    uint32_t expectedSequenceIndex = 0;
    for (const proto::Response& response : execute(request, "read Bio-Formats thumbnail")) {
      if (response.request_id() != requestId) {
        throw ZException(fmt::format("Bio-Formats gRPC bridge returned response id {} while waiting for {}",
                                     response.request_id(),
                                     requestId));
      }
      requireOk(response, "read Bio-Formats thumbnail");
      if (!response.has_thumbnail_chunk()) {
        throw ZException(
          fmt::format("Bio-Formats gRPC bridge returned '{}' while streaming thumbnail", responseCaseName(response)));
      }

      const auto& chunk = response.thumbnail_chunk();
      if (chunk.sequence_index() != expectedSequenceIndex) {
        throw ZException(fmt::format("Bio-Formats gRPC bridge thumbnail chunk sequence mismatch: expected {}, got {}",
                                     expectedSequenceIndex,
                                     chunk.sequence_index()));
      }
      ++expectedSequenceIndex;

      if (expectedSequenceIndex == 1) {
        result.width = chunk.width();
        result.height = chunk.height();
        result.channelCount = chunk.channel_count();
        result.bytesPerPixel = chunk.bytes_per_pixel();
        result.pixelType = QString::fromStdString(chunk.pixel_type());
      } else if (result.width != chunk.width() || result.height != chunk.height() ||
                 result.channelCount != chunk.channel_count() || result.bytesPerPixel != chunk.bytes_per_pixel() ||
                 result.pixelType != QString::fromStdString(chunk.pixel_type())) {
        throw ZException("Bio-Formats gRPC bridge changed thumbnail metadata while streaming");
      }

      const std::string& data = chunk.data();
      result.pixels.insert(result.pixels.end(), data.begin(), data.end());
      if (chunk.total_bytes() != result.pixels.size()) {
        throw ZException(
          fmt::format("Bio-Formats gRPC bridge thumbnail byte count mismatch: bridge reported {}, received {}",
                      chunk.total_bytes(),
                      result.pixels.size()));
      }
      if (chunk.final_chunk()) {
        VLOG(1) << "Bio-Formats gRPC bridge readThumbnail took " << timer.elapsed() << " ms for " << filename
                << " bytes=" << result.pixels.size();
        return result;
      }
    }
    throw ZException("Bio-Formats gRPC bridge ended thumbnail stream without a final chunk");
  }

private:
  uint64_t nextRequestId()
  {
    const uint64_t result = m_nextRequestId.fetch_add(1, std::memory_order_relaxed);
    CHECK(result != 0);
    return result;
  }

  [[nodiscard]] QString javaExecutable()
  {
    if (m_javaExecutable.isEmpty()) {
      m_javaExecutable = resolveJavaExecutable();
    }
    return m_javaExecutable;
  }

  void ensureProcess()
  {
    std::lock_guard<std::mutex> lock(m_startMutex);
    if (m_stub && m_process.state() == QProcess::Running) {
      return;
    }
    if (m_stub || m_channel || m_process.state() != QProcess::NotRunning) {
      resetGrpcProcessLocked();
    }

    if (!ZBioFormatsBridgeClient::hasRuntimeSupport()) {
      throw ZException(fmt::format("Bio-Formats bridge runtime is incomplete. Missing: {}",
                                   ZBioFormatsBridgeClient::missingRuntimeFiles().join(", ")));
    }

    m_stdoutBuffer.clear();
    m_diagnosticsFilePath = prepareBioFormatsDiagnosticsFile();
    if (!m_diagnosticsFilePath.isEmpty()) {
      LOG(INFO) << "Bio-Formats gRPC bridge Java diagnostics log: " << m_diagnosticsFilePath;
    }

    QElapsedTimer startupTimer;
    startupTimer.start();

    QStringList args;
    const std::optional<std::string> javaXmx = absl::GetFlag(FLAGS_atlas_bioformats_java_xmx);
    if (javaXmx.has_value()) {
      args.push_back(QString("-Xmx%1").arg(QString::fromStdString(*javaXmx)));
    }
    args << "-Djava.awt.headless=true"
         << "-Dorg.slf4j.simpleLogger.logFile=System.err"
         << "-Dorg.slf4j.simpleLogger.defaultLogLevel=warn"
         << "-Dorg.slf4j.simpleLogger.log.io.grpc.netty.shaded.io.netty=warn"
         << "-cp" << bridgeClasspath() << kAtlasBioFormatsBridgeMainClass << "--grpc-port=0";
    appendBioFormatsJavaBridgeArgs(args, m_diagnosticsFilePath);

    const QString java = javaExecutable();
    m_process.setProgram(java);
    m_process.setArguments(args);
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    m_process.setStandardErrorFile(QProcess::nullDevice());
    m_process.start();
    if (!m_process.waitForStarted(30000)) {
      const QString error = m_process.errorString();
      if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
      }
      m_stdoutBuffer.clear();
      throw ZException(fmt::format("failed to start Bio-Formats gRPC bridge '{}': {}", java, error));
    }
    const qint64 startMs = startupTimer.elapsed();
    const uint16_t port = readStartupPort();
    const QString target = QStringLiteral("127.0.0.1:%1").arg(port);

    grpc::ChannelArguments channelArgs;
    channelArgs.SetMaxReceiveMessageSize(static_cast<int>(kMaxFrameBytes));
    channelArgs.SetMaxSendMessageSize(static_cast<int>(kMaxFrameBytes));
    m_channel = grpc::CreateCustomChannel(target.toStdString(), grpc::InsecureChannelCredentials(), channelArgs);
    m_stub = std::shared_ptr<proto::BioFormatsBridge::Stub>(proto::BioFormatsBridge::NewStub(m_channel));

    QElapsedTimer handshakeTimer;
    handshakeTimer.start();
    try {
      proto::Request request;
      request.mutable_runtime_info();
      const proto::Response response = transactLocked(request, "handshake with Bio-Formats gRPC bridge");
      requireOk(response, "handshake with Bio-Formats gRPC bridge");
      if (!response.has_runtime_info()) {
        throw ZException(
          fmt::format("Bio-Formats gRPC bridge returned '{}' for runtime_info", responseCaseName(response)));
      }
      const proto::RuntimeInfoResponse& info = response.runtime_info();
      if (info.protocol_version() != kBridgeProtocolVersion) {
        throw ZException(fmt::format("Bio-Formats bridge protocol mismatch: Atlas expects {}, bridge reports {}",
                                     kBridgeProtocolVersion,
                                     info.protocol_version()));
      }
      const BioFormatsRuntimeConfig runtimeConfig = bioFormatsRuntimeConfigSnapshot();
      LOG(INFO) << "Bio-Formats gRPC bridge ready: pid=" << m_process.processId() << ", java=" << java
                << ", bridgeJar=" << runtimeConfig.bridgeJarPath
                << ", bioformatsJar=" << runtimeConfig.bioFormatsJarPath << ", target=" << target
                << ", startMs=" << startMs << ", handshakeMs=" << handshakeTimer.elapsed()
                << ", protocol=" << info.protocol_version() << ", bridge=" << info.bridge_version()
                << ", bioformats=" << info.bioformats_version() << ", javaVersion=" << info.java_version()
                << ", vm=" << info.java_vm_name() << ", javaPid=" << info.process_id();
    }
    catch (...) {
      resetGrpcProcessLocked();
      throw;
    }
  }

  [[nodiscard]] uint16_t readStartupPort()
  {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < kJavaVersionProbeTimeoutMs) {
      m_stdoutBuffer.append(m_process.readAllStandardOutput());
      const qsizetype newline = m_stdoutBuffer.indexOf('\n');
      if (newline >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newline).trimmed();
        m_stdoutBuffer.remove(0, newline + 1);
        constexpr std::string_view prefix = "ATLAS_BIOFORMATS_GRPC_PORT=";
        if (!line.startsWith(QByteArray(prefix.data(), static_cast<qsizetype>(prefix.size())))) {
          throw ZException(
            fmt::format("Bio-Formats gRPC bridge printed unexpected startup line: {}", QString::fromUtf8(line)));
        }
        bool ok = false;
        const int port = line.mid(static_cast<qsizetype>(prefix.size())).toInt(&ok);
        if (!ok || port <= 0 || port > std::numeric_limits<uint16_t>::max()) {
          throw ZException(
            fmt::format("Bio-Formats gRPC bridge printed invalid startup port: {}", QString::fromUtf8(line)));
        }
        return static_cast<uint16_t>(port);
      }
      if (m_process.state() != QProcess::Running) {
        throwGrpcProcessErrorLocked("Bio-Formats gRPC bridge exited before reporting its port");
      }
      const qint64 remaining = kJavaVersionProbeTimeoutMs - timer.elapsed();
      m_process.waitForReadyRead(static_cast<int>(std::max<qint64>(1, std::min<qint64>(1000, remaining))));
    }
    throwGrpcProcessErrorLocked("timed out waiting for Bio-Formats gRPC bridge startup");
  }

  [[nodiscard]] proto::Response transact(proto::Request& request, std::string_view operation)
  {
    request.set_request_id(nextRequestId());
    proto::Response response = transactWithId(request, operation);
    if (response.request_id() != request.request_id()) {
      throw ZException(fmt::format("Bio-Formats gRPC bridge returned response id {} while waiting for {}",
                                   response.request_id(),
                                   request.request_id()));
    }
    return response;
  }

  [[nodiscard]] proto::Response transactLocked(proto::Request& request, std::string_view operation)
  {
    request.set_request_id(nextRequestId());
    proto::Response response = executeLocked(request, operation);
    if (response.request_id() != request.request_id()) {
      throw ZException(fmt::format("Bio-Formats gRPC bridge returned response id {} while waiting for {}",
                                   response.request_id(),
                                   request.request_id()));
    }
    return response;
  }

  [[nodiscard]] proto::Response transactWithId(const proto::Request& request, std::string_view operation)
  {
    std::vector<proto::Response> responses = execute(request, operation);
    if (responses.empty()) {
      throw ZException(fmt::format("Bio-Formats gRPC bridge returned no response for {}", operation));
    }
    if (responses.size() != 1) {
      throw ZException(fmt::format("Bio-Formats gRPC bridge returned {} responses for unary operation {}",
                                   responses.size(),
                                   operation));
    }
    return std::move(responses.front());
  }

  [[nodiscard]] proto::Response executeLocked(const proto::Request& request, std::string_view operation)
  {
    std::vector<proto::Response> responses = executeLockedStream(request, operation);
    if (responses.empty()) {
      throw ZException(fmt::format("Bio-Formats gRPC bridge returned no response for {}", operation));
    }
    if (responses.size() != 1) {
      throw ZException(fmt::format("Bio-Formats gRPC bridge returned {} responses for unary operation {}",
                                   responses.size(),
                                   operation));
    }
    return std::move(responses.front());
  }

  [[nodiscard]] std::vector<proto::Response> execute(const proto::Request& request, std::string_view operation)
  {
    for (int attempt = 0; attempt < 2; ++attempt) {
      try {
        const std::shared_ptr<proto::BioFormatsBridge::Stub> stub = stubForRequest();
        std::vector<proto::Response> responses = executeStreamWithStub(stub, request, operation);
        VLOG(1) << "Bio-Formats gRPC bridge request_id=" << request.request_id() << " operation=" << operation
                << " responses=" << responses.size();
        return responses;
      }
      catch (const ZBioFormatsBridgeProcessFailure& e) {
        resetGrpcProcessAfterFailure();
        if (attempt != 0) {
          throw;
        }
        LOG(WARNING) << "Bio-Formats gRPC bridge process failed during " << operation
                     << "; restarting on demand and retrying once: " << e.what();
      }
    }
    CHECK(false);
    return {};
  }

  [[nodiscard]] std::vector<proto::Response> executeLockedStream(const proto::Request& request,
                                                                 std::string_view operation)
  {
    CHECK(m_stub != nullptr);
    const std::shared_ptr<proto::BioFormatsBridge::Stub> stub = m_stub;
    try {
      std::vector<proto::Response> responses = executeStreamWithStub(stub, request, operation);
      VLOG(1) << "Bio-Formats gRPC bridge request_id=" << request.request_id() << " operation=" << operation
              << " responses=" << responses.size();
      return responses;
    }
    catch (...) {
      throw;
    }
  }

  [[nodiscard]] std::vector<proto::Response>
  executeStreamWithStub(const std::shared_ptr<proto::BioFormatsBridge::Stub>& stub,
                        const proto::Request& request,
                        std::string_view operation)
  {
    CHECK(stub != nullptr);
    grpc::ClientContext context;
    applyDeadline(context);
    std::vector<proto::Response> responses;
    std::unique_ptr<grpc::ClientReader<proto::Response>> reader = stub->Execute(&context, request);
    proto::Response response;
    while (reader->Read(&response)) {
      responses.push_back(std::move(response));
      response.Clear();
    }
    const grpc::Status status = reader->Finish();
    if (!status.ok()) {
      throw ZBioFormatsBridgeProcessFailure(fmt::format("Bio-Formats gRPC bridge failed to {}: {} ({})",
                                                        operation,
                                                        status.error_message(),
                                                        static_cast<int>(status.error_code())));
    }
    return responses;
  }

  [[nodiscard]] std::shared_ptr<proto::BioFormatsBridge::Stub> stubForRequest()
  {
    ensureProcess();
    std::lock_guard<std::mutex> lock(m_startMutex);
    CHECK(m_stub != nullptr);
    return m_stub;
  }

  void applyDeadline(grpc::ClientContext& context) const
  {
    const int32_t ioTimeoutMs = absl::GetFlag(FLAGS_atlas_bioformats_bridge_io_timeout_ms);
    if (ioTimeoutMs <= 0) {
      return;
    }
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(ioTimeoutMs));
  }

  void requireOk(const proto::Response& response, std::string_view operation) const
  {
    if (response.status() == proto::STATUS_CODE_OK) {
      return;
    }
    throw ZException(fmt::format("Bio-Formats gRPC bridge failed to {}: {} ({})",
                                 operation,
                                 response.error_message(),
                                 proto::StatusCode_Name(response.status())));
  }

  void resetGrpcProcessAfterFailure()
  {
    std::lock_guard<std::mutex> lock(m_startMutex);
    resetGrpcProcessLocked();
  }

  void resetGrpcProcessLocked()
  {
    m_stub.reset();
    m_channel.reset();
    if (m_process.state() != QProcess::NotRunning) {
      m_process.kill();
      m_process.waitForFinished(1000);
    }
    m_stdoutBuffer.clear();
    m_diagnosticsFilePath.clear();
  }

  [[noreturn]] void throwGrpcProcessErrorLocked(const char* message)
  {
    QString detail = QString::fromUtf8(message);
    if (!m_process.errorString().isEmpty()) {
      detail += QString(": ") + m_process.errorString();
    }

    resetGrpcProcessLocked();
    throw ZBioFormatsBridgeProcessFailure(detail);
  }

  std::mutex m_startMutex;
  std::mutex m_cacheMutex;
  QProcess m_process;
  QByteArray m_stdoutBuffer;
  QString m_diagnosticsFilePath;
  QString m_javaExecutable;
  std::shared_ptr<grpc::Channel> m_channel;
  std::shared_ptr<proto::BioFormatsBridge::Stub> m_stub;
  std::atomic<uint64_t> m_nextRequestId = 1;
  std::vector<ZBioFormatsReaderFormat> m_cachedFormats;
};

class ZBioFormatsBridgeClient::Impl
{
public:
  Impl()
    : m_grpc(std::make_unique<ZBioFormatsGrpcBridgeProcess>())
  {}

  [[nodiscard]] std::vector<ZBioFormatsReaderFormat> listFormats()
  {
    return m_grpc->listFormats();
  }

  void warmUp()
  {
    m_grpc->warmUp();
  }

  [[nodiscard]] bool canRead(const QString& filename)
  {
    return m_grpc->canRead(filename);
  }

  [[nodiscard]] ZBioFormatsDatasetInfo readDatasetInfo(const QString& filename, bool metadataFiltered)
  {
    return m_grpc->readDatasetInfo(filename, metadataFiltered);
  }

  [[nodiscard]] std::vector<uint8_t> readRegion(const QString& filename, size_t scene, const ZImgRegion& region)
  {
    return m_grpc->readRegion(filename, scene, region);
  }

  [[nodiscard]] std::vector<uint8_t>
  readRegion(const QString& filename, size_t scene, uint32_t resolution, const ZImgRegion& region)
  {
    return m_grpc->readRegion(filename, scene, resolution, region);
  }

  [[nodiscard]] ZBioFormatsThumbnail readThumbnail(const QString& filename, size_t scene, size_t z, size_t t)
  {
    return m_grpc->readThumbnail(filename, scene, z, t);
  }

private:
  std::unique_ptr<ZBioFormatsGrpcBridgeProcess> m_grpc;
};

namespace {

std::mutex& bioFormatsBridgeClientSingletonMutex()
{
  static std::mutex mutex;
  return mutex;
}

std::unique_ptr<ZBioFormatsBridgeClient>& bioFormatsBridgeClientSingleton()
{
  static std::unique_ptr<ZBioFormatsBridgeClient> client;
  return client;
}

void configureRuntimePath(QString BioFormatsRuntimeConfig::* field, const QString& resolvedPath)
{
  std::lock_guard<std::mutex> singletonLock(bioFormatsBridgeClientSingletonMutex());
  if (bioFormatsBridgeClientSingleton() != nullptr) {
    const BioFormatsRuntimeConfig config = bioFormatsRuntimeConfigSnapshot();
    if (config.*field == resolvedPath) {
      return;
    }
    throw ZException("Bio-Formats runtime cannot be reconfigured after the bridge has been started");
  }

  std::lock_guard<std::mutex> configLock(bioFormatsRuntimeConfigMutex());
  BioFormatsRuntimeConfig& config = bioFormatsRuntimeConfig();
  config.*field = resolvedPath;
}

} // namespace

ZBioFormatsBridgeClient& ZBioFormatsBridgeClient::instance()
{
  std::lock_guard<std::mutex> lock(bioFormatsBridgeClientSingletonMutex());
  std::unique_ptr<ZBioFormatsBridgeClient>& client = bioFormatsBridgeClientSingleton();
  if (!client) {
    if (!hasRuntimeSupport()) {
      throw ZException(
        fmt::format("Bio-Formats bridge runtime is incomplete. Missing: {}", missingRuntimeFiles().join(", ")));
    }
    client = std::make_unique<ZBioFormatsBridgeClient>();
  }
  return *client;
}

void ZBioFormatsBridgeClient::resetInstanceForTesting()
{
  std::lock_guard<std::mutex> lock(bioFormatsBridgeClientSingletonMutex());
  bioFormatsBridgeClientSingleton().reset();
}

bool ZBioFormatsBridgeClient::hasRuntimeSupport()
{
  return missingRuntimeFiles().empty();
}

QStringList ZBioFormatsBridgeClient::missingRuntimeFiles()
{
  QStringList missing;
  const BioFormatsRuntimeConfig config = bioFormatsRuntimeConfigSnapshot();
  if (config.javaExecutablePath.isEmpty()) {
    missing.push_back("Java executable");
  } else if (!QFileInfo(config.javaExecutablePath).isExecutable()) {
    missing.push_back(config.javaExecutablePath);
  }
  if (config.bridgeJarPath.isEmpty()) {
    missing.push_back(kAtlasBioFormatsBridgeJar);
  } else if (!QFileInfo(config.bridgeJarPath).isFile()) {
    missing.push_back(config.bridgeJarPath);
  }
  if (config.bioFormatsJarPath.isEmpty()) {
    missing.push_back(kBioFormatsJar);
  } else if (!QFileInfo(config.bioFormatsJarPath).isFile()) {
    missing.push_back(config.bioFormatsJarPath);
  }
  return missing;
}

void ZBioFormatsBridgeClient::configureJavaExecutablePath(const QString& javaExecutablePath)
{
  const QString resolvedJavaExecutablePath = checkedJavaExecutablePath(javaExecutablePath);
  configureRuntimePath(&BioFormatsRuntimeConfig::javaExecutablePath, resolvedJavaExecutablePath);
}

void ZBioFormatsBridgeClient::configureBridgeJarPath(const QString& bridgeJarPath)
{
  const QString resolvedBridgeJarPath = checkedJarPath(bridgeJarPath, "Atlas Bio-Formats bridge jar");
  configureRuntimePath(&BioFormatsRuntimeConfig::bridgeJarPath, resolvedBridgeJarPath);
}

void ZBioFormatsBridgeClient::configureBioFormatsJarPath(const QString& bioFormatsJarPath)
{
  const QString resolvedBioFormatsJarPath = checkedJarPath(bioFormatsJarPath, "Bio-Formats jar");
  configureRuntimePath(&BioFormatsRuntimeConfig::bioFormatsJarPath, resolvedBioFormatsJarPath);
}

QString ZBioFormatsBridgeClient::javaExecutablePath()
{
  return bioFormatsRuntimeConfigSnapshot().javaExecutablePath;
}

QString ZBioFormatsBridgeClient::bridgeJarPath()
{
  return bioFormatsRuntimeConfigSnapshot().bridgeJarPath;
}

QString ZBioFormatsBridgeClient::bioFormatsJarPath()
{
  return bioFormatsRuntimeConfigSnapshot().bioFormatsJarPath;
}

ZBioFormatsBridgeClient::ZBioFormatsBridgeClient()
  : m_impl(std::make_unique<Impl>())
{}

ZBioFormatsBridgeClient::~ZBioFormatsBridgeClient() = default;

std::vector<ZBioFormatsReaderFormat> ZBioFormatsBridgeClient::listFormats()
{
  return m_impl->listFormats();
}

bool ZBioFormatsBridgeClient::canRead(const QString& filename)
{
  return m_impl->canRead(filename);
}

ZBioFormatsDatasetInfo ZBioFormatsBridgeClient::readDatasetInfo(const QString& filename, bool metadataFiltered)
{
  return m_impl->readDatasetInfo(filename, metadataFiltered);
}

std::vector<uint8_t>
ZBioFormatsBridgeClient::readRegion(const QString& filename, size_t scene, const ZImgRegion& region)
{
  return m_impl->readRegion(filename, scene, region);
}

std::vector<uint8_t> ZBioFormatsBridgeClient::readRegion(const QString& filename,
                                                         size_t scene,
                                                         uint32_t resolution,
                                                         const ZImgRegion& region)
{
  return m_impl->readRegion(filename, scene, resolution, region);
}

ZBioFormatsThumbnail ZBioFormatsBridgeClient::readThumbnail(const QString& filename, size_t scene, size_t z, size_t t)
{
  return m_impl->readThumbnail(filename, scene, z, t);
}

void ZBioFormatsBridgeClient::warmUp()
{
  m_impl->warmUp();
}

} // namespace nim
