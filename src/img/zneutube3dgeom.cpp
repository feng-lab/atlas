#include "zneutube3dgeom.h"

#include "zlog.h"

#include <cmath>

namespace nim {

void rotateXZLegacyLike(std::array<double, 3>* points, int n, double theta, double psi, int inverse)
{
  CHECK(points != nullptr);
  CHECK(n >= 0);

  const double ar0 = std::cos(theta);
  const double ar1 = std::sin(theta);
  const double ar2 = std::cos(psi);
  const double ar3 = std::sin(psi);

  if (inverse == 0) {
    for (int i = 0; i < n; ++i) {
      const double inx = points[i][0];
      const double iny = points[i][1];
      const double inz = points[i][2];

      double result2 = ar1 * iny + ar0 * inz;
      double result0 = inz * ar1 - iny * ar0;
      double result1 = inx * ar3 - result0 * ar2;
      result0 = inx * ar2 + result0 * ar3;

      points[i][0] = result0;
      points[i][1] = result1;
      points[i][2] = result2;
    }
  } else {
    for (int i = 0; i < n; ++i) {
      const double inx = points[i][0];
      const double iny = points[i][1];
      const double inz = points[i][2];

      double result0 = ar2 * inx + ar3 * iny;
      double result1 = iny * ar2 - inx * ar3;
      double result2 = inz * ar0 - result1 * ar1;
      result1 = inz * ar1 + result1 * ar0;

      points[i][0] = result0;
      points[i][1] = result1;
      points[i][2] = result2;
    }
  }
}

void rotateZLegacyLike(std::array<double, 3>* points, int n, double alpha, int inverse)
{
  CHECK(points != nullptr);
  CHECK(n >= 0);

  if (alpha == 0.0) {
    return;
  }

  const double cosA = std::cos(alpha);
  const double sinA = std::sin(alpha);

  if (inverse == 0) {
    for (int i = 0; i < n; ++i) {
      const double x = points[i][0];
      const double y = points[i][1];

      const double tmp = x * cosA - y * sinA;
      points[i][1] = x * sinA + y * cosA;
      points[i][0] = tmp;
    }
  } else {
    for (int i = 0; i < n; ++i) {
      const double x = points[i][0];
      const double y = points[i][1];

      const double tmp = x * cosA + y * sinA;
      points[i][1] = -x * sinA + y * cosA;
      points[i][0] = tmp;
    }
  }
}

} // namespace nim
