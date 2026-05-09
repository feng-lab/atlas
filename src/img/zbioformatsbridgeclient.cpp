#include "zbioformatsbridgeclient.h"

#include "bioformats_bridge.grpc.pb.h"
#include "bioformats_bridge.pb.h"
#include "zexception.h"
#include "zcpuinfo.h"
#include "zimginterface.h"
#include "zlog.h"

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <grpcpp/grpcpp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <gflags/gflags.h>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

DEFINE_string(atlas_bioformats_java_xmx,
              "2g",
              "Maximum Java heap for the persistent Bio-Formats bridge process. Empty means no -Xmx argument.");
DEFINE_int32(atlas_bioformats_bridge_io_timeout_ms,
             0,
             "Timeout for Bio-Formats bridge operations in milliseconds. 0 means wait indefinitely.");
DEFINE_int32(
  atlas_bioformats_bridge_worker_count,
  0,
  "Number of Bio-Formats bridge workers used for pixel reads. 0 means auto: the gRPC backend uses hardware "
  "concurrency, while the stdio backend chooses from hardware concurrency and a 2 GiB per-JVM memory budget. On "
  "gRPC, workers are independent Bio-Formats reader instances inside one Java process; on stdio, workers are "
  "independent Java sidecars.");
DEFINE_bool(atlas_bioformats_bridge_use_grpc,
            true,
            "Use the single-JVM gRPC Bio-Formats bridge. Set false to use the stdio multi-process bridge.");

namespace nim {

namespace {

namespace proto = atlas::bioformats::bridge;

constexpr uint32_t kMaxFrameBytes = 512_u32 * 1024_u32 * 1024_u32;
constexpr uint32_t kBridgeProtocolVersion = 1;
constexpr const char* kBioFormatsJar = "bioformats_package.jar";
constexpr const char* kAtlasBioFormatsBridgeJar = "atlas-bioformats-bridge.jar";
// This is used only for system Java discovery. A cold JVM can take several
// seconds to answer under CI load, especially while many tests start at once.
constexpr int kJavaVersionProbeTimeoutMs = 30000;
constexpr uint64_t kAutoBridgeWorkerHeapBudgetBytes = 2_u64 * 1024_u64 * 1024_u64 * 1024_u64;

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

QString canonicalPath(const QString& filename)
{
  QFileInfo fi(filename);
  if (!fi.exists()) {
    throw ZException(fmt::format("Bio-Formats input file does not exist: {}", filename));
  }
  QString path = fi.canonicalFilePath();
  if (path.isEmpty()) {
    path = fi.absoluteFilePath();
  }
  return path;
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
    case proto::Response::kOpenDataset:
      return "open_dataset";
    case proto::Response::kPixelChunk:
      return "pixel_chunk";
    case proto::Response::kCloseDataset:
      return "close_dataset";
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

QString platformJavaExecutableName()
{
#ifdef _WIN32
  return "java.exe";
#else
  return "java";
#endif
}

QString javaExecutableInHome(const QString& javaHome)
{
  return QDir(javaHome).absoluteFilePath(QString("bin/") + platformJavaExecutableName());
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
  const QString bundledJreDir = ZImgGlobal::instance().jreDIR;
  if (!bundledJreDir.isEmpty()) {
    const QString bundledJava = javaExecutableInHome(bundledJreDir);
    if (!QFileInfo(bundledJava).isExecutable()) {
      throw ZException(
        fmt::format("Bundled Bio-Formats Java runtime is incomplete. java executable not found: {}", bundledJava));
    }
    return bundledJava;
  }

  QStringList diagnostics;
  const QString javaHome = qEnvironmentVariable("JAVA_HOME");
  if (javaHome.isEmpty()) {
    diagnostics.push_back("JAVA_HOME: not set");
  } else {
    const JavaCandidate candidate{
      QString("JAVA_HOME=%1").arg(javaHome),
      javaExecutableInHome(javaHome),
      true,
    };
    const JavaCheckResult check = checkJavaCandidate(candidate);
    if (check.ok) {
      return candidate.program;
    }
    diagnostics.push_back(check.detail);
  }

  const JavaCandidate pathCandidate{
    "PATH java",
    platformJavaExecutableName(),
    false,
  };
  const JavaCheckResult pathCheck = checkJavaCandidate(pathCandidate);
  if (pathCheck.ok) {
    return pathCandidate.program;
  }
  diagnostics.push_back(pathCheck.detail);

  const QString detail = QString("Bio-Formats requires Java 11 or newer. Tried:\n- ") + diagnostics.join("\n- ");
  throw ZException(detail);
}

QString bridgeClasspath()
{
  QDir jarsDir(ZImgGlobal::instance().jarsDIR);
#ifdef _WIN32
  constexpr QChar separator(';');
#else
  constexpr QChar separator(':');
#endif
  return jarsDir.absoluteFilePath(kBioFormatsJar) + separator + jarsDir.absoluteFilePath(kAtlasBioFormatsBridgeJar);
}

size_t resolveBioFormatsBridgeWorkerCount()
{
  if (FLAGS_atlas_bioformats_bridge_worker_count < 0) {
    throw ZException("--atlas_bioformats_bridge_worker_count must be >= 0");
  }
  if (FLAGS_atlas_bioformats_bridge_worker_count > 0) {
    return static_cast<size_t>(FLAGS_atlas_bioformats_bridge_worker_count);
  }

  const size_t cpuWorkers = std::max<size_t>(1, std::thread::hardware_concurrency());
  if (FLAGS_atlas_bioformats_bridge_use_grpc) {
    return cpuWorkers;
  }

  const size_t memoryWorkers =
    std::max<size_t>(1, ZCpuInfo::instance().nPhysicalRAM / kAutoBridgeWorkerHeapBudgetBytes);
  return std::min(cpuWorkers, memoryWorkers);
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

ZBioFormatsDatasetInfo convertDatasetInfo(const proto::OpenDatasetResponse& dataset)
{
  ZBioFormatsDatasetInfo result;
  result.sessionId = dataset.session_id();
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

class ZBioFormatsBridgeProcess
{
public:
  explicit ZBioFormatsBridgeProcess(size_t workerIndex)
    : m_workerIndex(workerIndex)
  {}

  [[nodiscard]] std::vector<ZBioFormatsReaderFormat> listFormats()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool cached = !m_cachedFormats.empty();
    QElapsedTimer timer;
    timer.start();
    auto result = listFormatsLocked();
    if (!cached) {
      VLOG(1) << "Bio-Formats bridge worker " << m_workerIndex << " listFormats took " << timer.elapsed() << " ms for "
              << result.size() << " formats";
    }
    return result;
  }

  void warmUp()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    QElapsedTimer timer;
    timer.start();
    ensureProcess();
    (void)listFormatsLocked();
    LOG(INFO) << "Bio-Formats bridge worker " << m_workerIndex << " warmup completed in " << timer.elapsed() << " ms";
  }

  [[nodiscard]] bool canRead(const QString& filename)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    QElapsedTimer timer;
    timer.start();
    proto::Request request;
    auto* probe = request.mutable_probe();
    probe->set_path(canonicalPath(filename).toStdString());
    probe->set_grouping_policy(proto::FILE_GROUPING_POLICY_DEFAULT);
    probe->set_metadata_filtered(true);

    proto::Response response = transact(request);
    requireOk(response, "probe Bio-Formats reader");
    if (!response.has_probe()) {
      throw ZException(fmt::format("Bio-Formats bridge returned '{}' for probe", responseCaseName(response)));
    }
    VLOG(1) << "Bio-Formats bridge worker " << m_workerIndex << " probe took " << timer.elapsed() << " ms for "
            << filename;
    return response.probe().can_read();
  }

  [[nodiscard]] ZBioFormatsDatasetInfo openDataset(const QString& filename)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const QString path = canonicalPath(filename);
    const bool cached = m_datasetsByPath.contains(path);
    QElapsedTimer timer;
    timer.start();
    const ZBioFormatsDatasetInfo result = openDatasetByPathLocked(path);
    if (!cached) {
      VLOG(1) << "Bio-Formats bridge worker " << m_workerIndex << " openDataset took " << timer.elapsed() << " ms for "
              << filename;
    }
    return result;
  }

