#pragma once

#include <array>

namespace nim {

// C++ representation of tz_geo3d_circle.h::Geo3d_Circle used by neuTube tracing.
struct Geo3dCircle
{
  std::array<double, 3> center{};
  double radius = 0.0;
  std::array<double, 2> orientation{}; // (theta, psi)
};

} // namespace nim
