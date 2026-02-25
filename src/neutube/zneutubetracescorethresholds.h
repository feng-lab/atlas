#pragma once

#include "zneutubetraceconfig.h"
#include "zneutubetraceworkspace.h"

#include "zimg.h"

namespace nim::neutube {

// Port of `ZNeuronTracer::prepareTraceScoreThreshold(...)`.
enum class TracingModeLegacyLike
{
  Auto,
  Interactive,
  Seed
};

// Sets `tw->minScore` based on the tracing mode and whether the signal is 2D (depth==1).
void prepareTraceScoreThresholdLegacyLike(const ZImg& signal,
                                          const TraceConfig& cfg,
                                          TracingModeLegacyLike mode,
                                          TraceWorkspace& tw);

} // namespace nim::neutube
