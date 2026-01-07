#include "zskeleton.h"

namespace nim {

ZBBox<glm::dvec3> ZSkeleton::boundBox() const
{
  ZBBox<glm::dvec3> box;
  for (const glm::vec3& v : m_vertices) {
    box.expand(glm::dvec3(v.x, v.y, v.z));
  }
  return box;
}

} // namespace nim

