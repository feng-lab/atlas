#include "zneutubetraceconfig.h"

#include "zlog.h"

#include <QFileInfo>

#include <string_view>

namespace nim {

namespace {

[[nodiscard]] bool hasAcceptedLegacyTag(std::string_view tag)
{
  // Port of ZNeuronTracerConfig::loadJsonObject() tag checks.
  return tag == "trace configuration" || tag == "trace_configuration" || tag == "trace config" || tag == "trace_config";
}

[[nodiscard]] std::optional<std::string_view> jsonStringView(const json::value& v)
{
  if (!v.is_string()) {
    return std::nullopt;
  }
  const auto& s = v.as_string();
  return std::string_view(s.data(), s.size());
}

void applyConfigOverridesLegacyLike(const json::object& obj, TraceConfig& cfg)
{
  if (auto it = obj.find("minimalScoreAuto"); it != obj.end() && it->value().is_number()) {
    cfg.minAutoScore = it->value().to_number<double>();
  }

  if (auto it = obj.find("minimalScoreManual"); it != obj.end() && it->value().is_number()) {
    cfg.minManualScore = it->value().to_number<double>();
  }

  if (auto it = obj.find("minimalScoreSeed"); it != obj.end() && it->value().is_number()) {
    cfg.minSeedScore = it->value().to_number<double>();
  }

  if (auto it = obj.find("minimalScore2d"); it != obj.end() && it->value().is_number()) {
    cfg.min2dScore = it->value().to_number<double>();
  }

  if (auto it = obj.find("maxEucDist"); it != obj.end() && it->value().is_number()) {
    cfg.maxEucDist = it->value().to_number<double>();
  }

  if (auto it = obj.find("refit"); it != obj.end() && it->value().is_bool()) {
    cfg.refit = it->value().as_bool();
  }

  if (auto it = obj.find("spTest"); it != obj.end() && it->value().is_bool()) {
    cfg.spTest = it->value().as_bool();
  }

  if (auto it = obj.find("crossoverTest"); it != obj.end() && it->value().is_bool()) {
    cfg.crossoverTest = it->value().as_bool();
  }

  if (auto it = obj.find("tuneEnd"); it != obj.end() && it->value().is_bool()) {
    cfg.tuneEnd = it->value().as_bool();
  }

  if (auto it = obj.find("edgePath"); it != obj.end() && it->value().is_bool()) {
    cfg.edgePath = it->value().as_bool();
  }

  if (auto it = obj.find("seedMethod"); it != obj.end() && it->value().is_int64()) {
    cfg.seedMethod = static_cast<int>(it->value().as_int64());
  }

  if (auto it = obj.find("recover"); it != obj.end() && it->value().is_int64()) {
    cfg.recover = static_cast<int>(it->value().as_int64());
  }

  if (auto it = obj.find("enhanceMask"); it != obj.end() && it->value().is_bool()) {
    cfg.enhanceMask = it->value().as_bool();
  }

  if (auto it = obj.find("chainScreenCount"); it != obj.end() && it->value().is_int64()) {
    cfg.chainScreenCount = static_cast<int>(it->value().as_int64());
  }
}

[[nodiscard]] bool loadTraceConfigObjectLegacyLike(const json::object& root, TraceConfig& out, const QString& source)
{
  out = TraceConfig{};

  // Legacy accepts an optional wrapper object: { "trace": { ... } }.
  const json::object* configObj = &root;
  if (auto it = root.find("trace"); it != root.end() && it->value().is_object()) {
    configObj = &it->value().as_object();
  }

  const std::string_view tag = [&]() -> std::string_view {
    if (auto it = configObj->find("tag"); it != configObj->end()) {
      if (auto s = jsonStringView(it->value())) {
        return *s;
      }
    }
    return {};
  }();

  if (!hasAcceptedLegacyTag(tag)) {
    if (source.isEmpty()) {
      LOG(WARNING) << "Ignoring trace config object with unexpected tag '" << std::string(tag) << "'.";
    } else {
      LOG(WARNING) << "Ignoring trace config with unexpected tag '" << std::string(tag) << "': " << source;
    }
    return false;
  }

  if (auto it = configObj->find("default"); it != configObj->end() && it->value().is_object()) {
    applyConfigOverridesLegacyLike(it->value().as_object(), out);
  }

  if (auto it = configObj->find("level"); it != configObj->end() && it->value().is_object()) {
    const auto& levelObj = it->value().as_object();
    for (const auto& kv : levelObj) {
      const std::string_view key = std::string_view(kv.key().data(), kv.key().size());
      if (key.size() != 1) {
        continue;
      }
      const char c = key[0];
      if (c < '1' || c > '9') {
        continue;
      }
      const int level = c - '0';
      if (!kv.value().is_object()) {
        continue;
      }
      out.levelOverrides[static_cast<size_t>(level)] = kv.value().as_object();
    }
  }

  return true;
}

} // namespace

void applyTraceConfigOverridesLegacyLike(const json::object& obj, TraceConfig& cfg)
{
  applyConfigOverridesLegacyLike(obj, cfg);
}

bool loadTraceConfigLegacyLike(const QString& traceConfigPath, TraceConfig& out)
{
  out = TraceConfig{};

  if (traceConfigPath.isEmpty()) {
    return false;
  }

  const QFileInfo fi(traceConfigPath);
  if (!fi.exists() || !fi.isFile()) {
    return false;
  }

  json::object root;
  try {
    root = nim::loadJsonObject(fi.absoluteFilePath());
  }
  catch (const std::exception& e) {
    LOG(WARNING) << "Failed to parse trace config '" << traceConfigPath << "': " << e.what();
    return false;
  }

  return loadTraceConfigObjectLegacyLike(root, out, traceConfigPath);
}

bool loadTraceConfigLegacyLike(const json::object& traceConfigRoot, TraceConfig& out)
{
  return loadTraceConfigObjectLegacyLike(traceConfigRoot, out, QString{});
}

const json::object* selectTraceLevelOverrideLegacyLike(const TraceConfig& cfg, int level)
{
  // Port of ZNeuronTracerConfig::getLevelJson().
  bool hasAny = false;
  for (int i = 1; i <= 9; ++i) {
    if (cfg.levelOverrides[static_cast<size_t>(i)].has_value()) {
      hasAny = true;
      break;
    }
  }
  if (!hasAny) {
    return nullptr;
  }

  if (level >= 1 && level <= 9) {
    const auto& exact = cfg.levelOverrides[static_cast<size_t>(level)];
    if (exact.has_value()) {
      return &(*exact);
    }
  }

  // Legacy fallback behavior effectively returns the first entry in the map
  // (smallest key) when the requested level is not present.
  for (int i = 1; i <= 9; ++i) {
    const auto& v = cfg.levelOverrides[static_cast<size_t>(i)];
    if (v.has_value()) {
      return &(*v);
    }
  }

  return nullptr;
}

} // namespace nim