  [[nodiscard]] std::vector<uint8_t> readRegion(const QString& filename, size_t scene, const ZImgRegion& region)
  {
    return readRegion(filename, scene, 0, region);
  }

  [[nodiscard]] std::vector<uint8_t>
  readRegion(const QString& filename, size_t scene, uint32_t resolution, const ZImgRegion& region)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    QElapsedTimer timer;
    timer.start();
    const auto& dataset = openDatasetLocked(filename);
    if (scene >= dataset.series.size()) {
      throw ZException(fmt::format("Bio-Formats scene {} is out of range for {}", scene, filename));
    }

    proto::Request request;
    const uint64_t requestId = nextRequestId();
    request.set_request_id(requestId);
    auto* readRegion = request.mutable_read_region();
    readRegion->set_session_id(dataset.sessionId);
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

    writeRequest(request);

    std::vector<uint8_t> result;
    uint32_t expectedSequenceIndex = 0;
    while (true) {
      proto::Response response = readResponse();
      if (response.request_id() != requestId) {
        throw ZException(fmt::format("Bio-Formats bridge returned response id {} while waiting for {}",
                                     response.request_id(),
                                     requestId));
      }
      requireOk(response, "read Bio-Formats image region");
      if (!response.has_pixel_chunk()) {
        throw ZException(
          fmt::format("Bio-Formats bridge returned '{}' while streaming pixels", responseCaseName(response)));
      }

      const auto& chunk = response.pixel_chunk();
      if (chunk.sequence_index() != expectedSequenceIndex) {
        throw ZException(fmt::format("Bio-Formats bridge pixel chunk sequence mismatch: expected {}, got {}",
                                     expectedSequenceIndex,
                                     chunk.sequence_index()));
      }
      ++expectedSequenceIndex;

      const std::string& data = chunk.data();
      result.insert(result.end(), data.begin(), data.end());
      if (chunk.total_bytes() != result.size()) {
        throw ZException(fmt::format("Bio-Formats bridge pixel byte count mismatch: bridge reported {}, received {}",
                                     chunk.total_bytes(),
                                     result.size()));
      }
      if (chunk.final_chunk()) {
        VLOG(1) << "Bio-Formats bridge worker " << m_workerIndex << " readRegion took " << timer.elapsed() << " ms for "
                << filename << " bytes=" << result.size();
        return result;
      }
    }
  }

  [[nodiscard]] ZBioFormatsThumbnail readThumbnail(const QString& filename, size_t scene, size_t z, size_t t)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    QElapsedTimer timer;
    timer.start();
    const auto& dataset = openDatasetLocked(filename);
    if (scene >= dataset.series.size()) {
      throw ZException(fmt::format("Bio-Formats scene {} is out of range for {}", scene, filename));
    }

    proto::Request request;
    const uint64_t requestId = nextRequestId();
    request.set_request_id(requestId);
    auto* readThumbnail = request.mutable_read_thumbnail();
    readThumbnail->set_session_id(dataset.sessionId);
    readThumbnail->set_series(static_cast<uint32_t>(scene));
    readThumbnail->set_z(static_cast<uint64_t>(z));
    readThumbnail->set_t(static_cast<uint64_t>(t));

    writeRequest(request);

