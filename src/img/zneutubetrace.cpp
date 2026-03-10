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
#include "zlog.h"

#include <QDir>
#include <QFileInfo>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace nim {

namespace {

[[nodiscard]] std::string resolveTraceConfigPathLegacyLike(const std::string& traceConfigPath,
                                                           const std::string& jsonDirPath)
{
  if (!traceConfigPath.empty()) {
    return traceConfigPath;
  }
  if (jsonDirPath.empty()) {
    return {};
  }
  return QDir(QString::fromStdString(jsonDirPath)).absoluteFilePath("trace_config.json").toStdString();
}

[[nodiscard]] bool fileExists(const std::string& path)
{
  if (path.empty()) {
    return false;
  }
  const QFileInfo fi(QString::fromStdString(path));
  return fi.exists() && fi.isFile();
}

[[nodiscard]] bool isSwcFilePathLegacyLike(const std::string& path)
{
  if (path.empty()) {
    return false;
  }
  const QFileInfo fi(QString::fromStdString(path));
  return fi.suffix().compare("swc", Qt::CaseInsensitive) == 0;
}

[[nodiscard]] TraceConfig
buildTraceConfigLegacyLike(const std::string& traceConfigPath, const std::string& jsonDirPath, int level)
{
  const std::string resolvedConfigPath = resolveTraceConfigPathLegacyLike(traceConfigPath, jsonDirPath);

  TraceConfig baseCfg;
  if (!resolvedConfigPath.empty()) {
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

[[nodiscard]] ZImgInfo readSignalInfoOrThrow(const std::string& signalPath)
{
  const ZImgSource datasetSource(QString::fromStdString(signalPath));
  return ZImg::readImgInfo(datasetSource);
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

[[nodiscard]] ZImg loadSelectedSignalOrThrow(const std::string& signalPath,
                                             size_t c,
                                             size_t t,
                                             const std::array<size_t, 3>& ratio = {1, 1, 1})
{
  CHECK(ratio[0] > 0);
  CHECK(ratio[1] > 0);
  CHECK(ratio[2] > 0);
  const ZImgInfo info = readSignalInfoOrThrow(signalPath);
  const ZImgRegion region = selectedChannelTimeRegionOrThrow(info, c, t);

  ZImg signal;
  signal.load(ZImgSource(QString::fromStdString(signalPath), region), ratio[0], ratio[1], ratio[2]);
  CHECK(signal.numChannels() == 1);
  CHECK(signal.numTimes() == 1);
  return signal;
}

[[nodiscard]] double effectiveZToXYRatioLegacyLike(const ZImgInfo& info, const RunTraceOptions& options)
{
  return options.zToXYRatioOverride.value_or(
    preferredZToXYRatioFromImgInfoLegacyLike(info, options.signalDownsampleRatio));
}

[[nodiscard]] int
runSeededTraceLegacyLike(const std::string& signalPath, const std::string& outputPath, const RunTraceOptions& options)
{
  if (options.signalDownsampleRatio != std::array<size_t, 3>{1, 1, 1}) {
    LOG(ERROR) << "Seeded CLI tracing does not support downsampled tracing yet.";
    return 1;
  }

  nim::ZImg signal;
  try {
    signal = loadSelectedSignalOrThrow(signalPath, options.selectedChannel, options.selectedTime);
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

[[nodiscard]] int runSeededTraceWithHostSwcLegacyLike(const std::string& signalPath,
                                                      const std::string& hostSwcPath,
                                                      const std::string& outputPath,
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
    signal = loadSelectedSignalOrThrow(signalPath, options.selectedChannel, options.selectedTime);
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

[[nodiscard]] int runBlockedAutoTraceLegacyLike(const std::string& signalPath,
                                                const std::string& outputPath,
                                                const RunTraceOptions& options)
{
  if (options.position.has_value()) {
    LOG(ERROR) << "Blocked trace does not support seeded tracing.";
    return 1;
  }

  const QString sessionDir =
    QString::fromStdString(options.outputSessionDir.empty() ? outputPath : options.outputSessionDir);
  if (sessionDir.isEmpty()) {
    LOG(ERROR) << "Blocked trace requires an output session directory.";
    return 1;
  }

  const ZImgSource datasetSource(QString::fromStdString(signalPath));

  ZImgInfo info;
  try {
    info = ZImg::readImgInfo(datasetSource);
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Failed to read input image info: " << signalPath << " (" << e.what() << ")";
    return 1;
  }

  if (info.isEmpty()) {
    LOG(ERROR) << "Failed to read input image info (empty): " << signalPath;
    return 1;
  }

  try {
    (void)selectedChannelTimeRegionOrThrow(info, options.selectedChannel, options.selectedTime);
  }
  catch (const std::exception& e) {
    LOG(ERROR) << e.what();
    return 1;
  }

  const double zToXYRatio = effectiveZToXYRatioLegacyLike(info, options);
  const std::string resolvedConfigPath = resolveTraceConfigPathLegacyLike(options.traceConfigPath, options.jsonDirPath);

  ZNeutubeBlockedAutoTraceProcess worker;
  worker.setLogFile(QDir(sessionDir).absoluteFilePath(QStringLiteral("log.txt")));
  worker.setSignalInfo(info);
  worker.setDatasetId(datasetSource.toString());
  worker.setSelectedChannelTime(options.selectedChannel, options.selectedTime);
  worker.setZToXYRatio(zToXYRatio);
  worker.setSignalDownsampleRatio(options.signalDownsampleRatio);
  if (!resolvedConfigPath.empty()) {
    worker.setTraceConfigPath(QString::fromStdString(resolvedConfigPath));
  }
  worker.setTraceLevel(options.level);
  worker.setDoResampleAfterTracing(true);
  worker.setOutputSessionDir(sessionDir);
  worker.setOutputSwcPath(QDir(sessionDir).absoluteFilePath(QStringLiteral("result.swc")));
  worker.setRoiSignalProvider(
    [signalPath, info, ratio = options.signalDownsampleRatio, c = options.selectedChannel, t = options.selectedTime](
      int64_t sx,
      int64_t sy,
      int64_t sz,
      int64_t w,
      int64_t h,
      int64_t d,
      folly::CancellationToken token) -> ZNeutubeBlockedAutoTraceProcess::RoiSignalResult {
      maybeCancel(token);

      const int64_t x0 = sx * static_cast<int64_t>(ratio[0]);
      const int64_t y0 = sy * static_cast<int64_t>(ratio[1]);
      const int64_t z0 = sz * static_cast<int64_t>(ratio[2]);
      const int64_t x1 = std::min<int64_t>(static_cast<int64_t>(info.width), (sx + w) * static_cast<int64_t>(ratio[0]));
      const int64_t y1 =
        std::min<int64_t>(static_cast<int64_t>(info.height), (sy + h) * static_cast<int64_t>(ratio[1]));
      const int64_t z1 = std::min<int64_t>(static_cast<int64_t>(info.depth), (sz + d) * static_cast<int64_t>(ratio[2]));

      const auto xStart = static_cast<ZImgRegion::value_type>(x0);
      const auto xEnd = static_cast<ZImgRegion::value_type>(x1);
      const auto yStart = static_cast<ZImgRegion::value_type>(y0);
      const auto yEnd = static_cast<ZImgRegion::value_type>(y1);
      const auto zStart = static_cast<ZImgRegion::value_type>(z0);
      const auto zEnd = static_cast<ZImgRegion::value_type>(z1);
      const auto cStart = static_cast<ZImgRegion::value_type>(c);
      const auto cEnd = static_cast<ZImgRegion::value_type>(c + 1);
      const auto tStart = static_cast<ZImgRegion::value_type>(t);
      const auto tEnd = static_cast<ZImgRegion::value_type>(t + 1);

      const ZImgRegion region(xStart, xEnd, yStart, yEnd, zStart, zEnd, cStart, cEnd, tStart, tEnd);
      auto image =
        std::make_shared<ZImg>(ZImgSource(QString::fromStdString(signalPath), region), ratio[0], ratio[1], ratio[2]);
      maybeCancel(token);
      return ZNeutubeBlockedAutoTraceProcess::RoiSignalResult::ok(std::move(image));
    });

  try {
    worker.run();
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Blocked trace failed: " << e.what();
    return 1;
  }

  if (!worker.hasResult()) {
    LOG(WARNING) << "WARNING: No result generated.";
    return 1;
  }
  return 0;
}

} // namespace

int runTrace(const std::vector<std::string>& input, const std::string& outputPath, const RunTraceOptions& options)
{
  if (input.empty()) {
    LOG(INFO) << "No input specified. Abort.";
    return 1;
  }

  if (input[0].empty()) {
    LOG(INFO) << "No input data specified. Abort.";
    return 1;
  }

  if (outputPath.empty()) {
    LOG(INFO) << "No output specified. Abort.";
    return 1;
  }

  if (options.useBlocked) {
    const std::string swcPath = (input.size() > 1) ? input[1] : std::string{};
    if (!swcPath.empty() && isSwcFilePathLegacyLike(swcPath)) {
      LOG(ERROR) << "Blocked trace does not support tracing into an existing SWC.";
      return 1;
    }
    return runBlockedAutoTraceLegacyLike(input[0], outputPath, options);
  }

  if (!options.position.has_value()) {
    ZImgInfo signalInfo;
    try {
      signalInfo = readSignalInfoOrThrow(input[0]);
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Failed to read input image info: " << input[0] << " (" << e.what() << ")";
      return 1;
    }

    if (signalInfo.isEmpty()) {
      LOG(ERROR) << "Failed to read input image info (empty): " << input[0];
      return 1;
    }

    try {
      (void)selectedChannelTimeRegionOrThrow(signalInfo, options.selectedChannel, options.selectedTime);
    }
    catch (const std::exception& e) {
      LOG(ERROR) << e.what();
      return 1;
    }

    const double zToXYRatio = effectiveZToXYRatioLegacyLike(signalInfo, options);
    const std::string resolvedConfigPath =
      resolveTraceConfigPathLegacyLike(options.traceConfigPath, options.jsonDirPath);

    ZNeutubeAutoTraceProcess worker;
    worker.setSelectedChannelTime(options.selectedChannel, options.selectedTime);
    worker.setZToXYRatio(zToXYRatio);
    if (!resolvedConfigPath.empty()) {
      worker.setTraceConfigPath(QString::fromStdString(resolvedConfigPath));
    }
    worker.setTraceLevel(options.level);
    worker.setDoResampleAfterTracing(true);
    worker.setOutputSwcPath(QString::fromStdString(outputPath));
    worker.setSignalDownsampleRatio(options.signalDownsampleRatio);
    worker.setSignalProvider([signalPath = input[0],
                              ratio = options.signalDownsampleRatio,
                              c = options.selectedChannel,
                              t = options.selectedTime](folly::CancellationToken token) -> ZImg {
      maybeCancel(token);
      ZImg signal = loadSelectedSignalOrThrow(signalPath, c, t, ratio);
      maybeCancel(token);
      return signal;
    });

    try {
      worker.run();
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Auto trace failed: " << e.what();
      return 1;
    }

    if (!worker.hasResult()) {
      LOG(WARNING) << "WARNING: No result generated.";
      return 1;
    }
    return 0;
  }

  const std::string resolvedConfig = resolveTraceConfigPathLegacyLike(options.traceConfigPath, options.jsonDirPath);
  if (!resolvedConfig.empty() && !fileExists(resolvedConfig)) {
    LOG(WARNING) << "Tracing configuration failed: failed to load " << resolvedConfig;
  }

  const std::string swcPath = (input.size() > 1) ? input[1] : std::string{};
  const int rc = (swcPath.empty() || !isSwcFilePathLegacyLike(swcPath))
                   ? runSeededTraceLegacyLike(input[0], outputPath, options)
                   : runSeededTraceWithHostSwcLegacyLike(input[0], swcPath, outputPath, options);
  if (rc != 0) {
    LOG(WARNING) << "WARNING: No result generated.";
  }
  return rc;
}

} // namespace nim
