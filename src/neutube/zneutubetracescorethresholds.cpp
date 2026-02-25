#include "zneutubetracescorethresholds.h"

#include "zlog.h"

namespace nim::neutube {

namespace {

[[nodiscard]] bool is2dTraceLegacyLike(const ZImg& signal)
{
  return signal.depth() == 1;
}

} // namespace

void prepareTraceScoreThresholdLegacyLike(const ZImg& signal,
                                          const TraceConfig& cfg,
                                          TracingModeLegacyLike mode,
                                          TraceWorkspace& tw)
{
  if (is2dTraceLegacyLike(signal)) {
    tw.minScore = cfg.min2dScore;
    return;
  }

  switch (mode) {
    case TracingModeLegacyLike::Auto:
      tw.minScore = cfg.minAutoScore;
      return;
    case TracingModeLegacyLike::Interactive:
      tw.minScore = cfg.minManualScore;
      return;
    case TracingModeLegacyLike::Seed:
      tw.minScore = cfg.minSeedScore;
      return;
  }

  CHECK(false) << "Unhandled tracing mode";
}

} // namespace nim::neutube