    ZBioFormatsThumbnail result;
    uint32_t expectedSequenceIndex = 0;
    while (true) {
      proto::Response response = readResponse();
      if (response.request_id() != requestId) {
        throw ZException(fmt::format("Bio-Formats bridge returned response id {} while waiting for {}",
                                     response.request_id(),
                                     requestId));
      }
      requireOk(response, "read Bio-Formats thumbnail");
      if (!response.has_thumbnail_chunk()) {
        throw ZException(
          fmt::format("Bio-Formats bridge returned '{}' while streaming thumbnail", responseCaseName(response)));
      }

      const auto& chunk = response.thumbnail_chunk();
      if (chunk.sequence_index() != expectedSequenceIndex) {
        throw ZException(fmt::format("Bio-Formats bridge thumbnail chunk sequence mismatch: expected {}, got {}",
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
        throw ZException("Bio-Formats bridge changed thumbnail metadata while streaming");
      }

      const std::string& data = chunk.data();
      result.pixels.insert(result.pixels.end(), data.begin(), data.end());
      if (chunk.total_bytes() != result.pixels.size()) {
        throw ZException(
          fmt::format("Bio-Formats bridge thumbnail byte count mismatch: bridge reported {}, received {}",
                      chunk.total_bytes(),
                      result.pixels.size()));
      }
      if (chunk.final_chunk()) {
        VLOG(1) << "Bio-Formats bridge worker " << m_workerIndex << " readThumbnail took " << timer.elapsed()
                << " ms for " << filename << " bytes=" << result.pixels.size();
        return result;
      }
    }
  }

  void closeDataset(const QString& filename)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    QElapsedTimer timer;
    timer.start();
    const QString path = canonicalPath(filename);
    auto it = m_datasetsByPath.find(path);
    if (it == m_datasetsByPath.end()) {
      return;
    }

    proto::Request request;
    request.mutable_close_dataset()->set_session_id(it.value().sessionId);
    proto::Response response = transact(request);
    requireOk(response, "close Bio-Formats dataset");
    m_datasetsByPath.erase(it);
    VLOG(1) << "Bio-Formats bridge worker " << m_workerIndex << " closeDataset took " << timer.elapsed() << " ms for "
            << filename;
  }

  ~ZBioFormatsBridgeProcess()
  {
    try {
      if (m_process.state() == QProcess::Running) {
        proto::Request request;
        request.set_request_id(nextRequestId());
        request.mutable_shutdown();
        writeRequest(request);
        (void)readResponse();
        m_process.waitForFinished(1000);
      }
    }
    catch (...) {
    }
    if (m_process.state() != QProcess::NotRunning) {
      m_process.kill();
      m_process.waitForFinished(1000);
    }
  }

private:
  [[nodiscard]] std::vector<ZBioFormatsReaderFormat> listFormatsLocked()
  {
    if (!m_cachedFormats.empty()) {
      return m_cachedFormats;
    }

    proto::Request request;
    request.mutable_list_formats();
    proto::Response response = transact(request);
    requireOk(response, "list Bio-Formats readers");
    if (!response.has_list_formats()) {
      throw ZException(fmt::format("Bio-Formats bridge returned '{}' for list_formats", responseCaseName(response)));
    }

    m_cachedFormats.reserve(response.list_formats().formats_size());
    for (const auto& format : response.list_formats().formats()) {
      m_cachedFormats.push_back(convertReaderFormat(format));
    }
    return m_cachedFormats;
  }
  [[nodiscard]] const ZBioFormatsDatasetInfo& openDatasetLocked(const QString& filename)
  {
    const QString path = canonicalPath(filename);
    return openDatasetByPathLocked(path);
  }

  [[nodiscard]] const ZBioFormatsDatasetInfo& openDatasetByPathLocked(const QString& path)
  {
    auto it = m_datasetsByPath.find(path);
    if (it != m_datasetsByPath.end()) {
      return it.value();
    }

    proto::Request request;
    auto* open = request.mutable_open_dataset();
    open->set_path(path.toStdString());
    open->set_grouping_policy(proto::FILE_GROUPING_POLICY_DEFAULT);
    open->set_metadata_filtered(false);

    proto::Response response = transact(request);
    requireOk(response, "open Bio-Formats dataset");
    if (!response.has_open_dataset()) {
      throw ZException(fmt::format("Bio-Formats bridge returned '{}' for open_dataset", responseCaseName(response)));
    }
    auto inserted = m_datasetsByPath.insert(path, convertDatasetInfo(response.open_dataset()));
    return inserted.value();
  }

  [[nodiscard]] proto::Response transact(proto::Request& request)
  {
    request.set_request_id(nextRequestId());
    writeRequest(request);
    proto::Response response = readResponse();
    if (response.request_id() != request.request_id()) {
      throw ZException(fmt::format("Bio-Formats bridge returned response id {} while waiting for {}",
                                   response.request_id(),
                                   request.request_id()));
    }
    return response;
  }

  uint64_t nextRequestId()
  {
    const uint64_t result = m_nextRequestId++;
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
    if (m_process.state() == QProcess::Running) {
      return;
    }

    if (!ZBioFormatsBridgeClient::hasRuntimeSupport()) {
      throw ZException(fmt::format("Bio-Formats bridge runtime is incomplete. Missing: {}",
                                   ZBioFormatsBridgeClient::missingRuntimeFiles().join(", ")));
    }

    m_datasetsByPath.clear();
    m_stdoutBuffer.clear();

    QElapsedTimer startupTimer;
    startupTimer.start();

    QStringList args;
    if (!FLAGS_atlas_bioformats_java_xmx.empty()) {
      args.push_back(QString("-Xmx%1").arg(QString::fromStdString(FLAGS_atlas_bioformats_java_xmx)));
    }
    args << "-Djava.awt.headless=true"
         << "-cp" << bridgeClasspath() << "org.fenglab.atlas.bioformats.AtlasBioFormatsBridge";

    const QString java = javaExecutable();
    m_process.setProgram(java);
    m_process.setArguments(args);
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    m_process.start();
    if (!m_process.waitForStarted(30000)) {
      const QString error = m_process.errorString();
      if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
      }
      m_datasetsByPath.clear();
      m_stdoutBuffer.clear();
      throw ZException(fmt::format("failed to start Bio-Formats bridge '{}': {}", java, error));
    }
    const qint64 startMs = startupTimer.elapsed();

