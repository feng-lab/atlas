#include "zskeleton.h"

#include "zlog.h"

#include <limits>

namespace nim {

void ZSkeleton::appendGeometry(std::vector<glm::vec3> v, std::vector<glm::uvec2> e)
{
  if (v.empty() || e.empty()) {
    return;
  }

  CHECK(m_radii.empty()) << "appendGeometry is not supported when radii are present";

  const size_t base = m_vertices.size();
  CHECK(base <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
  const uint32_t base32 = static_cast<uint32_t>(base);

  // Validate and offset edges.
  for (glm::uvec2& edge : e) {
    CHECK(edge.x < v.size());
    CHECK(edge.y < v.size());
    edge.x += base32;
    edge.y += base32;
  }

  m_vertices.insert(m_vertices.end(), v.begin(), v.end());
  m_edges.insert(m_edges.end(), e.begin(), e.end());
}

ZBBox<glm::dvec3> ZSkeleton::boundBox() const
{
  ZBBox<glm::dvec3> box;
  for (const glm::vec3& v : m_vertices) {
    box.expand(glm::dvec3(v.x, v.y, v.z));
  }
  return box;
}

} // namespace nim
