#pragma once

#include "zneutubetraceconfig.h"

#include "zimg.h"
#include "zswc.h"

#include <memory>

namespace nim {

// Port of `ZNeuronTracer::trace(Stack *stack, bool doResampleAfterTracing=true)`
// when called without a seed position (fully automatic tracing).
//
// Notes:
// - `signal` is passed by value and is modified in-place (legacy preprocess subtracts background, etc.).
// - `predefinedMask` is optional; when provided it replaces the legacy auto-generated
//   trace mask (useful for A/B tests or callers that already computed a mask).
// - Returns nullptr on legacy "no result generated" paths.
[[nodiscard]] std::unique_ptr<ZSwc> traceNeuronAutoLegacyLike(ZImg signal,
                                                              const TraceConfig& cfg,
                                                              bool diagnosis,
                                                              bool verbose,
                                                              /*nullable*/ const ZImg* predefinedMask = nullptr);

} // namespace nim