    QElapsedTimer handshakeTimer;
    handshakeTimer.start();
    try {
      const proto::RuntimeInfoResponse info = runtimeInfo();
      if (info.protocol_version() != kBridgeProtocolVersion) {
        throw ZException(fmt::format("Bio-Formats bridge protocol mismatch: Atlas expects {}, bridge reports {}",
                                     kBridgeProtocolVersion,
                                     info.protocol_version()));
      }
      LOG(INFO) << "Bio-Formats bridge worker " << m_workerIndex << " ready: pid=" << m_process.processId()
                << ", java=" << java << ", jarsDIR=" << ZImgGlobal::instance().jarsDIR << ", startMs=" << startMs
                << ", handshakeMs=" << handshakeTimer.elapsed() << ", protocol=" << info.protocol_version()
                << ", bridge=" << info.bridge_version() << ", bioformats=" << info.bioformats_version()
                << ", javaVersion=" << info.java_version() << ", vm=" << info.java_vm_name()
                << ", javaPid=" << info.process_id();
    }
    catch (...) {
      if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
      }
      m_datasetsByPath.clear();
      m_stdoutBuffer.clear();
      throw;
    }
  }

  [[nodiscard]] proto::RuntimeInfoResponse runtimeInfo()
  {
    proto::Request request;
    request.mutable_runtime_info();
    proto::Response response = transact(request);
    requireOk(response, "handshake with Bio-Formats bridge");
    if (!response.has_runtime_info()) {
      throw ZException(fmt::format("Bio-Formats bridge returned '{}' for runtime_info", responseCaseName(response)));
    }
    return response.runtime_info();
  }

  void writeRequest(const proto::Request& request)
  {
    ensureProcess();
    std::string payload;
    if (!request.SerializeToString(&payload)) {
      throw ZException("failed to serialize Bio-Formats bridge request");
    }
    if (payload.size() > kMaxFrameBytes) {
      throw ZException(fmt::format("Bio-Formats bridge request frame is too large: {} bytes", payload.size()));
    }

    const uint32_t size = static_cast<uint32_t>(payload.size());
    char header[4] = {
      static_cast<char>(size & 0xff_u32),
      static_cast<char>((size >> 8_u32) & 0xff_u32),
      static_cast<char>((size >> 16_u32) & 0xff_u32),
      static_cast<char>((size >> 24_u32) & 0xff_u32),
    };
    writeBytes(header, sizeof(header));
    writeBytes(payload.data(), payload.size());
    waitForBytesWritten();
  }

  void writeBytes(const char* data, size_t size)
  {
    CHECK(data);
    if (size > static_cast<size_t>(std::numeric_limits<qint64>::max())) {
      throw ZException(fmt::format("Bio-Formats bridge write is too large: {} bytes", size));
    }
    const qint64 written = m_process.write(data, static_cast<qint64>(size));
    if (written != static_cast<qint64>(size)) {
      throwProcessError("failed to write Bio-Formats bridge request");
    }
  }

  void waitForBytesWritten()
  {
    QElapsedTimer timer;
    timer.start();
    while (m_process.bytesToWrite() > 0) {
      if (m_process.state() != QProcess::Running) {
        throwProcessError("Bio-Formats bridge exited while writing request");
      }
      const int waitMs = nextWaitMilliseconds(timer);
      if (!m_process.waitForBytesWritten(waitMs) && FLAGS_atlas_bioformats_bridge_io_timeout_ms > 0 &&
          timer.elapsed() >= FLAGS_atlas_bioformats_bridge_io_timeout_ms) {
        throwProcessError("timed out writing Bio-Formats bridge request");
      }
    }
  }

  [[nodiscard]] proto::Response readResponse()
  {
    const QByteArray header = readExact(4);
    const uint32_t size = static_cast<uint8_t>(header[0]) |
                          (static_cast<uint32_t>(static_cast<uint8_t>(header[1])) << 8_u32) |
                          (static_cast<uint32_t>(static_cast<uint8_t>(header[2])) << 16_u32) |
                          (static_cast<uint32_t>(static_cast<uint8_t>(header[3])) << 24_u32);
    if (size > kMaxFrameBytes) {
      throw ZException(fmt::format("Bio-Formats bridge response frame is too large: {} bytes", size));
    }
    const QByteArray payload = readExact(size);
    proto::Response response;
    if (!response.ParseFromArray(payload.constData(), payload.size())) {
      throw ZException("failed to parse Bio-Formats bridge response");
    }
    return response;
  }

  [[nodiscard]] QByteArray readExact(uint32_t size)
  {
    QElapsedTimer timer;
    timer.start();
    while (m_stdoutBuffer.size() < static_cast<qsizetype>(size)) {
      m_stdoutBuffer.append(m_process.readAllStandardOutput());
      if (m_stdoutBuffer.size() >= static_cast<qsizetype>(size)) {
        break;
      }
      if (m_process.state() != QProcess::Running) {
        throwProcessError("Bio-Formats bridge exited while reading response");
      }
      const int waitMs = nextWaitMilliseconds(timer);
      if (!m_process.waitForReadyRead(waitMs) && FLAGS_atlas_bioformats_bridge_io_timeout_ms > 0 &&
          timer.elapsed() >= FLAGS_atlas_bioformats_bridge_io_timeout_ms) {
        throwProcessError("timed out reading Bio-Formats bridge response");
      }
    }

    QByteArray result = m_stdoutBuffer.left(size);
    m_stdoutBuffer.remove(0, size);
    return result;
  }

  [[nodiscard]] int nextWaitMilliseconds(const QElapsedTimer& timer) const
  {
    if (FLAGS_atlas_bioformats_bridge_io_timeout_ms <= 0) {
      return 1000;
    }
    const qint64 elapsed = timer.elapsed();
    if (elapsed >= FLAGS_atlas_bioformats_bridge_io_timeout_ms) {
      return 0;
    }
    return static_cast<int>(std::min<qint64>(1000, FLAGS_atlas_bioformats_bridge_io_timeout_ms - elapsed));
  }

  [[noreturn]] void throwProcessError(const char* message)
  {
    const QString stderrText = QString::fromUtf8(m_process.readAllStandardError());
    QString detail = QString::fromUtf8(message);
    if (!stderrText.isEmpty()) {
      detail += QString(": ") + stderrText;
    } else if (!m_process.errorString().isEmpty()) {
      detail += QString(": ") + m_process.errorString();
    }

    if (m_process.state() != QProcess::NotRunning) {
      m_process.kill();
      m_process.waitForFinished(1000);
    }
    m_datasetsByPath.clear();
    m_stdoutBuffer.clear();
    throw ZException(detail);
  }

  void requireOk(const proto::Response& response, std::string_view operation) const
  {
    if (response.status() == proto::STATUS_CODE_OK) {
      return;
    }
    throw ZException(fmt::format("Bio-Formats bridge failed to {}: {} ({})",
                                 operation,
                                 response.error_message(),
                                 proto::StatusCode_Name(response.status())));
  }

  std::mutex m_mutex;
  size_t m_workerIndex = 0;
  QProcess m_process;
  QByteArray m_stdoutBuffer;
  QString m_javaExecutable;
  uint64_t m_nextRequestId = 1;
  std::vector<ZBioFormatsReaderFormat> m_cachedFormats;
  QMap<QString, ZBioFormatsDatasetInfo> m_datasetsByPath;
};

