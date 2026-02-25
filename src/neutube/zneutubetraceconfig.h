#pragma once

#include "zjson.h"

#include <array>
#include <optional>
#include <string>

namespace nim::neutube {

// Port of `ZNeuronTracerConfig` (src/neurolabi/gui/zneurontracerconfig.*) as a
// pure Boost.JSON-backed configuration object.
//
// Notes:
// - Defaults are intentionally kept byte-identical to legacy `ZNeuronTracerConfig::init()`.
// - Parsing is intentionally strict about the legacy `tag` field: if the tag does not
//   match an accepted legacy value, the config is treated as invalid and defaults remain.
struct TraceConfig
{
  // Defaults match ZNeuronTracerConfig::init().
  double minAutoScore = 0.3;
  double minManualScore = 0.3;
  double minSeedScore = 0.35;
  double min2dScore = 0.5;

  bool refit = false;
  bool spTest = true;
  bool crossoverTest = false;
  bool tuneEnd = true;
  bool edgePath = false;
  bool enhanceMask = false;

  int seedMethod = 1;
  int recover = 2;

  int chainScreenCount = 0;
  double maxEucDist = 20.0;

  // Optional per-level overrides loaded from `"level": { "1": {...}, ... }`.
  // Index 1..9 correspond to levels '1'..'9'. Index 0 is unused.
  std::array<std::optional<json::object>, 10> levelOverrides{};
};

// Applies a legacy trace-config object (e.g. `"default"` or a `"level"` entry)
// onto an existing config.
//
// This is equivalent to legacy `ZNeuronTracerConfig::loadJsonObject()` field
// updates, but operates on the already-parsed Boost.JSON object.
void applyTraceConfigOverridesLegacyLike(const json::object& obj, TraceConfig& cfg);

// Loads a legacy trace config file (trace_config.json) into `out`.
//
// Returns true if the file parsed and had a legacy-accepted `"tag"` value.
// On false, `out` is still populated with legacy defaults.
[[nodiscard]] bool loadTraceConfigLegacyLike(const std::string& traceConfigPath, TraceConfig& out);

// Selects the per-level override JSON object using the legacy `getLevelJson()` semantics.
// Returns nullptr if no level overrides are available.
[[nodiscard]] const json::object* selectTraceLevelOverrideLegacyLike(const TraceConfig& cfg, int level);

} // namespace nim::neutube
