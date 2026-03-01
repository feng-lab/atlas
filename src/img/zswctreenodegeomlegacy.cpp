#include "zswctreenodegeomlegacy.h"

#include "zlog.h"
#include "zswcnodeops.h"

#include <cmath>
#include <limits>
#include <vector>

namespace nim {

double swcNodeDistanceLegacyLike(const ZSwc::ConstSwcTreeNode& a,
                                 const ZSwc::ConstSwcTreeNode& b,
                                 double sx,
                                 double sy,
                                 double sz)
{
  CHECK(!ZSwc::isNull(a));
  CHECK(!ZSwc::isNull(b));

  const double dx = (a->x - b->x) * sx;
  const double dy = (a->y - b->y) * sy;
  const double dz = (a->z - b->z) * sz;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double swcNodeSurfaceDistanceLegacyLike(const ZSwc::ConstSwcTreeNode& a,
                                        const ZSwc::ConstSwcTreeNode& b,
                                        double sx,
                                        double sy,
                                        double sz)
{
  const double d = swcNodeDistanceLegacyLike(a, b, sx, sy, sz);
  return d - a->radius - b->radius;
}

bool swcNodesHasOverlapLegacyLike(const ZSwc::ConstSwcTreeNode& a, const ZSwc::ConstSwcTreeNode& b)
{
  CHECK(!ZSwc::isNull(a));
  CHECK(!ZSwc::isNull(b));

  const double d = swcNodeDistanceLegacyLike(a, b);
  return d < (a->radius + b->radius);
}

bool swcNodesHasSignificantOverlapLegacyLike(const ZSwc::ConstSwcTreeNode& a, const ZSwc::ConstSwcTreeNode& b)
{
  CHECK(!ZSwc::isNull(a));
  CHECK(!ZSwc::isNull(b));

  const double d = swcNodeDistanceLegacyLike(a, b);
  return (d < a->radius) || (d < b->radius);
}

bool swcNodesFormingTurnLegacyLike(const ZSwc::ConstSwcTreeNode& tn1,
                                   const ZSwc::ConstSwcTreeNode& tn2,
                                   const ZSwc::ConstSwcTreeNode& tn3)
{
  CHECK(!ZSwc::isNull(tn1));
  CHECK(!ZSwc::isNull(tn2));
  CHECK(!ZSwc::isNull(tn3));

  const auto p2 = ZSwc::parent(tn2);
  if (ZSwc::isNull(p2)) {
    return false;
  }

  const bool adjacent = ((p2 == tn1) && (ZSwc::parent(tn3) == tn2)) || ((p2 == tn3) && (ZSwc::parent(tn1) == tn2));
  if (!adjacent) {
    return false;
  }

  double v1x = tn1->x - tn2->x;
  double v1y = tn1->y - tn2->y;
  double v1z = tn1->z - tn2->z;
  double v2x = tn2->x - tn3->x;
  double v2y = tn2->y - tn3->y;
  double v2z = tn2->z - tn3->z;

  const double n1 = std::sqrt(v1x * v1x + v1y * v1y + v1z * v1z);
  if (n1 > 0.0) {
    v1x /= n1;
    v1y /= n1;
    v1z /= n1;
  }

  const double n2 = std::sqrt(v2x * v2x + v2y * v2y + v2z * v2z);
  if (n2 > 0.0) {
    v2x /= n2;
    v2y /= n2;
    v2z /= n2;
  }

  const double dot = v1x * v2x + v1y * v2y + v1z * v2z;
  return !(dot > 0.0);
}

std::vector<ZSwc::SwcTreeNode> swcNeighborArrayLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& node)
{
  if (ZSwc::isNull(node)) {
    return {};
  }

  std::vector<ZSwc::SwcTreeNode> out;
  out.reserve(8);

  const auto p = ZSwc::parent(node);
  if (!ZSwc::isNull(p)) {
    out.push_back(p);
  }

  for (auto child = swc.beginChild(node); child != swc.endChild(node); ++child) {
    out.push_back(child);
  }

  return out;
}

void swcNodeAverageLegacyLike(const ZSwc::ConstSwcTreeNode& a, const ZSwc::ConstSwcTreeNode& b, ZSwc::SwcTreeNode out)
{
  CHECK(!ZSwc::isNull(a));
  CHECK(!ZSwc::isNull(b));
  CHECK(!ZSwc::isNull(out));

  out->radius = std::max(0.0, (a->radius + b->radius) * 0.5);
  out->x = (a->x + b->x) * 0.5;
  out->y = (a->y + b->y) * 0.5;
  out->z = (a->z + b->z) * 0.5;
}

void swcNodeInterpolateLegacyLike(const ZSwc::ConstSwcTreeNode& a,
                                  const ZSwc::ConstSwcTreeNode& b,
                                  double lambda,
                                  ZSwc::SwcTreeNode out)
{
  CHECK(!ZSwc::isNull(a));
  CHECK(!ZSwc::isNull(b));
  CHECK(!ZSwc::isNull(out));

  out->radius = std::max(0.0, a->radius * lambda + b->radius * (1.0 - lambda));
  out->x = a->x * lambda + b->x * (1.0 - lambda);
  out->y = a->y * lambda + b->y * (1.0 - lambda);
  out->z = a->z * lambda + b->z * (1.0 - lambda);
}

namespace {

[[nodiscard]] double normalizedDotLegacyLike(const ZSwc::ConstSwcTreeNode& tn1,
                                             const ZSwc::ConstSwcTreeNode& tn2,
                                             const ZSwc::ConstSwcTreeNode& tn3)
{
  CHECK(!ZSwc::isNull(tn1));
  CHECK(!ZSwc::isNull(tn2));
  CHECK(!ZSwc::isNull(tn3));

  const auto p2 = ZSwc::parent(tn2);
  if (ZSwc::isNull(p2)) {
    return 0.0;
  }

  const bool adjacent = ((p2 == tn1) && (ZSwc::parent(tn3) == tn2)) || ((p2 == tn3) && (ZSwc::parent(tn1) == tn2));
  if (!adjacent) {
    return 0.0;
  }

  double v1x = tn1->x - tn2->x;
  double v1y = tn1->y - tn2->y;
  double v1z = tn1->z - tn2->z;
  double v2x = tn2->x - tn3->x;
  double v2y = tn2->y - tn3->y;
  double v2z = tn2->z - tn3->z;

  const double n1 = std::sqrt(v1x * v1x + v1y * v1y + v1z * v1z);
  if (n1 > 0.0) {
    v1x /= n1;
    v1y /= n1;
    v1z /= n1;
  }

  const double n2 = std::sqrt(v2x * v2x + v2y * v2y + v2z * v2z);
  if (n2 > 0.0) {
    v2x /= n2;
    v2y /= n2;
    v2z /= n2;
  }

  return v1x * v2x + v1y * v2y + v1z * v2z;
}

[[nodiscard]] double pathLengthRatioLegacyLike(const ZSwc::ConstSwcTreeNode& tn1,
                                               const ZSwc::ConstSwcTreeNode& tn2,
                                               const ZSwc::ConstSwcTreeNode& tn)
{
  CHECK(!ZSwc::isNull(tn1));
  CHECK(!ZSwc::isNull(tn2));
  CHECK(!ZSwc::isNull(tn));

  const double d1 = swcNodeDistanceLegacyLike(tn1, tn);
  const double d2 = swcNodeDistanceLegacyLike(tn2, tn);

  if (d1 == 0.0 && d2 == 0.0) {
    return 0.5;
  }
  if (d1 == 0.0) {
    return 0.0;
  }

  if (std::isinf(d1) && std::isinf(d2)) {
    return 0.5;
  }
  if (std::isinf(d1)) {
    return 1.0;
  }
  if (std::isinf(d2)) {
    return 0.0;
  }

  return d1 / (d1 + d2);
}

[[nodiscard]] double cosAngle(const glm::dvec3& v1, const glm::dvec3& v2)
{
  const double n1 = glm::length(v1);
  const double n2 = glm::length(v2);
  if (n1 <= 0.0 || n2 <= 0.0) {
    return 1.0;
  }
  return glm::dot(v1, v2) / (n1 * n2);
}

} // namespace

void swcNodeCorrectTurnLegacyLike(ZSwc& swc, const ZSwc::SwcTreeNode& tn)
{
  if (ZSwc::isNull(tn)) {
    return;
  }

  ZSwc::SwcTreeNode tn1 = ZSwc::SwcTreeNode{};
  ZSwc::SwcTreeNode tn2 = ZSwc::SwcTreeNode{};

  if (isContinuation(tn)) {
    tn1 = ZSwc::firstChild(tn);
    tn2 = ZSwc::parent(tn);
  } else {
    const std::vector<ZSwc::SwcTreeNode> neighbors = swcNeighborArrayLegacyLike(swc, tn);
    double minDot = 0.0;
    for (size_t i = 0; i < neighbors.size(); ++i) {
      for (size_t j = 0; j < neighbors.size(); ++j) {
        if (i == j) {
          continue;
        }
        const double dot = normalizedDotLegacyLike(neighbors[i], tn, neighbors[j]);
        if (dot < minDot) {
          minDot = dot;
          tn1 = neighbors[i];
          tn2 = neighbors[j];
        }
      }
    }
  }

  if (ZSwc::isNull(tn1) || ZSwc::isNull(tn2)) {
    return;
  }

  if (!swcNodesFormingTurnLegacyLike(tn1, tn, tn2)) {
    return;
  }

  const double lambda = pathLengthRatioLegacyLike(tn2, tn1, tn);
  swcNodeInterpolateLegacyLike(tn1, tn2, lambda, tn);
}

double swcNodeMaxBendingEnergyLegacyLike(const ZSwc& swc, const ZSwc::ConstSwcTreeNode& tn)
{
  if (ZSwc::isNull(tn)) {
    return 0.0;
  }
  if (isBranchPoint(tn)) {
    return 0.0;
  }

  std::vector<glm::dvec3> points;
  points.reserve(5);

  // Insert in the same order as legacy: [grandparent, parent, self, child, grandchild] when available.
  points.push_back(glm::dvec3(tn->x, tn->y, tn->z));

  if (swc.numChildren(tn) == 1) {
    const auto child = ZSwc::firstChild(tn);
    if (!ZSwc::isNull(child)) {
      points.push_back(glm::dvec3(child->x, child->y, child->z));

      if (swc.numChildren(child) == 1) {
        const auto grandchild = ZSwc::firstChild(child);
        if (!ZSwc::isNull(grandchild)) {
          points.push_back(glm::dvec3(grandchild->x, grandchild->y, grandchild->z));
        }
      }
    }
  }

  if (!isRoot(tn)) {
    const auto parent = ZSwc::parent(tn);
    if (!ZSwc::isNull(parent)) {
      points.insert(points.begin(), glm::dvec3(parent->x, parent->y, parent->z));

      if (!isRoot(parent)) {
        const auto grandparent = ZSwc::parent(parent);
        if (!ZSwc::isNull(grandparent)) {
          points.insert(points.begin(), glm::dvec3(grandparent->x, grandparent->y, grandparent->z));
        }
      }
    }
  }

  if (points.size() <= 2) {
    return 0.0;
  }

  glm::dvec3 v1 = points[1] - points[0];
  glm::dvec3 v2 = points[2] - points[1];
  double e = cosAngle(v1, v2);

  for (size_t i = 3; i < points.size(); ++i) {
    v1 = v2;
    v2 = points[i] - points[i - 1];
    const double tmpE = cosAngle(v1, v2);
    if (tmpE < e) {
      e = tmpE;
    }
  }

  return 1.0 - e;
}

} // namespace nim
