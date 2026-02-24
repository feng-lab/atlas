#include "zneutubegeo3dpointarray.h"

#include "zlog.h"

#include <cmath>

namespace nim::neutube {

void geo3dPointArrayTranslateLegacyLike(std::array<double, 3>* points, int n, double dx, double dy, double dz)
{
  CHECK(points != nullptr);
  CHECK(n >= 0);

  for (int i = 0; i < n; ++i) {
    points[i][0] += dx;
    points[i][1] += dy;
    points[i][2] += dz;
  }
}

void geo3dPointArrayBendLegacyLike(std::array<double, 3>* points, int n, double c)
{
  CHECK(points != nullptr);
  CHECK(n >= 0);

  for (int i = 0; i < n; ++i) {
    const double d = c - points[i][1];
    if (d == 0.0) {
      points[i][1] = 0.0;
      points[i][2] = 0.0;
    } else {
      points[i][2] /= d;
      points[i][1] += d - d * std::cos(points[i][2]);
      points[i][2] = std::sin(points[i][2]);
    }

    points[i][2] *= d;
  }
}

} // namespace nim::neutube
