#pragma once

#include <array>

namespace nim::neutube {

[[nodiscard]] double geo3dDistSqr(double x1, double y1, double z1, double x2, double y2, double z2);

[[nodiscard]] double geo3dDist(double x1, double y1, double z1, double x2, double y2, double z2);

// Port of `Geo3d_Point_Lineseg_Dist` (tz_geo3d_utils.c).
// Returns distance from point to line segment; lambda is clamped to [0, 1].
[[nodiscard]] double geo3dPointLineSegDist(const std::array<double, 3>& point,
                                           const std::array<double, 3>& lineStart,
                                           const std::array<double, 3>& lineEnd,
                                           double* lambda);

[[nodiscard]] std::array<double, 3>
geo3dLineSegBreak(const std::array<double, 3>& lineStart, const std::array<double, 3>& lineEnd, double lambda);

} // namespace nim::neutube
