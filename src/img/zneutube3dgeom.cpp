#include "zneutube3dgeom.h"

#include "zlog.h"

#include <cmath>

namespace nim {

void rotateXZLegacyLike(std::span<std::array<double, 3>> points, double theta, double psi, int inverse)
{
  if (points.empty()) {
    return;
  }

  const double ar0 = std::cos(theta);
  const double ar1 = std::sin(theta);
  const double ar2 = std::cos(psi);
  const double ar3 = std::sin(psi);

  if (inverse == 0) {
    for (auto& point : points) {
      const double inx = point[0];
      const double iny = point[1];
      const double inz = point[2];

      double result2 = ar1 * iny + ar0 * inz;
      double result0 = inz * ar1 - iny * ar0;
      double result1 = inx * ar3 - result0 * ar2;
      result0 = inx * ar2 + result0 * ar3;

      point[0] = result0;
      point[1] = result1;
      point[2] = result2;
    }
  } else {
    for (auto& point : points) {
      const double inx = point[0];
      const double iny = point[1];
      const double inz = point[2];

      double result0 = ar2 * inx + ar3 * iny;
      double result1 = iny * ar2 - inx * ar3;
      double result2 = inz * ar0 - result1 * ar1;
      result1 = inz * ar1 + result1 * ar0;

      point[0] = result0;
      point[1] = result1;
      point[2] = result2;
    }
  }
}

void rotateXZLegacyLike(std::array<double, 3>& point, double theta, double psi, int inverse)
{
  rotateXZLegacyLike(std::span<std::array<double, 3>>(&point, 1), theta, psi, inverse);
}

void rotateZLegacyLike(std::span<std::array<double, 3>> points, double alpha, int inverse)
{
  if (points.empty()) {
    return;
  }

  if (alpha == 0.0) {
    return;
  }

  const double cosA = std::cos(alpha);
  const double sinA = std::sin(alpha);

  if (inverse == 0) {
    for (auto& point : points) {
      const double x = point[0];
      const double y = point[1];

      const double tmp = x * cosA - y * sinA;
      point[1] = x * sinA + y * cosA;
      point[0] = tmp;
    }
  } else {
    for (auto& point : points) {
      const double x = point[0];
      const double y = point[1];

      const double tmp = x * cosA + y * sinA;
      point[1] = -x * sinA + y * cosA;
      point[0] = tmp;
    }
  }
}

void rotateZLegacyLike(std::array<double, 3>& point, double alpha, int inverse)
{
  rotateZLegacyLike(std::span<std::array<double, 3>>(&point, 1), alpha, inverse);
}

} // namespace nim
