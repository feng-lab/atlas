#include "zswcgeom.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>

namespace nim {

namespace {

constexpr double TzDist3dEpsLegacyLike = 1e-13;

[[nodiscard]] double geo3dOrgDistSqr(const std::array<double, 3>& v)
{
  return v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
}

// Port of tz_geo3d_utils.c::geo3d_line_line_dist().
// Returns:
//  -3: point to point (both lines degenerate)
//  -2: point to line1 (line2 degenerate)
//  -1: point to line2 (line1 degenerate)
//   0: parallel
//   1: general position (not parallel)
[[nodiscard]] int geo3dLineLineDistInternalLegacyLike(const std::array<double, 3>& line1Start,
                                                      const std::array<double, 3>& line1End,
                                                      const std::array<double, 3>& line2Start,
                                                      const std::array<double, 3>& line2End,
                                                      std::array<double, 3>& dc1,
                                                      std::array<double, 3>& dc2,
                                                      double& intersect1,
                                                      double& intersect2,
                                                      double eps)
{
  std::array<double, 3> ds{};
  for (int i = 0; i < 3; ++i) {
    dc1[i] = line1End[i] - line1Start[i];
    dc2[i] = line2End[i] - line2Start[i];
    ds[i] = line1Start[i] - line2Start[i];
  }

  const double d2121 = geo3dOrgDistSqr(dc1);
  const double d4343 = geo3dOrgDistSqr(dc2);

  if (d2121 < eps) {
    if (d4343 < eps) {
      return -3;
    }
    return -1;
  }

  if (d4343 < eps) {
    return -2;
  }

  const double d4321 = dc2[0] * dc1[0] + dc2[1] * dc1[1] + dc2[2] * dc1[2];
  const double denom = d2121 * d4343 - d4321 * d4321;

  const double d1343 = ds[0] * dc2[0] + ds[1] * dc2[1] + ds[2] * dc2[2];
  const double d1321 = ds[0] * dc1[0] + ds[1] * dc1[1] + ds[2] * dc1[2];

  const double numer = d1343 * d4321 - d1321 * d4343;

  // parallel
  if (denom <= eps) {
    return 0;
  }

  intersect1 = numer / denom;
  intersect2 = (d1343 + d4321 * intersect1) / d4343;
  return 1;
}

// Port of tz_geo3d_utils.c::geo3d_point_line_dist().
[[nodiscard]] bool geo3dPointLineDistInternalLegacyLike(const std::array<double, 3>& point,
                                                        const std::array<double, 3>& lineStart,
                                                        const std::array<double, 3>& lineEnd,
                                                        double& lambda,
                                                        double eps)
{
  const std::array<double, 3> lineVec = {lineEnd[0] - lineStart[0],
                                         lineEnd[1] - lineStart[1],
                                         lineEnd[2] - lineStart[2]};
  const std::array<double, 3> pointVec = {point[0] - lineStart[0], point[1] - lineStart[1], point[2] - lineStart[2]};

  const double lengthSqr = geo3dOrgDistSqr(lineVec);
  if (lengthSqr < eps) {
    return false;
  }

  const double dot = pointVec[0] * lineVec[0] + pointVec[1] * lineVec[1] + pointVec[2] * lineVec[2];
  lambda = dot / lengthSqr;
  return true;
}

} // namespace

double geo3dDistSqr(double x1, double y1, double z1, double x2, double y2, double z2)
{
  const double dx = x1 - x2;
  const double dy = y1 - y2;
  const double dz = z1 - z2;
  return dx * dx + dy * dy + dz * dz;
}

double geo3dDist(double x1, double y1, double z1, double x2, double y2, double z2)
{
  return std::sqrt(geo3dDistSqr(x1, y1, z1, x2, y2, z2));
}

double geo3dPointLineSegDist(const std::array<double, 3>& point,
                             const std::array<double, 3>& lineStart,
                             const std::array<double, 3>& lineEnd,
                             double& lambda)
{
  constexpr double Eps = 1e-9;

  const double vx = lineEnd[0] - lineStart[0];
  const double vy = lineEnd[1] - lineStart[1];
  const double vz = lineEnd[2] - lineStart[2];
  const double wx = point[0] - lineStart[0];
  const double wy = point[1] - lineStart[1];
  const double wz = point[2] - lineStart[2];

  const double lenSqr = vx * vx + vy * vy + vz * vz;
  double t = 0.0;
  if (lenSqr > Eps) {
    const double dot = wx * vx + wy * vy + wz * vz;
    t = dot / lenSqr;
  }

  if (t < 0.0) {
    t = 0.0;
  } else if (t > 1.0) {
    t = 1.0;
  }

  lambda = t;

  const double cx = (1.0 - t) * lineStart[0] + t * lineEnd[0];
  const double cy = (1.0 - t) * lineStart[1] + t * lineEnd[1];
  const double cz = (1.0 - t) * lineStart[2] + t * lineEnd[2];
  return geo3dDist(point[0], point[1], point[2], cx, cy, cz);
}

double geo3dLineLineDistLegacyLike(const std::array<double, 3>& line1Start,
                                   const std::array<double, 3>& line1End,
                                   const std::array<double, 3>& line2Start,
                                   const std::array<double, 3>& line2End)
{
  // Port of tz_geo3d_utils.c::Geo3d_Line_Line_Dist().
  double intersect1 = 0.0;
  double intersect2 = 0.0;
  std::array<double, 3> dc1{};
  std::array<double, 3> dc2{};

  double lambda = 0.0;

  switch (geo3dLineLineDistInternalLegacyLike(line1Start,
                                              line1End,
                                              line2Start,
                                              line2End,
                                              dc1,
                                              dc2,
                                              intersect1,
                                              intersect2,
                                              TzDist3dEpsLegacyLike)) {
    case -3: // point to point
      return geo3dDist(line1Start[0], line1Start[1], line1Start[2], line2Start[0], line2Start[1], line2Start[2]);
    case -1: // point to line2
      (void)geo3dPointLineDistInternalLegacyLike(line1Start, line2Start, line2End, lambda, TzDist3dEpsLegacyLike);
      return geo3dDist(line1Start[0],
                       line1Start[1],
                       line1Start[2],
                       line2Start[0] + lambda * dc2[0],
                       line2Start[1] + lambda * dc2[1],
                       line2Start[2] + lambda * dc2[2]);
    case -2: // point to line1
      (void)geo3dPointLineDistInternalLegacyLike(line2Start, line1Start, line1End, lambda, TzDist3dEpsLegacyLike);
      return geo3dDist(line2Start[0],
                       line2Start[1],
                       line2Start[2],
                       line1Start[0] + lambda * dc1[0],
                       line1Start[1] + lambda * dc1[1],
                       line1Start[2] + lambda * dc1[2]);
    case 0: // parallel
      (void)geo3dPointLineDistInternalLegacyLike(line1Start, line2Start, line2End, lambda, TzDist3dEpsLegacyLike);
      return geo3dDist(line1Start[0],
                       line1Start[1],
                       line1Start[2],
                       line2Start[0] + lambda * dc2[0],
                       line2Start[1] + lambda * dc2[1],
                       line2Start[2] + lambda * dc2[2]);
    default: { // line to line
      const double x1 = line1Start[0] + intersect1 * dc1[0];
      const double y1 = line1Start[1] + intersect1 * dc1[1];
      const double z1 = line1Start[2] + intersect1 * dc1[2];
      const double x2 = line2Start[0] + intersect2 * dc2[0];
      const double y2 = line2Start[1] + intersect2 * dc2[1];
      const double z2 = line2Start[2] + intersect2 * dc2[2];
      return std::sqrt(geo3dDistSqr(x1, y1, z1, x2, y2, z2));
    }
  }
}

double geo3dLineSegLineSegDistLegacyLike(const std::array<double, 3>& line1Start,
                                         const std::array<double, 3>& line1End,
                                         const std::array<double, 3>& line2Start,
                                         const std::array<double, 3>& line2End,
                                         double& intersect1,
                                         double& intersect2,
                                         int& cond)
{
  // Port of tz_geo3d_utils.c::Geo3d_Lineseg_Lineseg_Dist().
  std::array<double, 3> dc1{};
  std::array<double, 3> dc2{};

  double d1 = 0.0;
  double d2 = 0.0;

  const int code = geo3dLineLineDistInternalLegacyLike(line1Start,
                                                       line1End,
                                                       line2Start,
                                                       line2End,
                                                       dc1,
                                                       dc2,
                                                       intersect1,
                                                       intersect2,
                                                       TzDist3dEpsLegacyLike);

  switch (code) {
    case 0: // parallel
      cond = 9;
      d1 = geo3dPointLineSegDist(line1End, line2Start, line2End, intersect1);
      d2 = geo3dPointLineSegDist(line1Start, line2Start, line2End, intersect2);
      if (d1 <= d2) {
        intersect2 = intersect1;
        intersect1 = 1.0;
        return d1;
      }
      intersect1 = 0.0;
      return d2;
    case -1:
      intersect1 = 0.0;
      cond = 10;
      return geo3dPointLineSegDist(line1Start, line2Start, line2End, intersect2);
    case -2:
      intersect2 = 0.0;
      cond = 10;
      return geo3dPointLineSegDist(line2Start, line1Start, line1End, intersect1);
    case -3:
      cond = 10;
      intersect1 = 0.0;
      intersect2 = 0.0;
      return geo3dDist(line1Start[0], line1Start[1], line1Start[2], line2Start[0], line2Start[1], line2Start[2]);
    default:
      break;
  }

  auto breakIsInRange = [](double mu) -> bool {
    return (mu >= 0.0) && (mu <= 1.0);
  };

  if (breakIsInRange(intersect1) && breakIsInRange(intersect2)) {
    cond = 0;
    const double x1 = line1Start[0] + intersect1 * dc1[0];
    const double y1 = line1Start[1] + intersect1 * dc1[1];
    const double z1 = line1Start[2] + intersect1 * dc1[2];
    const double x2 = line2Start[0] + intersect2 * dc2[0];
    const double y2 = line2Start[1] + intersect2 * dc2[1];
    const double z2 = line2Start[2] + intersect2 * dc2[2];
    return std::sqrt(geo3dDistSqr(x1, y1, z1, x2, y2, z2));
  }
  if ((intersect1 < 0.0) && breakIsInRange(intersect2)) {
    cond = 1;
    intersect1 = 0.0;
    return geo3dPointLineSegDist(line1Start, line2Start, line2End, intersect2);
  }
  if ((intersect1 > 0.0) && breakIsInRange(intersect2)) {
    cond = 2;
    intersect1 = 1.0;
    return geo3dPointLineSegDist(line1End, line2Start, line2End, intersect2);
  }
  if ((intersect2 < 0.0) && breakIsInRange(intersect1)) {
    cond = 3;
    intersect2 = 0.0;
    return geo3dPointLineSegDist(line2Start, line1Start, line1End, intersect1);
  }
  if ((intersect2 > 1.0) && breakIsInRange(intersect1)) {
    cond = 4;
    intersect2 = 1.0;
    return geo3dPointLineSegDist(line2End, line1Start, line1End, intersect1);
  }
  if ((intersect1 < 0.0) && (intersect2 < 0.0)) {
    cond = 5;
    d1 = geo3dPointLineSegDist(line1Start, line2Start, line2End, intersect2);
    d2 = geo3dPointLineSegDist(line2Start, line1Start, line1End, intersect1);
    if (d1 <= d2) {
      intersect1 = 0.0;
      return d1;
    }
    intersect2 = 0.0;
    return d2;
  }
  if ((intersect1 > 1.0) && (intersect2 < 0.0)) {
    cond = 6;
    d1 = geo3dPointLineSegDist(line1End, line2Start, line2End, intersect2);
    d2 = geo3dPointLineSegDist(line2Start, line1Start, line1End, intersect1);
    if (d1 <= d2) {
      intersect1 = 1.0;
      return d1;
    }
    intersect2 = 0.0;
    return d2;
  }
  if ((intersect1 < 0.0) && (intersect2 > 1.0)) {
    cond = 7;
    d1 = geo3dPointLineSegDist(line1Start, line2Start, line2End, intersect2);
    d2 = geo3dPointLineSegDist(line2End, line1Start, line1End, intersect1);
    if (d1 <= d2) {
      intersect1 = 0.0;
      return d1;
    }
    intersect2 = 1.0;
    return d2;
  }
  if ((intersect1 > 1.0) && (intersect2 > 1.0)) {
    cond = 8;
    d1 = geo3dPointLineSegDist(line1End, line2Start, line2End, intersect2);
    d2 = geo3dPointLineSegDist(line2End, line1Start, line1End, intersect1);
    if (d1 <= d2) {
      intersect1 = 1.0;
      return d1;
    }
    intersect2 = 1.0;
    return d2;
  }

  return -1.0;
}

std::array<double, 3>
geo3dLineSegBreak(const std::array<double, 3>& lineStart, const std::array<double, 3>& lineEnd, double lambda)
{
  return {lambda * lineEnd[0] + (1.0 - lambda) * lineStart[0],
          lambda * lineEnd[1] + (1.0 - lambda) * lineStart[1],
          lambda * lineEnd[2] + (1.0 - lambda) * lineStart[2]};
}

} // namespace nim