class ZBioFormatsBridgeWorker
{
public:
  explicit ZBioFormatsBridgeWorker(size_t workerIndex)
    : m_workerIndex(workerIndex)
    , m_thread([this]() {
      run();
    })
  {}

  ~ZBioFormatsBridgeWorker()
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stopping = true;
    }
    m_cv.notify_one();
    if (m_thread.joinable()) {
      m_thread.join();
    }
  }

  template<typename Fn>
  auto call(Fn&& fn) -> std::invoke_result_t<Fn, ZBioFormatsBridgeProcess&>
  {
    auto future = callAsync(std::forward<Fn>(fn));
    if constexpr (std::is_void_v<std::invoke_result_t<Fn, ZBioFormatsBridgeProcess&>>) {
      future.get();
    } else {
      return future.get();
    }
  }

  template<typename Fn>
  [[nodiscard]] auto callAsync(Fn&& fn) -> std::future<std::invoke_result_t<Fn, ZBioFormatsBridgeProcess&>>
  {
    using Result = std::invoke_result_t<Fn, ZBioFormatsBridgeProcess&>;

    if constexpr (std::is_void_v<Result>) {
      std::promise<void> promise;
      std::future<void> future = promise.get_future();
      enqueue(std::packaged_task<void()>([this, fn = std::forward<Fn>(fn), promise = std::move(promise)]() mutable {
        try {
          fn(core());
          promise.set_value();
        }
        catch (...) {
          promise.set_exception(std::current_exception());
        }
      }));
      return future;
    } else {
      std::promise<Result> promise;
      std::future<Result> future = promise.get_future();
      enqueue(std::packaged_task<void()>([this, fn = std::forward<Fn>(fn), promise = std::move(promise)]() mutable {
        try {
          promise.set_value(fn(core()));
        }
        catch (...) {
          promise.set_exception(std::current_exception());
        }
      }));
      return future;
    }
  }

private:
  void enqueue(std::packaged_task<void()> task)
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_stopping) {
        throw ZException("Bio-Formats bridge worker is shutting down");
      }
      m_tasks.emplace_back(std::move(task));
    }
    m_cv.notify_one();
  }

  ZBioFormatsBridgeProcess& core()
  {
    if (!m_core) {
      m_core = std::make_unique<ZBioFormatsBridgeProcess>(m_workerIndex);
    }
    return *m_core;
  }

  void run()
  {
    while (true) {
      std::packaged_task<void()> task;
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() {
          return m_stopping || !m_tasks.empty();
        });
        if (m_tasks.empty()) {
          CHECK(m_stopping);
          break;
        }
        task = std::move(m_tasks.front());
        m_tasks.pop_front();
      }
      task();
    }
    m_core.reset();
  }

  size_t m_workerIndex = 0;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<std::packaged_task<void()>> m_tasks;
  bool m_stopping = false;
  std::unique_ptr<ZBioFormatsBridgeProcess> m_core;
  std::thread m_thread;
};

