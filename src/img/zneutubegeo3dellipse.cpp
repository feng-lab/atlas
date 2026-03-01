#include "zneutubegeo3dellipse.h"

#include "zneutube3dgeom.h"
#include "zneutubegeo3dutils.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nim {

namespace {

constexpr double TzPiLegacyLike = 3.14159265358979323846264338328;
constexpr double Tz2PiLegacyLike = 2.0 * TzPiLegacyLike;

[[nodiscard]] double normalizeRadianLegacyLike(double r)
{
  // Port of tz_constant.h::Normalize_Radian().
  r = std::fmod(r, Tz2PiLegacyLike);
  if (r < 0.0) {
    r += Tz2PiLegacyLike;
  }
  return r;
}

[[nodiscard]] bool pointInEllipseLegacyLike(double x, double y, double a, double b)
{
  // Port of tz_geometry.c::Point_In_Ellipse().
  x /= a;
  y /= b;
  return (x * x + y * y) <= 1.0;
}

// Port of tz_geometry.c::DistancePointEllipseSpecial() (David Eberly).
[[nodiscard]] double distancePointEllipseSpecialLegacyLike(double u,
                                                           double v,
                                                           double a,
                                                           double b,
                                                           double epsilon,
                                                           int maxIter,
                                                           double* outX,
                                                           double* outY)
{
  double t = b * (v - b);

  for (int i = 0; i < maxIter; ++i) {
    const double tpASqr = t + a * a;
    const double tpBSqr = t + b * b;
    const double invTpASqr = 1.0 / tpASqr;
    const double invTpBSqr = 1.0 / tpBSqr;

    const double xDivA = a * u * invTpASqr;
    const double yDivB = b * v * invTpBSqr;
    const double xDivASqr = xDivA * xDivA;
    const double yDivBSqr = yDivB * yDivB;

    const double f = xDivASqr + yDivBSqr - 1.0;
    if (f < epsilon) {
      *outX = xDivA * a;
      *outY = yDivB * b;
      break;
    }

    const double fDer = 2.0 * (xDivASqr * invTpASqr + yDivBSqr * invTpBSqr);
    const double ratio = f / fDer;
    if (ratio < epsilon) {
      *outX = xDivA * a;
      *outY = yDivB * b;
      break;
    }

    t += ratio;
  }

  const double dx = *outX - u;
  const double dy = *outY - v;
  return std::sqrt(dx * dx + dy * dy);
}

// Port of tz_geometry.c::Ellipse_Point_Distance().
[[nodiscard]] double ellipsePointDistanceLegacyLike(double u, double v, double a, double b, double* outX, double* outY)
{
  constexpr double epsilon = 1e-5;
  constexpr int maxIter = 100;

  double tmpX = 0.0;
  double tmpY = 0.0;
  if (outX == nullptr) {
    outX = &tmpX;
  }
  if (outY == nullptr) {
    outY = &tmpY;
  }

  if (std::fabs(a - b) < epsilon) {
    const double length = std::sqrt(u * u + v * v);
    *outX = a / length * u;
    *outY = a / length * v;
    return std::fabs(length - a);
  }

  bool xReflect = false;
  if (u > epsilon) {
    xReflect = false;
  } else if (u < -epsilon) {
    xReflect = true;
    u = -u;
  } else {
    xReflect = false;
    u = 0.0;
  }

  bool yReflect = false;
  if (v > epsilon) {
    yReflect = false;
  } else if (v < -epsilon) {
    yReflect = true;
    v = -v;
  } else {
    yReflect = false;
    v = 0.0;
  }

  bool transpose = false;
  if (a < b) {
    transpose = true;
    std::swap(a, b);
    std::swap(u, v);
  }

  double dist = 0.0;
  if (u != 0.0) {
    if (v != 0.0) {
      dist = distancePointEllipseSpecialLegacyLike(u, v, a, b, epsilon, maxIter, outX, outY);
    } else {
      const double bSqr = b * b;
      if (u < a - bSqr / a) {
        const double aSqr = a * a;
        *outX = aSqr * u / (aSqr - bSqr);
        const double xDivA = *outX / a;
        *outY = b * std::sqrt(std::fabs(1.0 - xDivA * xDivA));
        const double dx = *outX - u;
        dist = std::sqrt(dx * dx + (*outY) * (*outY));
      } else {
        dist = std::fabs(u - a);
        *outX = a;
        *outY = 0.0;
      }
    }
  } else {
    dist = std::fabs(v - b);
    *outX = 0.0;
    *outY = b;
  }

  if (transpose) {
    std::swap(*outX, *outY);
  }
  if (yReflect) {
    *outY = -(*outY);
  }
  if (xReflect) {
    *outX = -(*outX);
  }

  return dist;
}

} // namespace

