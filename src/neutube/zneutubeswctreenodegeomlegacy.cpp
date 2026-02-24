#include "zneutubeswctreenodegeomlegacy.h"

#include "zlog.h"

#include <cmath>

namespace nim::neutube {

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

std::vector<ZSwc::SwcTreeNode> swcNeighborArrayLegacyLike(ZSwc* swc, const ZSwc::SwcTreeNode& node)
{
  CHECK(swc != nullptr);
  if (ZSwc::isNull(node)) {
    return {};
  }

  std::vector<ZSwc::SwcTreeNode> out;
  out.reserve(8);

  const auto p = ZSwc::parent(node);
  if (!ZSwc::isNull(p)) {
    out.push_back(p);
  }

  for (auto child = swc->beginChild(node); child != swc->endChild(node); ++child) {
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

} // namespace nim::neutube