class ZBioFormatsStdioBridgePool
{
public:
  ZBioFormatsStdioBridgePool()
  {
    const size_t workerCount = resolveBioFormatsBridgeWorkerCount();
    CHECK(workerCount > 0);
    m_workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
      m_workers.push_back(std::make_unique<ZBioFormatsBridgeWorker>(i));
    }
    LOG(INFO) << "Bio-Formats bridge worker pool size: " << workerCount
              << " (--atlas_bioformats_bridge_worker_count=" << FLAGS_atlas_bioformats_bridge_worker_count
              << ", --atlas_bioformats_java_xmx=" << FLAGS_atlas_bioformats_java_xmx << ")";
  }

  [[nodiscard]] std::vector<ZBioFormatsReaderFormat> listFormats()
  {
    return primaryWorker().call([](ZBioFormatsBridgeProcess& core) {
      return core.listFormats();
    });
  }

  void warmUp()
  {
    primaryWorker().call([](ZBioFormatsBridgeProcess& core) {
      core.warmUp();
    });
  }

  [[nodiscard]] bool canRead(const QString& filename)
  {
    return primaryWorker().call([&filename](ZBioFormatsBridgeProcess& core) {
      return core.canRead(filename);
    });
  }

  [[nodiscard]] ZBioFormatsDatasetInfo openDataset(const QString& filename)
  {
    std::vector<std::future<ZBioFormatsDatasetInfo>> futures;
    futures.reserve(m_workers.size());
    for (const auto& worker : m_workers) {
      futures.push_back(worker->callAsync([filename](ZBioFormatsBridgeProcess& core) {
        return core.openDataset(filename);
      }));
    }

    std::optional<ZBioFormatsDatasetInfo> primaryResult;
    std::exception_ptr firstException;
    for (size_t i = 0; i < futures.size(); ++i) {
      try {
        ZBioFormatsDatasetInfo dataset = futures[i].get();
        if (i == 0) {
          primaryResult = std::move(dataset);
        }
      }
      catch (...) {
        if (!firstException) {
          firstException = std::current_exception();
        }
      }
    }
    if (firstException) {
      std::rethrow_exception(firstException);
    }
    CHECK(primaryResult.has_value());
    return std::move(*primaryResult);
  }

  [[nodiscard]] std::vector<uint8_t> readRegion(const QString& filename, size_t scene, const ZImgRegion& region)
  {
    return readWorker().call([&filename, scene, &region](ZBioFormatsBridgeProcess& core) {
      return core.readRegion(filename, scene, region);
    });
  }

  [[nodiscard]] std::vector<uint8_t>
  readRegion(const QString& filename, size_t scene, uint32_t resolution, const ZImgRegion& region)
  {
    return readWorker().call([&filename, scene, resolution, &region](ZBioFormatsBridgeProcess& core) {
      return core.readRegion(filename, scene, resolution, region);
    });
  }

  [[nodiscard]] ZBioFormatsThumbnail readThumbnail(const QString& filename, size_t scene, size_t z, size_t t)
  {
    return primaryWorker().call([&filename, scene, z, t](ZBioFormatsBridgeProcess& core) {
      return core.readThumbnail(filename, scene, z, t);
    });
  }

  void closeDataset(const QString& filename)
  {
    std::vector<std::future<void>> futures;
    futures.reserve(m_workers.size());
    for (const auto& worker : m_workers) {
      futures.push_back(worker->callAsync([filename](ZBioFormatsBridgeProcess& core) {
        core.closeDataset(filename);
      }));
    }
    waitAll(std::move(futures));
  }

private:
  ZBioFormatsBridgeWorker& primaryWorker()
  {
    CHECK(!m_workers.empty());
    CHECK(m_workers.front() != nullptr);
    return *m_workers.front();
  }

  ZBioFormatsBridgeWorker& readWorker()
  {
    CHECK(!m_workers.empty());
    const size_t index = m_nextReadWorker.fetch_add(1, std::memory_order_relaxed) % m_workers.size();
    CHECK(m_workers[index] != nullptr);
    return *m_workers[index];
  }

  template<typename T>
  void waitAll(std::vector<std::future<T>> futures)
  {
    std::exception_ptr firstException;
    for (auto& future : futures) {
      try {
        if constexpr (std::is_void_v<T>) {
          future.get();
        } else {
          (void)future.get();
        }
      }
      catch (...) {
        if (!firstException) {
          firstException = std::current_exception();
        }
      }
    }
    if (firstException) {
      std::rethrow_exception(firstException);
    }
  }

  std::vector<std::unique_ptr<ZBioFormatsBridgeWorker>> m_workers;
  std::atomic<size_t> m_nextReadWorker = 0;
};

class ZBioFormatsGrpcBridgeProcess
{
public:
  explicit ZBioFormatsGrpcBridgeProcess(size_t workerCount)
    : m_workerCount(workerCount)
  {
    CHECK(m_workerCount > 0);
    LOG(INFO) << "Bio-Formats bridge gRPC backend enabled with one JVM and " << m_workerCount << " reader worker(s)"
              << " (--atlas_bioformats_bridge_worker_count=" << FLAGS_atlas_bioformats_bridge_worker_count
              << ", --atlas_bioformats_java_xmx=" << FLAGS_atlas_bioformats_java_xmx << ")";
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
    ensureProcess();
    (void)listFormats();
    LOG(INFO) << "Bio-Formats gRPC bridge warmup completed in " << timer.elapsed() << " ms";
  }

