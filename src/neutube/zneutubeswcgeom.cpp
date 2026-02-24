#include "zneutubeswcgeom.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>

namespace nim::neutube {

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
                             double* lambda)
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

  if (lambda != nullptr) {
    *lambda = t;
  }

  const double cx = (1.0 - t) * lineStart[0] + t * lineEnd[0];
  const double cy = (1.0 - t) * lineStart[1] + t * lineEnd[1];
  const double cz = (1.0 - t) * lineStart[2] + t * lineEnd[2];
  return geo3dDist(point[0], point[1], point[2], cx, cy, cz);
}

std::array<double, 3>
geo3dLineSegBreak(const std::array<double, 3>& lineStart, const std::array<double, 3>& lineEnd, double lambda)
{
  return {lambda * lineEnd[0] + (1.0 - lambda) * lineStart[0],
          lambda * lineEnd[1] + (1.0 - lambda) * lineStart[1],
          lambda * lineEnd[2] + (1.0 - lambda) * lineStart[2]};
}

} // namespace nim::neutube
