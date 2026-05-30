#include "zneutubegeneral.h"

#include <QDir>
#include <QFileInfo>

#include "zneutubetraceconfig.h"
#include "zneutubetraceauto.h"
#include "zneutubetracezscale.h"
#include "zneutubetracemask.h"
#include "zswcwriter.h"

#include "zimg.h"
#include "zlog.h"

#include <optional>

namespace {

enum class MaskOverrideDecision
{
  None,
  AbortTrace,
  UseMask
};

struct MaskOverrideResult
{
  MaskOverrideDecision decision = MaskOverrideDecision::None;
  std::optional<nim::ZImg> mask;
};

[[nodiscard]] bool isTiffFilePathLegacyLike(const QString& path)
{
  if (path.isEmpty()) {
    return false;
  }
  const QFileInfo fi(path);
  const QString suf = fi.suffix();
  return suf.compare("tif", Qt::CaseInsensitive) == 0 || suf.compare("tiff", Qt::CaseInsensitive) == 0;
}

[[nodiscard]] MaskOverrideResult parseMaskOverrideLegacyLike(const json::object& inputJson,
                                                             const nim::TraceConfig& traceCfg)
{
  MaskOverrideResult res;

  const auto maskIt = inputJson.find("mask");
  if (maskIt == inputJson.end() || !maskIt->value().is_string()) {
    return res;
  }

  const QString maskPath = json::value_to<QString>(maskIt->value());
  if (maskPath.isEmpty()) {
    return res;
  }

  LOG(INFO) << "Using a predefined mask: " << maskPath;
  const QFileInfo fi(maskPath);
  if (!fi.exists() || !fi.isFile()) {
    LOG(ERROR) << "Missing file: Cannot find the mask file " << maskPath;
    res.decision = MaskOverrideDecision::AbortTrace;
    return res;
  }

  if (!isTiffFilePathLegacyLike(maskPath)) {
    LOG(ERROR) << "File error: Failed to recognize the mask file " << maskPath << " as a TIFF";
    res.decision = MaskOverrideDecision::AbortTrace;
    return res;
  }

  nim::ZImg maskImg;
  try {
    maskImg.load(maskPath);
  }
  catch (const std::exception& e) {
    LOG(WARNING) << "File error: Failed to read mask file " << maskPath << " (" << e.what() << ")";
    return res;
  }

  if (maskImg.isEmpty()) {
    LOG(WARNING) << "File error: Failed to read mask file " << maskPath << " (empty)";
    return res;
  }

  int threshold = 0;
  if (auto thrIt = inputJson.find("maskThreshold"); thrIt != inputJson.end() && thrIt->value().is_int64()) {
    threshold = static_cast<int>(thrIt->value().as_int64());
  }

  if (threshold < 0) {
    nim::MakeMaskDiagnosticsLegacyLike diag;
    std::optional<nim::ZImg> mask = nim::makeMaskLegacyLike(maskImg, traceCfg, &diag);
    if (!mask) {
      res.decision = MaskOverrideDecision::AbortTrace;
      return res;
    }
    res.decision = MaskOverrideDecision::UseMask;
    res.mask = std::move(*mask);
    return res;
  }

  res.decision = MaskOverrideDecision::UseMask;
  res.mask = maskImg.binarized(threshold, nim::ZImg::ThresholdMode::ExcludeThreshold);
  return res;
}

} // namespace

namespace {

[[nodiscard]] QString resolveDefaultTraceConfigPathLegacyLike(const QString& traceIncludePath,
                                                              const QString& jsonDirPath)
{
  if (!traceIncludePath.isEmpty()) {
    return traceIncludePath;
  }
  if (jsonDirPath.isEmpty()) {
    return {};
  }
  return QDir(jsonDirPath).absoluteFilePath("trace_config.json");
}

[[nodiscard]] QString resolveTraceConfigPathForGeneralLegacyLike(const json::object& generalCfg,
                                                                 const QString& traceIncludePath,
                                                                 const QString& jsonDirPath)
{
  if (auto it = generalCfg.find("path"); it != generalCfg.end() && it->value().is_string()) {
    const QString path = json::value_to<QString>(it->value());
    if (path.isEmpty() || path == "default") {
      return resolveDefaultTraceConfigPathLegacyLike(traceIncludePath, jsonDirPath);
    }
    return path;
  }

  return resolveDefaultTraceConfigPathLegacyLike(traceIncludePath, jsonDirPath);
}

} // namespace