  [[nodiscard]] bool canRead(const QString& filename)
  {
    QElapsedTimer timer;
    timer.start();
    proto::Request request;
    auto* probe = request.mutable_probe();
    probe->set_path(canonicalPath(filename).toStdString());
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

  [[nodiscard]] ZBioFormatsDatasetInfo openDataset(const QString& filename)
  {
    const QString path = canonicalPath(filename);
    std::lock_guard<std::mutex> lock(m_datasetMutex);
    auto it = m_datasetsByPath.find(path);
    if (it != m_datasetsByPath.end()) {
      return it.value();
    }

    QElapsedTimer timer;
    timer.start();
    proto::Request request;
    auto* open = request.mutable_open_dataset();
    open->set_path(path.toStdString());
    open->set_grouping_policy(proto::FILE_GROUPING_POLICY_DEFAULT);
    open->set_metadata_filtered(false);

    proto::Response response = transact(request, "open Bio-Formats dataset");
    requireOk(response, "open Bio-Formats dataset");
    if (!response.has_open_dataset()) {
      throw ZException(
        fmt::format("Bio-Formats gRPC bridge returned '{}' for open_dataset", responseCaseName(response)));
    }
    auto inserted = m_datasetsByPath.insert(path, convertDatasetInfo(response.open_dataset()));
    VLOG(1) << "Bio-Formats gRPC bridge openDataset took " << timer.elapsed() << " ms for " << filename;
    return inserted.value();
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
    const ZBioFormatsDatasetInfo dataset = openDataset(filename);
    if (scene >= dataset.series.size()) {
      throw ZException(fmt::format("Bio-Formats scene {} is out of range for {}", scene, filename));
    }

    proto::Request request;
    const uint64_t requestId = nextRequestId();
    request.set_request_id(requestId);
    auto* readRegion = request.mutable_read_region();
    readRegion->set_session_id(dataset.sessionId);
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
    const ZBioFormatsDatasetInfo dataset = openDataset(filename);
    if (scene >= dataset.series.size()) {
      throw ZException(fmt::format("Bio-Formats scene {} is out of range for {}", scene, filename));
    }

    proto::Request request;
    const uint64_t requestId = nextRequestId();
    request.set_request_id(requestId);
    auto* readThumbnail = request.mutable_read_thumbnail();
    readThumbnail->set_session_id(dataset.sessionId);
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

  void closeDataset(const QString& filename)
  {
    const QString path = canonicalPath(filename);
    ZBioFormatsDatasetInfo dataset;
    {
      std::lock_guard<std::mutex> lock(m_datasetMutex);
      auto it = m_datasetsByPath.find(path);
      if (it == m_datasetsByPath.end()) {
        return;
      }
      dataset = it.value();
      m_datasetsByPath.erase(it);
    }

    proto::Request request;
    request.mutable_close_dataset()->set_session_id(dataset.sessionId);
    proto::Response response = transact(request, "close Bio-Formats dataset");
    requireOk(response, "close Bio-Formats dataset");
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
    if (m_stub) {
      return;
    }

    if (!ZBioFormatsBridgeClient::hasRuntimeSupport()) {
      throw ZException(fmt::format("Bio-Formats bridge runtime is incomplete. Missing: {}",
                                   ZBioFormatsBridgeClient::missingRuntimeFiles().join(", ")));
    }

    m_datasetsByPath.clear();
    m_stdoutBuffer.clear();

    QElapsedTimer startupTimer;
    startupTimer.start();

    QStringList args;
    if (!FLAGS_atlas_bioformats_java_xmx.empty()) {
      args.push_back(QString("-Xmx%1").arg(QString::fromStdString(FLAGS_atlas_bioformats_java_xmx)));
    }
    args << "-Djava.awt.headless=true"
         << "-cp" << bridgeClasspath() << "org.fenglab.atlas.bioformats.AtlasBioFormatsBridge"
         << "--grpc-port=0" << QString("--worker-count=%1").arg(m_workerCount);

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
    m_stub = proto::BioFormatsBridge::NewStub(m_channel);

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
      LOG(INFO) << "Bio-Formats gRPC bridge ready: pid=" << m_process.processId() << ", java=" << java
                << ", jarsDIR=" << ZImgGlobal::instance().jarsDIR << ", target=" << target
                << ", readerWorkers=" << m_workerCount << ", startMs=" << startMs
                << ", handshakeMs=" << handshakeTimer.elapsed() << ", protocol=" << info.protocol_version()
                << ", bridge=" << info.bridge_version() << ", bioformats=" << info.bioformats_version()
                << ", javaVersion=" << info.java_version() << ", vm=" << info.java_vm_name()
                << ", javaPid=" << info.process_id();
    }
    catch (...) {
      m_stub.reset();
      m_channel.reset();
      if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
      }
      m_stdoutBuffer.clear();
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
          throw ZException(fmt::format("Bio-Formats gRPC bridge printed unexpected startup line: {}",
                                       QString::fromUtf8(line).toStdString()));
        }
        bool ok = false;
        const int port = line.mid(static_cast<qsizetype>(prefix.size())).toInt(&ok);
        if (!ok || port <= 0 || port > std::numeric_limits<uint16_t>::max()) {
          throw ZException(fmt::format("Bio-Formats gRPC bridge printed invalid startup port: {}",
                                       QString::fromUtf8(line).toStdString()));
        }
        return static_cast<uint16_t>(port);
      }
      if (m_process.state() != QProcess::Running) {
        throwGrpcProcessError("Bio-Formats gRPC bridge exited before reporting its port");
      }
      const qint64 remaining = kJavaVersionProbeTimeoutMs - timer.elapsed();
      m_process.waitForReadyRead(static_cast<int>(std::max<qint64>(1, std::min<qint64>(1000, remaining))));
    }
    throwGrpcProcessError("timed out waiting for Bio-Formats gRPC bridge startup");
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
    ensureProcess();
    return executeLockedStream(request, operation);
  }

  [[nodiscard]] std::vector<proto::Response> executeLockedStream(const proto::Request& request,
                                                                 std::string_view operation)
  {
    CHECK(m_stub != nullptr);
    grpc::ClientContext context;
    applyDeadline(context);
    std::vector<proto::Response> responses;
    std::unique_ptr<grpc::ClientReader<proto::Response>> reader = m_stub->Execute(&context, request);
    proto::Response response;
    while (reader->Read(&response)) {
      responses.push_back(std::move(response));
      response.Clear();
    }
    const grpc::Status status = reader->Finish();
    if (!status.ok()) {
      throw ZException(fmt::format("Bio-Formats gRPC bridge failed to {}: {} ({})",
                                   operation,
                                   status.error_message(),
                                   static_cast<int>(status.error_code())));
    }
    return responses;
  }

  void applyDeadline(grpc::ClientContext& context) const
  {
    if (FLAGS_atlas_bioformats_bridge_io_timeout_ms <= 0) {
      return;
    }
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(FLAGS_atlas_bioformats_bridge_io_timeout_ms));
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

  [[noreturn]] void throwGrpcProcessError(const char* message)
  {
    const QString stderrText = QString::fromUtf8(m_process.readAllStandardError());
    QString detail = QString::fromUtf8(message);
    if (!stderrText.isEmpty()) {
      detail += QString(": ") + stderrText;
    } else if (!m_process.errorString().isEmpty()) {
      detail += QString(": ") + m_process.errorString();
    }

    if (m_process.state() != QProcess::NotRunning) {
      m_process.kill();
      m_process.waitForFinished(1000);
    }
    m_stdoutBuffer.clear();
    throw ZException(detail);
  }

  size_t m_workerCount = 1;
  std::mutex m_startMutex;
  std::mutex m_cacheMutex;
  std::mutex m_datasetMutex;
  QProcess m_process;
  QByteArray m_stdoutBuffer;
  QString m_javaExecutable;
  std::shared_ptr<grpc::Channel> m_channel;
  std::unique_ptr<proto::BioFormatsBridge::Stub> m_stub;
  std::atomic<uint64_t> m_nextRequestId = 1;
  std::vector<ZBioFormatsReaderFormat> m_cachedFormats;
  QMap<QString, ZBioFormatsDatasetInfo> m_datasetsByPath;
};

