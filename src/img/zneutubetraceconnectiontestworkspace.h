#pragma once

#include "zneutubeneuroseg.h"

#include <array>

namespace nim {

class ZImg;

}

namespace nim {

// C++ port of `tz_trace_defs.h::Connection_Test_Workspace`.
//
// This workspace is used during reconstruction to decide whether two traced
// `LocsegChain`s should be connected, including optional shortest-path tests.
struct ConnectionTestWorkspaceLegacyLike
{
  int hookSpot = -1;
  double dist = NeurosegDefaultHLegacyLike * 10.0;
  double cos1 = 0.0;
  double cos2 = 0.0;
  double distThre = NeurosegDefaultHLegacyLike;
  bool goodDist = false;

  // Step lengths used by the optional shortest-path test.
  //
  // Tracing code treats this as the trace-space resolution, not raw image metadata.
  // Entry points should set it explicitly from the chosen tracing anisotropy via
  // `traceResolutionFromZToXYRatioLegacyLike(zToXYRatio)`.
  std::array<double, 3> resolution = {1.0, 1.0, 1.0};
  char unit = 'p'; // 'p' pixel, 'u' um

  // Optional mask used by the shortest-path test (nullptr means no mask).
  const ZImg* mask = nullptr;

  double bigEuc = 15.0;
  double bigPlanar = 10.0;

  bool spTest = true;
  bool interpolate = true;
  bool crossoverTest = false;
};

// Port of `tz_trace_utils.c::Default_Connection_Test_Workspace()`.
void defaultConnectionTestWorkspaceLegacyLike(ConnectionTestWorkspaceLegacyLike& ctw);

} // namespace nim
