#pragma once

#include <array>

namespace nim {

// C++ port of tz_geo3d_ellipse.h::Geo3d_Ellipse (minimal fields used by tracing).
struct Geo3dEllipseLegacyLike
{
  double radius = 1.0; // ry
  double scale = 1.0; // rx / ry
  double alpha = 0.0;
  std::array<double, 3> center = {0.0, 0.0, 0.0};
  std::array<double, 2> orientation = {0.0, 0.0}; // theta, psi
};

// Port of tz_geo3d_ellipse.c::Geo3d_Ellipse_Point_Distance().
[[nodiscard]] double geo3dEllipsePointDistanceLegacyLike(const Geo3dEllipseLegacyLike& ellipse,
                                                         const std::array<double, 3>& pt);

// Port of tz_geo3d_ellipse.c::Geo3d_Ellipse_Interpolate().
[[nodiscard]] Geo3dEllipseLegacyLike geo3dEllipseInterpolateLegacyLike(const Geo3dEllipseLegacyLike& start,
                                                                       const Geo3dEllipseLegacyLike& end,
                                                                       double lambda);

} // namespace nim