class ZBioFormatsBridgeClient::Impl
{
public:
  Impl()
    : m_useGrpc(FLAGS_atlas_bioformats_bridge_use_grpc)
  {
    const size_t workerCount = resolveBioFormatsBridgeWorkerCount();
    CHECK(workerCount > 0);
    if (m_useGrpc) {
      m_grpc = std::make_unique<ZBioFormatsGrpcBridgeProcess>(workerCount);
    } else {
      m_stdio = std::make_unique<ZBioFormatsStdioBridgePool>();
    }
  }

  [[nodiscard]] std::vector<ZBioFormatsReaderFormat> listFormats()
  {
    return m_useGrpc ? m_grpc->listFormats() : m_stdio->listFormats();
  }

  void warmUp()
  {
    if (m_useGrpc) {
      m_grpc->warmUp();
    } else {
      m_stdio->warmUp();
    }
  }

  [[nodiscard]] bool canRead(const QString& filename)
  {
    return m_useGrpc ? m_grpc->canRead(filename) : m_stdio->canRead(filename);
  }

  [[nodiscard]] ZBioFormatsDatasetInfo openDataset(const QString& filename)
  {
    return m_useGrpc ? m_grpc->openDataset(filename) : m_stdio->openDataset(filename);
  }

  [[nodiscard]] std::vector<uint8_t> readRegion(const QString& filename, size_t scene, const ZImgRegion& region)
  {
    return m_useGrpc ? m_grpc->readRegion(filename, scene, region) : m_stdio->readRegion(filename, scene, region);
  }

  [[nodiscard]] std::vector<uint8_t>
  readRegion(const QString& filename, size_t scene, uint32_t resolution, const ZImgRegion& region)
  {
    return m_useGrpc ? m_grpc->readRegion(filename, scene, resolution, region)
                     : m_stdio->readRegion(filename, scene, resolution, region);
  }

  [[nodiscard]] ZBioFormatsThumbnail readThumbnail(const QString& filename, size_t scene, size_t z, size_t t)
  {
    return m_useGrpc ? m_grpc->readThumbnail(filename, scene, z, t) : m_stdio->readThumbnail(filename, scene, z, t);
  }

  void closeDataset(const QString& filename)
  {
    if (m_useGrpc) {
      m_grpc->closeDataset(filename);
    } else {
      m_stdio->closeDataset(filename);
    }
  }

private:
  bool m_useGrpc = false;
  std::unique_ptr<ZBioFormatsStdioBridgePool> m_stdio;
  std::unique_ptr<ZBioFormatsGrpcBridgeProcess> m_grpc;
};

ZBioFormatsBridgeClient& ZBioFormatsBridgeClient::instance()
{
  static ZBioFormatsBridgeClient client;
  return client;
}

bool ZBioFormatsBridgeClient::hasRuntimeSupport()
{
  return missingRuntimeFiles().empty();
}

QStringList ZBioFormatsBridgeClient::missingRuntimeFiles()
{
  QStringList missing;
  if (ZImgGlobal::instance().jarsDIR.isEmpty()) {
    missing.push_back("jars directory");
    return missing;
  }
  QDir jarsDir(ZImgGlobal::instance().jarsDIR);
  if (!jarsDir.exists()) {
    missing.push_back(jarsDir.absolutePath());
    return missing;
  }
  if (!jarsDir.exists(kBioFormatsJar)) {
    missing.push_back(kBioFormatsJar);
  }
  if (!jarsDir.exists(kAtlasBioFormatsBridgeJar)) {
    missing.push_back(kAtlasBioFormatsBridgeJar);
  }
  return missing;
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

ZBioFormatsDatasetInfo ZBioFormatsBridgeClient::openDataset(const QString& filename)
{
  return m_impl->openDataset(filename);
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

void ZBioFormatsBridgeClient::closeDataset(const QString& filename)
{
  m_impl->closeDataset(filename);
}

void ZBioFormatsBridgeClient::warmUp()
{
  m_impl->warmUp();
}

} // namespace nim