double geo3dEllipsePointDistanceLegacyLike(const Geo3dEllipseLegacyLike& ellipse, const std::array<double, 3>& pt)
{
  // Port of tz_geo3d_ellipse.c::Geo3d_Ellipse_Point_Distance().
  std::array<double, 3> tmpPt = {pt[0] - ellipse.center[0], pt[1] - ellipse.center[1], pt[2] - ellipse.center[2]};

  if (ellipse.alpha != 0.0) {
    rotateZLegacyLike(&tmpPt, 1, ellipse.alpha, 1);
  }

  if (ellipse.orientation[0] != 0.0 || ellipse.orientation[1] != 0.0) {
    rotateXZLegacyLike(&tmpPt, 1, ellipse.orientation[0], ellipse.orientation[1], 1);
  }

  double d = 0.0;
  const double rx = ellipse.radius * ellipse.scale;
  const double ry = ellipse.radius;

  if (!pointInEllipseLegacyLike(tmpPt[0], tmpPt[1], rx, ry)) {
    d = ellipsePointDistanceLegacyLike(tmpPt[0], tmpPt[1], rx, ry, nullptr, nullptr);
  }

  return std::sqrt(d * d + tmpPt[2] * tmpPt[2]);
}

Geo3dEllipseLegacyLike
geo3dEllipseInterpolateLegacyLike(const Geo3dEllipseLegacyLike& start, const Geo3dEllipseLegacyLike& end, double lambda)
{
  // Port of tz_geo3d_ellipse.c::Geo3d_Ellipse_Interpolate().
  Geo3dEllipseLegacyLike out;
  const double alpha = 1.0 - lambda;

  out.center[0] = alpha * start.center[0] + lambda * end.center[0];
  out.center[1] = alpha * start.center[1] + lambda * end.center[1];
  out.center[2] = alpha * start.center[2] + lambda * end.center[2];

  if (start.orientation[0] == end.orientation[0] && start.orientation[1] == end.orientation[1]) {
    out.orientation = start.orientation;
  } else {
    double startNormal[3];
    double endNormal[3];
    geo3dOrientationNormalLegacyLike(start.orientation[0],
                                     start.orientation[1],
                                     startNormal[0],
                                     startNormal[1],
                                     startNormal[2]);
    geo3dOrientationNormalLegacyLike(end.orientation[0], end.orientation[1], endNormal[0], endNormal[1], endNormal[2]);

    const double normal[3] = {alpha * startNormal[0] + lambda * endNormal[0],
                              alpha * startNormal[1] + lambda * endNormal[1],
                              alpha * startNormal[2] + lambda * endNormal[2]};
    geo3dNormalOrientationLegacyLike(normal[0], normal[1], normal[2], out.orientation[0], out.orientation[1]);
  }

  if (start.alpha != 0.0 || end.alpha != 0.0) {
    const double startAlpha = normalizeRadianLegacyLike(start.alpha);
    const double endAlpha = normalizeRadianLegacyLike(end.alpha);
    if (std::fabs(endAlpha - startAlpha) > TzPiLegacyLike) {
      out.alpha = alpha * startAlpha + lambda * (endAlpha - Tz2PiLegacyLike);
    } else {
      out.alpha = alpha * startAlpha + lambda * endAlpha;
    }
  } else {
    out.alpha = 0.0;
  }

  out.radius = alpha * start.radius + lambda * end.radius;
  out.scale = alpha * start.scale + lambda * end.scale;

  return out;
}

} // namespace nim
