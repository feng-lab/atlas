#include "zneutubetrace.h"

#include "zcancellation.h"
#include "zexception.h"
#include "zneutubeautotraceprocess.h"
#include "zneutubeblockedautotraceprocess.h"
#include "zswcloaderlegacy.h"
#include "zneutubetraceconfig.h"
#include "zneutubetraceinteractive.h"
#include "zneutubetracezscale.h"
#include "zswcwriter.h"

#include "zimg.h"
#include "zioutils.h"
#include "zlog.h"

#include <QDir>
#include <QFileInfo>
#include <QString>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace nim {

namespace {

[[nodiscard]] QString resolveTraceConfigPathLegacyLike(const QString& traceConfigPath, const QString& jsonDirPath)
{
  if (!traceConfigPath.isEmpty()) {
    return traceConfigPath;
  }
  if (jsonDirPath.isEmpty()) {
    return {};
  }
  return QDir(jsonDirPath).absoluteFilePath("trace_config.json");
}

[[nodiscard]] bool isSwcFilePathLegacyLike(const QString& path)
{
  if (path.isEmpty()) {
    return false;
  }
  const QFileInfo fi(path);
  return fi.suffix().compare("swc", Qt::CaseInsensitive) == 0;
}

[[nodiscard]] TraceConfig
buildTraceConfigLegacyLike(const QString& traceConfigPath, const QString& jsonDirPath, int level)
{
  const QString resolvedConfigPath = resolveTraceConfigPathLegacyLike(traceConfigPath, jsonDirPath);

  TraceConfig baseCfg;
  if (!resolvedConfigPath.isEmpty()) {
    const bool ok = loadTraceConfigLegacyLike(resolvedConfigPath, baseCfg);
    if (!ok) {
      LOG(WARNING) << "Tracing configuration failed: failed to load " << resolvedConfigPath;
    }
  } else {
    LOG(WARNING) << "Tracing configuration skipped: no trace config path and no json dir available.";
    baseCfg = TraceConfig{};
  }

  TraceConfig cfg = baseCfg;
  if (level > 0) {
    if (const json::object* levelOverride = selectTraceLevelOverrideLegacyLike(baseCfg, level)) {
      applyTraceConfigOverridesLegacyLike(*levelOverride, cfg);
    }
  }
  return cfg;
}

[[nodiscard]] ZImgRegion selectedChannelTimeRegionOrThrow(const ZImgInfo& info, size_t c, size_t t)
{
  if (c >= info.numChannels || t >= info.numTimes) {
    throw ZException(
      fmt::format("Trace failed: invalid channel/time selection (c={}, t={}) for signal <{}>.", c, t, info));
  }

  const auto cStart = static_cast<ZImgRegion::value_type>(c);
  const auto cEnd = static_cast<ZImgRegion::value_type>(c + 1);
  const auto tStart = static_cast<ZImgRegion::value_type>(t);
  const auto tEnd = static_cast<ZImgRegion::value_type>(t + 1);
  return ZImgRegion(0, -1, 0, -1, 0, -1, cStart, cEnd, tStart, tEnd);
}

[[nodiscard]] double effectiveZToXYRatioLegacyLike(const ZImgInfo& info, const RunTraceOptions& options)
{
  return options.zToXYRatioOverride.value_or(
    preferredZToXYRatioFromImgInfoLegacyLike(info, options.signalDownsampleRatio));
}

[[nodiscard]] int
runSeededTraceLegacyLike(const QString& signalPath, const QString& outputPath, const RunTraceOptions& options)
{
  if (options.signalDownsampleRatio != std::array<size_t, 3>{1, 1, 1}) {
    LOG(ERROR) << "Seeded CLI tracing does not support downsampled tracing yet.";
    return 1;
  }

  nim::ZImg signal;
  try {
    const ZImgSource datasetSource(signalPath);
    const ZImgInfo signalInfo = ZImg::readImgInfo(datasetSource);
    const ZImgRegion region =
      selectedChannelTimeRegionOrThrow(signalInfo, options.selectedChannel, options.selectedTime);
    signal.load(ZImgSource(signalPath, region));
    CHECK(signal.numChannels() == 1);
    CHECK(signal.numTimes() == 1);
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Failed to read input image: " << signalPath << " (" << e.what() << ")";
    return 1;
  }

  if (signal.isEmpty()) {
    LOG(ERROR) << "Failed to read input image (empty): " << signalPath;
    return 1;
  }

  const TraceConfig cfg = buildTraceConfigLegacyLike(options.traceConfigPath, options.jsonDirPath, options.level);

  CHECK(options.position.has_value());
  const std::array<double, 3> seed = {static_cast<double>((*options.position)[0]),
                                      static_cast<double>((*options.position)[1]),
                                      static_cast<double>((*options.position)[2])};

  const double zToXYRatio = effectiveZToXYRatioLegacyLike(signal.info(), options);
  const SeedTraceResult traceRes = traceSeedNewSwcLegacyLike(signal, seed, cfg, zToXYRatio);
  if (!traceRes.swc) {
    return 1;
  }

  writeSwcLegacyNeuTu(*traceRes.swc, outputPath);
  return 0;
}

[[nodiscard]] int runSeededTraceWithHostSwcLegacyLike(const QString& signalPath,
                                                      const QString& hostSwcPath,
                                                      const QString& outputPath,
                                                      const RunTraceOptions& options)
{
  if (options.signalDownsampleRatio != std::array<size_t, 3>{1, 1, 1}) {
    LOG(ERROR) << "Seeded CLI tracing into an existing SWC does not support downsampled tracing yet.";
    return 1;
  }

  if (!isSwcFilePathLegacyLike(hostSwcPath)) {
    return runSeededTraceLegacyLike(signalPath, outputPath, options);
  }

  nim::ZImg signal;
  try {
    const ZImgSource datasetSource(signalPath);
    const ZImgInfo signalInfo = ZImg::readImgInfo(datasetSource);
    const ZImgRegion region =
      selectedChannelTimeRegionOrThrow(signalInfo, options.selectedChannel, options.selectedTime);
    signal.load(ZImgSource(signalPath, region));
    CHECK(signal.numChannels() == 1);
    CHECK(signal.numTimes() == 1);
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Failed to read input image: " << signalPath << " (" << e.what() << ")";
    return 1;
  }

  if (signal.isEmpty()) {
    LOG(ERROR) << "Failed to read input image (empty): " << signalPath;
    return 1;
  }

  const TraceConfig cfg = buildTraceConfigLegacyLike(options.traceConfigPath, options.jsonDirPath, options.level);

  nim::ZSwc hostSwc;
  std::string swcError;
  if (!loadSwcLegacyOrder(hostSwcPath, hostSwc, &swcError)) {
    LOG(ERROR) << "Failed to read host SWC: " << hostSwcPath << " (" << swcError << ")";
    return 1;
  }

  CHECK(options.position.has_value());
  const std::array<double, 3> seed = {static_cast<double>((*options.position)[0]),
                                      static_cast<double>((*options.position)[1]),
                                      static_cast<double>((*options.position)[2])};

  const double zToXYRatio = effectiveZToXYRatioLegacyLike(signal.info(), options);
  const SeedTraceResult traceRes = traceSeedIntoHostSwcLegacyLike(signal, hostSwc, seed, cfg, zToXYRatio);
  CHECK(traceRes.swc);
  writeSwcLegacyNeuTu(*traceRes.swc, outputPath);
  return 0;
}

[[nodiscard]] int
runBlockedAutoTraceLegacyLike(const QString& signalPath, const QString& outputPath, const RunTraceOptions& options)
{
  if (options.position.has_value()) {
    LOG(ERROR) << "Blocked trace does not support seeded tracing.";
    return 1;
  }

  const QString sessionDir = options.outputSessionDir.isEmpty() ? outputPath : options.outputSessionDir;
  if (sessionDir.isEmpty()) {
    LOG(ERROR) << "Blocked trace requires an output session directory.";
    return 1;
  }

  const QString resolvedConfigPath = resolveTraceConfigPathLegacyLike(options.traceConfigPath, options.jsonDirPath);

  try {
    ZNeutubeBlockedAutoTraceProcess worker;
    worker.setLogFile(QDir(sessionDir).absoluteFilePath(QStringLiteral("log.txt")));
    worker.setInputImageSource(ZImgSource(signalPath));
    worker.setSelectedChannelTime(options.selectedChannel, options.selectedTime);
    if (options.zToXYRatioOverride.has_value()) {
      worker.setZToXYRatio(*options.zToXYRatioOverride);
    }
    worker.setSignalDownsampleRatio(options.signalDownsampleRatio);
    if (!resolvedConfigPath.isEmpty()) {
      worker.setTraceConfigPath(resolvedConfigPath);
    }
    worker.setTraceLevel(options.level);
    worker.setDoResampleAfterTracing(true);
    worker.setOutputSessionDir(sessionDir);
    worker.setOutputSwcPath(QDir(sessionDir).absoluteFilePath(QStringLiteral("result.swc")));
    worker.run();

    if (!worker.hasResult()) {
      LOG(WARNING) << "WARNING: No result generated.";
      return 1;
    }
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Blocked trace failed: " << e.what();
    return 1;
  }
  return 0;
}

} // namespace