namespace nim {

int runGeneral(const QString& generalConfigTextOrPath,
               const json::object& generalCfg,
               const json::object& inputJson,
               const std::vector<QString>& positionalInput,
               const QString& outputPath,
               int level,
               bool diagnosis,
               const QString& traceIncludePath,
               const QString& jsonDirPath,
               bool verbose)
{
  if (generalConfigTextOrPath.isEmpty()) {
    LOG(ERROR) << "General: missing --general config (JSON string or JSON file path).";
    return 1;
  }

  json::object cfg = generalCfg;
  cfg["_input"] = inputJson;
  cfg["_source"] = json::value_from(generalConfigTextOrPath);

  const auto commandIt = cfg.find("command");
  const QString commandName = (commandIt != cfg.end() && commandIt->value().is_string())
                                ? json::value_to<QString>(commandIt->value())
                                : QString{};

  LOG(INFO) << "Running command " << commandName << "...";
  if (commandName != "trace_neuron") {
    LOG(ERROR) << "Invalid command module: " << commandName;
    return 1;
  }

  std::vector<QString> updatedInput = positionalInput;
  if (updatedInput.empty()) {
    if (auto it = inputJson.find("signal"); it != inputJson.end() && it->value().is_string()) {
      const QString signalPath = json::value_to<QString>(it->value());
      if (!signalPath.isEmpty()) {
        updatedInput.push_back(signalPath);
      }
    }
  }

  if (updatedInput.empty() || outputPath.isEmpty()) {
    LOG(ERROR) << "trace_neuron: missing input signal and/or output (-o).";
    return 1;
  }

  const QString configPath = resolveTraceConfigPathForGeneralLegacyLike(cfg, traceIncludePath, jsonDirPath);

  TraceConfig baseTraceCfg;
  if (!configPath.isEmpty()) {
    if (!loadTraceConfigLegacyLike(configPath, baseTraceCfg)) {
      LOG(WARNING) << "Configuration failed: failed to load " << configPath;
      baseTraceCfg = TraceConfig{};
    }
  } else {
    LOG(WARNING) << "Configuration skipped: no trace config path resolved.";
    baseTraceCfg = TraceConfig{};
  }

  TraceConfig traceCfg = baseTraceCfg;
  if (level > 0) {
    if (const json::object* levelOverride = selectTraceLevelOverrideLegacyLike(baseTraceCfg, level)) {
      applyTraceConfigOverridesLegacyLike(*levelOverride, traceCfg);
    }
  }

  if (auto it = cfg.find("diagnosis"); it != cfg.end() && it->value().is_bool()) {
    diagnosis = it->value().as_bool();
  }

  if (auto it = cfg.find("action"); it != cfg.end() && it->value().is_string()) {
    const QString action = json::value_to<QString>(it->value());
    if (action == "inspect") {
      LOG(INFO) << "Inspecting trace configuration...";
      LOG(INFO) << "Resolved trace config path: " << (configPath.isEmpty() ? QStringLiteral("<none>") : configPath);
      return 0;
    }
  }

  const MaskOverrideResult maskOverride = parseMaskOverrideLegacyLike(inputJson, traceCfg);

  if (maskOverride.decision == MaskOverrideDecision::AbortTrace) {
    LOG(WARNING) << "WARNING: No result generated.";
    return 0;
  }

  nim::ZImg signal;
  try {
    signal.load(updatedInput[0]);
  }
  catch (const std::exception& e) {
    LOG(ERROR) << "Failed to read input image: " << updatedInput[0] << " (" << e.what() << ")";
    LOG(WARNING) << "WARNING: No result generated.";
    return 0;
  }

  if (signal.isEmpty()) {
    LOG(ERROR) << "Failed to read input image (empty): " << updatedInput[0];
    LOG(WARNING) << "WARNING: No result generated.";
    return 0;
  }

  if (maskOverride.mask) {
    if (maskOverride.mask->width() != signal.width() || maskOverride.mask->height() != signal.height() ||
        maskOverride.mask->depth() != signal.depth()) {
      LOG(ERROR) << "Mask size mismatch: mask(" << maskOverride.mask->info() << ") vs signal(" << signal.info() << ")";
      LOG(WARNING) << "WARNING: No result generated.";
      return 0;
    }
  }

  const ZImg* predefinedMask = maskOverride.mask ? &(*maskOverride.mask) : nullptr;
  const double zToXYRatio = preferredZToXYRatioFromImgInfoLegacyLike(signal.info());
  std::unique_ptr<ZSwc> tree = traceNeuronAutoLegacyLike(std::move(signal),
                                                         traceCfg,
                                                         zToXYRatio,
                                                         diagnosis,
                                                         verbose,
                                                         /*doResampleAfterTracing=*/true,
                                                         predefinedMask);

  if (tree) {
    LOG(INFO) << "Saving " << outputPath << "...";
    writeSwcLegacyNeuTu(*tree, outputPath);
  } else {
    LOG(WARNING) << "WARNING: No result generated.";
  }

  return 0;
}

} // namespace nim
