#include "zswcpointdist.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>

namespace nim {

namespace {

struct SegmentDistResult
{
  double dist = 0.0;
  std::array<double, 3> closestPoint = {0.0, 0.0, 0.0};
};

[[nodiscard]] SegmentDistResult frustumPointDist(const std::array<double, 3>& bottom,
                                                 const std::array<double, 3>& top,
                                                 double rBottom,
                                                 double rTop,
                                                 const std::array<double, 3>& point)
{
  const double ax = top[0] - bottom[0];
  const double ay = top[1] - bottom[1];
  const double az = top[2] - bottom[2];
  const double len = std::sqrt(ax * ax + ay * ay + az * az);

  // Legacy Local_Neuroseg_Change_Top() treats very short segments as h=1.0 (degenerate).
  // Treat as a sphere-like endpoint for distance purposes.
  if (len < 0.1) {
    const double dx = point[0] - bottom[0];
    const double dy = point[1] - bottom[1];
    const double dz = point[2] - bottom[2];
    const double centerDist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double d = std::max(0.0, centerDist - rBottom);
    return {d, bottom};
  }

  const double ux = ax / len;
  const double uy = ay / len;
  const double uz = az / len;

  const double qx = point[0] - bottom[0];
  const double qy = point[1] - bottom[1];
  const double qz = point[2] - bottom[2];

  const double z = qx * ux + qy * uy + qz * uz; // axial coordinate in [0, len]

  const double rx = qx - z * ux;
  const double ry = qy - z * uy;
  const double rz = qz - z * uz;

  const double rho = std::sqrt(rx * rx + ry * ry + rz * rz);

  const double adjustedHeight = len;
  double coef = 1.0;
  if (adjustedHeight >= 0.001) {
    coef = (rTop - rBottom) / adjustedHeight;
  }

  auto radiusAt = [rBottom, coef](double h) -> double {
    return rBottom + coef * h;
  };

  // Hit test: inside the segment volume => distance 0, closest point is the point itself.
  if (z >= 0.0 && z <= len) {
    const double rZ = radiusAt(z);
    if (rho <= rZ) {
      return {0.0, point};
    }
  }

  // Unit radial direction (world space), for constructing boundary points.
  const bool hasRadial = (rho > 0.0);
  const double urx = hasRadial ? (rx / rho) : 0.0;
  const double ury = hasRadial ? (ry / rho) : 0.0;
  const double urz = hasRadial ? (rz / rho) : 0.0;

  auto axisPoint = [bottom, ux, uy, uz](double h) -> std::array<double, 3> {
    return {bottom[0] + ux * h, bottom[1] + uy * h, bottom[2] + uz * h};
  };

  auto boundaryPoint = [bottom, ux, uy, uz, urx, ury, urz](double h, double r) -> std::array<double, 3> {
    return {bottom[0] + ux * h + urx * r, bottom[1] + uy * h + ury * r, bottom[2] + uz * h + urz * r};
  };

  SegmentDistResult best;

  if (z <= 0.0) { // below bottom
    const double r = rBottom;
    if (rho <= r) {
      best.dist = -z;
      best.closestPoint = axisPoint(0.0);
    } else {
      const double dxy = std::abs(rho - r);
      best.dist = std::sqrt(z * z + dxy * dxy);
      best.closestPoint = boundaryPoint(0.0, r);
    }
    return best;
  }

  if (z >= len) { // above top
    const double r = radiusAt(len);
    const double dz = z - len;
    if (rho <= r) {
      best.dist = dz;
      best.closestPoint = axisPoint(len);
    } else {
      const double dxy = std::abs(rho - r);
      best.dist = std::sqrt(dz * dz + dxy * dxy);
      best.closestPoint = boundaryPoint(len, r);
    }
    return best;
  }

  // 0 < z < len: lateral case
  const double rZ = radiusAt(z);
  best.dist = std::abs(rho - rZ);
  best.closestPoint = boundaryPoint(z, rZ);

  if (coef != 0.0) {
    double ryCurrent = rBottom;
    for (double h = 0.5; h < len; h += 0.5) {
      ryCurrent += coef * 0.5;
      const double dxy = std::abs(rho - ryCurrent);
      const double dz = z - h;
      const double d = std::sqrt(dxy * dxy + dz * dz);
      if (d < best.dist) {
        best.dist = d;
        best.closestPoint = boundaryPoint(h, ryCurrent);
      }
    }
  }

  return best;
}

} // namespace

bool swcTreeHitTest(const ZSwc& tree, double x, double y, double z)
{
  const std::array<double, 3> point = {x, y, z};

  for (auto it = tree.begin(); it != tree.end(); ++it) {
    if (it->id < 0) {
      continue;
    }

    const double dx = x - it->x;
    const double dy = y - it->y;
    const double dz = z - it->z;
    const double r = it->radius;
    if ((dx * dx + dy * dy + dz * dz) <= (r * r)) {
      return true;
    }

    if (ZSwc::isRoot(it)) {
      continue;
    }

    const auto parent = ZSwc::parent(it);
    if (ZSwc::isNull(parent) || parent->id < 0) {
      continue;
    }

    const std::array<double, 3> bottom = {parent->x, parent->y, parent->z};
    const std::array<double, 3> top = {it->x, it->y, it->z};
    const SegmentDistResult rSeg = frustumPointDist(bottom, top, parent->radius, it->radius, point);
    if (rSeg.dist == 0.0) {
      return true;
    }
  }

  return false;
}

SwcPointDistResult swcTreePointDist(ZSwc& tree, double x, double y, double z)
{
  return swcTreePointDist(tree, x, y, z, ZSwc::SwcTreeNode{});
}

SwcPointDistResult swcTreePointDist(ZSwc& tree, double x, double y, double z, const ZSwc::SwcTreeNode& excludeRoot)
{
  SwcPointDistResult out;
  const std::array<double, 3> point = {x, y, z};

  for (auto it = tree.begin(); it != tree.end(); ++it) {
    if (ZSwc::isRoot(it)) {
      continue;
    }

    if (!ZSwc::isNull(excludeRoot)) {
      const auto r = ZSwc::root(it);
      if (r == excludeRoot) {
        continue;
      }
    }

    const auto parent = ZSwc::parent(it);
    CHECK(!ZSwc::isNull(parent));

    const std::array<double, 3> bottom = {parent->x, parent->y, parent->z};
    const std::array<double, 3> top = {it->x, it->y, it->z};
    const SegmentDistResult r = frustumPointDist(bottom, top, parent->radius, it->radius, point);

    if ((out.dist > r.dist) || (out.dist < 0.0)) {
      out.dist = r.dist;
      out.closestPoint = r.closestPoint;
      out.closestNode = it;
    }

    if (out.dist == 0.0) {
      break;
    }
  }

  return out;
}

} // namespace nim
