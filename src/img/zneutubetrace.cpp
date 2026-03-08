#include "zneutubetrace.h"

#include "zswcloaderlegacy.h"
#include "zneutubetraceinteractive.h"
#include "zneutubetraceauto.h"
#include "zneutubetraceconfig.h"
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

[[nodiscard]] int runSeededTraceLegacyLike(const std::string& signalPath,
                                           const std::string& outputPath,
                                           const std::array<int, 3>& position,
                                           int level,
                                           const std::string& traceConfigPath,
                                           const std::string& jsonDirPath)
{
  nim::ZImg signal;
  try {
    signal.load(QString::fromStdString(signalPath));
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Failed to read input image: " << signalPath << " (" << e.what() << ")";
    return 1;
  }

  if (signal.isEmpty()) {
    LOG(ERROR) << "Failed to read input image (empty): " << signalPath;
    return 1;
  }

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

  const std::array<double, 3> seed = {static_cast<double>(position[0]),
                                      static_cast<double>(position[1]),
                                      static_cast<double>(position[2])};

  const double zScale = preferredZScaleFromImgInfoLegacyLike(signal.info());
  const SeedTraceResult traceRes = traceSeedNewSwcLegacyLike(signal, seed, cfg, zScale);
  if (!traceRes.swc) {
    return 1;
  }

  writeSwcLegacyNeuTu(*traceRes.swc, outputPath);
  return 0;
}

[[nodiscard]] int runSeededTraceWithHostSwcLegacyLike(const std::string& signalPath,
                                                      const std::string& hostSwcPath,
                                                      const std::string& outputPath,
                                                      const std::array<int, 3>& position,
                                                      int level,
                                                      const std::string& traceConfigPath,
                                                      const std::string& jsonDirPath)
{
  if (!isSwcFilePathLegacyLike(hostSwcPath)) {
    return runSeededTraceLegacyLike(signalPath, outputPath, position, level, traceConfigPath, jsonDirPath);
  }

  nim::ZImg signal;
  try {
    signal.load(QString::fromStdString(signalPath));
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Failed to read input image: " << signalPath << " (" << e.what() << ")";
    return 1;
  }

  if (signal.isEmpty()) {
    LOG(ERROR) << "Failed to read input image (empty): " << signalPath;
    return 1;
  }

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

  nim::ZSwc hostSwc;
  std::string swcError;
  if (!loadSwcLegacyOrder(hostSwcPath, hostSwc, &swcError)) {
    LOG(ERROR) << "Failed to read host SWC: " << hostSwcPath << " (" << swcError << ")";
    return 1;
  }

  const std::array<double, 3> seed = {static_cast<double>(position[0]),
                                      static_cast<double>(position[1]),
                                      static_cast<double>(position[2])};

  const double zScale = preferredZScaleFromImgInfoLegacyLike(signal.info());
  const SeedTraceResult traceRes = traceSeedIntoHostSwcLegacyLike(signal, hostSwc, seed, cfg, zScale);
  CHECK(traceRes.swc);
  writeSwcLegacyNeuTu(*traceRes.swc, outputPath);
  return 0;
}

} // namespace

int runTrace(const std::vector<std::string>& input,
             const std::string& outputPath,
             const std::optional<std::array<int, 3>>& position,
             int level,
             bool diagnosis,
             const std::string& traceConfigPath,
             const std::string& jsonDirPath,
             bool verbose)
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

  if (!position.has_value()) {
    nim::ZImg signal;
    try {
      signal.load(QString::fromStdString(input[0]));
    }
    catch (const std::exception& e) {
      LOG(ERROR) << "Failed to read input image: " << input[0] << " (" << e.what() << ")";
      return 1;
    }

    if (signal.isEmpty()) {
      LOG(ERROR) << "Failed to read input image (empty): " << input[0];
      return 1;
    }

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

    const double zScale = preferredZScaleFromImgInfoLegacyLike(signal.info());
    std::unique_ptr<ZSwc> tree = traceNeuronAutoLegacyLike(std::move(signal),
                                                           cfg,
                                                           zScale,
                                                           diagnosis,
                                                           verbose,
                                                           /*doResampleAfterTracing=*/true,
                                                           nullptr);
    if (tree) {
      writeSwcLegacyNeuTu(*tree, outputPath);
      return 0;
    }

    LOG(WARNING) << "WARNING: No result generated.";
    return 1;
  }

  const std::string resolvedConfig = resolveTraceConfigPathLegacyLike(traceConfigPath, jsonDirPath);
  if (!resolvedConfig.empty() && !fileExists(resolvedConfig)) {
    LOG(WARNING) << "Tracing configuration failed: failed to load " << resolvedConfig;
  }

  const std::string swcPath = (input.size() > 1) ? input[1] : std::string{};
  const int rc = (swcPath.empty() || !isSwcFilePathLegacyLike(swcPath))
                   ? runSeededTraceLegacyLike(input[0], outputPath, *position, level, traceConfigPath, jsonDirPath)
                   : runSeededTraceWithHostSwcLegacyLike(input[0],
                                                         swcPath,
                                                         outputPath,
                                                         *position,
                                                         level,
                                                         traceConfigPath,
                                                         jsonDirPath);
  if (rc != 0) {
    LOG(WARNING) << "WARNING: No result generated.";
  }
  return rc;
}

} // namespace nim
