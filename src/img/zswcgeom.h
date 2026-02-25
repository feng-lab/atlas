#pragma once

#include <array>

namespace nim {

[[nodiscard]] double geo3dDistSqr(double x1, double y1, double z1, double x2, double y2, double z2);

[[nodiscard]] double geo3dDist(double x1, double y1, double z1, double x2, double y2, double z2);

// Port of `Geo3d_Point_Lineseg_Dist` (tz_geo3d_utils.c).
// Returns distance from point to line segment; lambda is clamped to [0, 1].
[[nodiscard]] double geo3dPointLineSegDist(const std::array<double, 3>& point,
                                           const std::array<double, 3>& lineStart,
                                           const std::array<double, 3>& lineEnd,
                                           double& lambda);

// Port of tz_geo3d_utils.c::Geo3d_Line_Line_Dist().
[[nodiscard]] double geo3dLineLineDistLegacyLike(const std::array<double, 3>& line1Start,
                                                 const std::array<double, 3>& line1End,
                                                 const std::array<double, 3>& line2Start,
                                                 const std::array<double, 3>& line2End);

// Port of tz_geo3d_utils.c::Geo3d_Lineseg_Lineseg_Dist().
// - `intersect1` / `intersect2` are the break parameters on segment1/segment2 (may be outside [0, 1]).
// - `cond` matches the legacy condition codes.
[[nodiscard]] double geo3dLineSegLineSegDistLegacyLike(const std::array<double, 3>& line1Start,
                                                       const std::array<double, 3>& line1End,
                                                       const std::array<double, 3>& line2Start,
                                                       const std::array<double, 3>& line2End,
                                                       double& intersect1,
                                                       double& intersect2,
                                                       int& cond);

[[nodiscard]] std::array<double, 3>
geo3dLineSegBreak(const std::array<double, 3>& lineStart, const std::array<double, 3>& lineEnd, double lambda);

} // namespace nim
