#pragma once

#include <array>

namespace nim {

// Port of `tz_neurocomp_conn.h` connection modes.
enum class NeurocompConnModeLegacyLike : int
{
  None = 0,
  HookLoop = 1,
  Link = 2
};

// Port of `tz_neurocomp_conn.h::Neurocomp_Conn`.
struct NeurocompConnLegacyLike
{
  NeurocompConnModeLegacyLike mode = NeurocompConnModeLegacyLike::None;
  std::array<int, 2> info = {0, 0};
  std::array<double, 3> pos = {0.0, 0.0, 0.0};
  std::array<double, 3> ort = {0.0, 0.0, 0.0};
  double cost = 0.0;
  double pdist = -1.0;
  double sdist = -1.0;
};

void defaultNeurocompConnLegacyLike(NeurocompConnLegacyLike& conn);

// Port of `tz_neurocomp_conn.c::Neurocomp_Conn_Translate_Mode()`.
//
// The legacy helper translates HookLoop into Link mode when the loop position
// is at an endpoint of the second component.
void neurocompConnTranslateModeLegacyLike(int len2, NeurocompConnLegacyLike& conn);

} // namespace nim
