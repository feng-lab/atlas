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

[[nodiscard]] bool isTiffFilePathLegacyLike(const std::string& path)
{
  if (path.empty()) {
    return false;
  }
  const QFileInfo fi(QString::fromStdString(path));
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

  const std::string maskPath = std::string(maskIt->value().as_string().c_str());
  if (maskPath.empty()) {
    return res;
  }

  LOG(INFO) << "Using a predefined mask: " << maskPath;
  const QFileInfo fi(QString::fromStdString(maskPath));
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
    maskImg.load(QString::fromStdString(maskPath));
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

[[nodiscard]] std::string resolveDefaultTraceConfigPathLegacyLike(const std::string& traceIncludePath,
                                                                  const std::string& jsonDirPath)
{
  if (!traceIncludePath.empty()) {
    return traceIncludePath;
  }
  if (jsonDirPath.empty()) {
    return {};
  }
  return QDir(QString::fromStdString(jsonDirPath)).absoluteFilePath("trace_config.json").toStdString();
}

[[nodiscard]] std::string resolveTraceConfigPathForGeneralLegacyLike(const json::object& generalCfg,
                                                                     const std::string& traceIncludePath,
                                                                     const std::string& jsonDirPath)
{
  if (auto it = generalCfg.find("path"); it != generalCfg.end() && it->value().is_string()) {
    const std::string path = std::string(it->value().as_string().c_str());
    if (path.empty() || path == "default") {
      return resolveDefaultTraceConfigPathLegacyLike(traceIncludePath, jsonDirPath);
    }
    return path;
  }

  return resolveDefaultTraceConfigPathLegacyLike(traceIncludePath, jsonDirPath);
}

} // namespace

namespace nim {

int runGeneral(const std::string& generalConfigTextOrPath,
               const json::object& generalCfg,
               const json::object& inputJson,
               const std::vector<std::string>& positionalInput,
               const std::string& outputPath,
               int level,
               bool diagnosis,
               const std::string& traceIncludePath,
               const std::string& jsonDirPath,
               bool verbose)
{
  if (generalConfigTextOrPath.empty()) {
    LOG(ERROR) << "General: missing --general config (JSON string or JSON file path).";
    return 1;
  }

  json::object cfg = generalCfg;
  cfg["_input"] = inputJson;
  cfg["_source"] = generalConfigTextOrPath;

  const auto commandIt = cfg.find("command");
  const std::string commandName = (commandIt != cfg.end() && commandIt->value().is_string())
                                    ? std::string(commandIt->value().as_string().c_str())
                                    : std::string{};

  LOG(INFO) << "Running command " << commandName << "...";
  if (commandName != "trace_neuron") {
    LOG(ERROR) << "Invalid command module: " << commandName;
    return 1;
  }

  std::vector<std::string> updatedInput = positionalInput;
  if (updatedInput.empty()) {
    if (auto it = inputJson.find("signal"); it != inputJson.end() && it->value().is_string()) {
      const std::string signalPath = std::string(it->value().as_string().c_str());
      if (!signalPath.empty()) {
        updatedInput.push_back(signalPath);
      }
    }
  }

  if (updatedInput.empty() || outputPath.empty()) {
    LOG(ERROR) << "trace_neuron: missing input signal and/or output (-o).";
    return 1;
  }

  const std::string configPath = resolveTraceConfigPathForGeneralLegacyLike(cfg, traceIncludePath, jsonDirPath);

  TraceConfig baseTraceCfg;
  if (!configPath.empty()) {
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
    const std::string action = std::string(it->value().as_string().c_str());
    if (action == "inspect") {
      LOG(INFO) << "Inspecting trace configuration...";
      LOG(INFO) << "Resolved trace config path: " << (configPath.empty() ? "<none>" : configPath);
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
    signal.load(QString::fromStdString(updatedInput[0]));
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
  const double zScale = preferredZScaleFromImgInfoLegacyLike(signal.info());
  std::unique_ptr<ZSwc> tree = traceNeuronAutoLegacyLike(std::move(signal),
                                                         traceCfg,
                                                         zScale,
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