int runTrace(const std::vector<QString>& input, const QString& outputPath, const RunTraceOptions& options)
{
  if (input.empty()) {
    LOG(INFO) << "No input specified. Abort.";
    return 1;
  }

  if (input[0].isEmpty()) {
    LOG(INFO) << "No input data specified. Abort.";
    return 1;
  }

  if (outputPath.isEmpty()) {
    LOG(INFO) << "No output specified. Abort.";
    return 1;
  }

  if (options.useBlocked) {
    const QString swcPath = (input.size() > 1) ? input[1] : QString{};
    if (!swcPath.isEmpty() && isSwcFilePathLegacyLike(swcPath)) {
      LOG(ERROR) << "Blocked trace does not support tracing into an existing SWC.";
      return 1;
    }
    return runBlockedAutoTraceLegacyLike(input[0], outputPath, options);
  }

  if (!options.position.has_value()) {
    const QString resolvedConfigPath = resolveTraceConfigPathLegacyLike(options.traceConfigPath, options.jsonDirPath);

    try {
      ZNeutubeAutoTraceProcess worker;
      worker.setInputImageSource(ZImgSource(input[0]));
      worker.setSelectedChannelTime(options.selectedChannel, options.selectedTime);
      if (options.zToXYRatioOverride.has_value()) {
        worker.setZToXYRatio(*options.zToXYRatioOverride);
      }
      if (!resolvedConfigPath.isEmpty()) {
        worker.setTraceConfigPath(resolvedConfigPath);
      }
      worker.setTraceLevel(options.level);
      worker.setDoResampleAfterTracing(true);
      worker.setOutputSwcPath(outputPath);
      worker.setSignalDownsampleRatio(options.signalDownsampleRatio);
      worker.run();

      if (!worker.hasResult()) {
        LOG(WARNING) << "WARNING: No result generated.";
        return 1;
      }
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Auto trace failed: " << e.what();
      return 1;
    }
    return 0;
  }

  const QString resolvedConfig = resolveTraceConfigPathLegacyLike(options.traceConfigPath, options.jsonDirPath);
  if (!resolvedConfig.isEmpty() && !fileExists(resolvedConfig)) {
    LOG(WARNING) << "Tracing configuration failed: failed to load " << resolvedConfig;
  }

  const QString swcPath = (input.size() > 1) ? input[1] : QString{};
  const int rc = (swcPath.isEmpty() || !isSwcFilePathLegacyLike(swcPath))
                   ? runSeededTraceLegacyLike(input[0], outputPath, options)
                   : runSeededTraceWithHostSwcLegacyLike(input[0], swcPath, outputPath, options);
  if (rc != 0) {
    LOG(WARNING) << "WARNING: No result generated.";
  }
  return rc;
}

} // namespace nim
