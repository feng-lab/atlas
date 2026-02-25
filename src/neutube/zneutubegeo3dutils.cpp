#include "zneutubegeo3dutils.h"

#include "zneutube3dgeom.h"

#include "zlog.h"

#include <array>
#include <cmath>

namespace nim {

namespace {

constexpr double TzPiLegacyLike = 3.14159265358979323846264338328;
constexpr double TzPi2LegacyLike = TzPiLegacyLike / 2.0;
constexpr double Tz2PiLegacyLike = 2.0 * TzPiLegacyLike;
constexpr double GeoangleCompareEpsLegacyLike = 0.00001;

} // namespace

double vectorAngleLegacyLike(double x, double y)
{
  // Port of tz_geometry.c::Vector_Angle().
  if (x == 0.0 && y == 0.0) {
    return 0.0;
  }

  double angle = 0.0;
  if (x == 0.0) {
    angle = TzPi2LegacyLike;
    if (y < 0.0) {
      angle += TzPiLegacyLike;
    }
  } else {
    angle = std::atan(y / x);
    if (x < 0.0) {
      angle += TzPiLegacyLike;
    }
  }

  if (angle < 0.0) {
    angle += Tz2PiLegacyLike;
  }

  return angle;
}

void geo3dOrientationNormalLegacyLike(double theta, double psi, double& x, double& y, double& z)
{
  // Port of tz_geo3d_utils.c::Geo3d_Orientation_Normal().
  const double sinTheta = std::sin(theta);
  x = sinTheta * std::sin(psi);
  y = -sinTheta * std::cos(psi);
  z = std::sqrt(1.0 - sinTheta * sinTheta);
}

void geo3dNormalOrientationLegacyLike(double x, double y, double z, double& theta, double& psi)
{
  // Port of tz_geo3d_utils.c::Geo3d_Normal_Orientation().
  theta = std::acos(z);

  if (theta < GeoangleCompareEpsLegacyLike) {
    psi = 0.0;
  } else {
    if (y >= 0.0) {
      theta = -theta;
      psi = vectorAngleLegacyLike(x, y) - TzPi2LegacyLike;
    } else {
      psi = vectorAngleLegacyLike(x, y) - TzPi2LegacyLike * 3.0;
    }
  }
}

void geo3dRotateOrientationLegacyLike(double rtheta, double rpsi, double& theta, double& psi)
{
  // Port of tz_geo3d_utils.c::Geo3d_Rotate_Orientation().
  std::array<double, 3> coord{};
  geo3dOrientationNormalLegacyLike(theta, psi, coord[0], coord[1], coord[2]);
  rotateXZLegacyLike(&coord, 1, rtheta, rpsi, 0);
  geo3dNormalOrientationLegacyLike(coord[0], coord[1], coord[2], theta, psi);
}

} // namespace nim
