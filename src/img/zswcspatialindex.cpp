#include "zswcspatialindex.h"

#include "zlog.h"

#include <algorithm>
#include <cmath>

namespace nim {

namespace {

namespace bgi = boost::geometry::index;

[[nodiscard]] double safeRadius(double r)
{
  // SWC radii may come from user-provided files. Treat non-finite/negative radii as zero rather than CHECK-crashing.
  if (!std::isfinite(r) || r <= 0.0) {
    return 0.0;
  }
  return r;
}

[[nodiscard]] bool isFiniteVec3(const glm::dvec3& v)
{
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

void ZSwcSpatialIndex::setZScale(double zScale)
{
  CHECK(std::isfinite(zScale));
  CHECK(zScale > 0.0);
  const double v = zScale;
  if (v == m_zScale) {
    return;
  }

  // Changing zScale changes the coordinate transform; any existing primitives would become invalid.
  clear();
  m_zScale = v;
}

double ZSwcSpatialIndex::zScale() const
{
  return m_zScale;
}

void ZSwcSpatialIndex::clear()
{
  m_prims.clear();
  m_rtree.clear();
}

void ZSwcSpatialIndex::rebuild(const ZSwc& swc)
{
  clear();

  if (swc.empty()) {
    return;
  }

  // Reserve a conservative estimate: one primitive per edge plus roots.
  const size_t reserveCount = swc.size();
  m_prims.reserve(reserveCount);

  for (auto it = swc.cbeginBreadthFirst(); it != swc.cendBreadthFirst(); ++it) {
    if (ZSwc::isNull(it)) {
      continue;
    }

    const glm::dvec3 p{it->x, it->y, it->z};
    if (!isFiniteVec3(p)) {
      continue;
    }

    const auto parent = ZSwc::parent(it);
    if (ZSwc::isNull(parent)) {
      // Root node: represent as a sphere (degenerate segment).
      insertSegment(p, p, safeRadius(it->radius), safeRadius(it->radius));
      continue;
    }

    const glm::dvec3 q{parent->x, parent->y, parent->z};
    if (!isFiniteVec3(q)) {
      continue;
    }

    insertSegment(q, p, safeRadius(parent->radius), safeRadius(it->radius));
  }
}

void ZSwcSpatialIndex::insertSegment(glm::dvec3 a, glm::dvec3 b, double ra, double rb)
{
  if (!isFiniteVec3(a) || !isFiniteVec3(b)) {
    return;
  }

  Primitive prim;
  prim.a = glm::dvec3{a.x, a.y, a.z * m_zScale};
  prim.b = glm::dvec3{b.x, b.y, b.z * m_zScale};
  prim.ra = safeRadius(ra);
  prim.rb = safeRadius(rb);

  const size_t idx = m_prims.size();
  m_prims.push_back(prim);
  m_rtree.insert(RTreeValue{primitiveBox(prim), idx});
}

bool ZSwcSpatialIndex::empty() const
{
  return m_prims.empty();
}

size_t ZSwcSpatialIndex::primitiveCount() const
{
  return m_prims.size();
}

bool ZSwcSpatialIndex::containsPoint(double x, double y, double z) const
{
  return containsPoint(glm::dvec3{x, y, z});
}

bool ZSwcSpatialIndex::containsPoint(glm::dvec3 p) const
{
  if (m_prims.empty()) {
    return false;
  }

  if (!isFiniteVec3(p)) {
    return false;
  }

  p.z *= m_zScale;
  const Point3 pt(p.x, p.y, p.z);

  for (auto it = m_rtree.qbegin(bgi::intersects(pt)); it != m_rtree.qend(); ++it) {
    const size_t idx = it->second;
    if (idx >= m_prims.size()) {
      CHECK(false) << "ZSwcSpatialIndex: rtree returned invalid primitive index";
    }
    if (primitiveContainsPoint(m_prims[idx], p)) {
      return true;
    }
  }

  return false;
}

ZSwcSpatialIndex::Box3 ZSwcSpatialIndex::primitiveBox(const Primitive& p)
{
  const double r = std::max(p.ra, p.rb);

  const double minX = std::min(p.a.x, p.b.x) - r;
  const double minY = std::min(p.a.y, p.b.y) - r;
  const double minZ = std::min(p.a.z, p.b.z) - r;

  const double maxX = std::max(p.a.x, p.b.x) + r;
  const double maxY = std::max(p.a.y, p.b.y) + r;
  const double maxZ = std::max(p.a.z, p.b.z) + r;

  return Box3{Point3(minX, minY, minZ), Point3(maxX, maxY, maxZ)};
}

bool ZSwcSpatialIndex::primitiveContainsPoint(const Primitive& prim, const glm::dvec3& p)
{
  const glm::dvec3 a = prim.a;
  const glm::dvec3 b = prim.b;

  const glm::dvec3 ab = b - a;
  const double ab2 = glm::dot(ab, ab);

  if (ab2 <= 1e-12) {
    const double r = prim.ra;
    if (r <= 0.0) {
      return false;
    }
    const glm::dvec3 d = p - a;
    return glm::dot(d, d) <= r * r;
  }

  const glm::dvec3 ap = p - a;
  double t = glm::dot(ap, ab) / ab2;

  if (t <= 0.0) {
    const double r = prim.ra;
    if (r <= 0.0) {
      return false;
    }
    const glm::dvec3 d = p - a;
    return glm::dot(d, d) <= r * r;
  }

  if (t >= 1.0) {
    const double r = prim.rb;
    if (r <= 0.0) {
      return false;
    }
    const glm::dvec3 d = p - b;
    return glm::dot(d, d) <= r * r;
  }

  const glm::dvec3 closest = a + t * ab;
  const glm::dvec3 d = p - closest;
  const double dist2 = glm::dot(d, d);

  const double r = prim.ra + t * (prim.rb - prim.ra);
  if (r <= 0.0) {
    return false;
  }
  return dist2 <= r * r;
}

} // namespace nim
