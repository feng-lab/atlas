#pragma once

#include "zbbox.h"
#include "zglmutils.h"

#include <cstdint>
#include <vector>

namespace nim {

// A lightweight graph skeleton: vertices + undirected edges, with an optional per-vertex radius.
// Coordinates are in the same "local voxel" space used by Atlas volumes/meshes.
class ZSkeleton
{
public:
  void swap(ZSkeleton& rhs) noexcept
  {
    m_vertices.swap(rhs.m_vertices);
    m_edges.swap(rhs.m_edges);
    m_radii.swap(rhs.m_radii);
  }

  [[nodiscard]] bool empty() const
  {
    return m_vertices.empty() || m_edges.empty();
  }

  void clear()
  {
    m_vertices.clear();
    m_edges.clear();
    m_radii.clear();
  }

  [[nodiscard]] size_t numVertices() const
  {
    return m_vertices.size();
  }

  [[nodiscard]] size_t numEdges() const
  {
    return m_edges.size();
  }

  [[nodiscard]] const std::vector<glm::vec3>& vertices() const
  {
    return m_vertices;
  }

  [[nodiscard]] const std::vector<glm::uvec2>& edges() const
  {
    return m_edges;
  }

  // Optional per-vertex radii. When present, size must match numVertices().
  [[nodiscard]] const std::vector<float>& radii() const
  {
    return m_radii;
  }

  [[nodiscard]] bool hasRadii() const
  {
    return !m_radii.empty();
  }

  void setVertices(std::vector<glm::vec3> v)
  {
    m_vertices = std::move(v);
  }

  void setEdges(std::vector<glm::uvec2> e)
  {
    m_edges = std::move(e);
  }

  void setRadii(std::vector<float> r)
  {
    m_radii = std::move(r);
  }

  [[nodiscard]] ZBBox<glm::dvec3> boundBox() const;

private:
  std::vector<glm::vec3> m_vertices;
  std::vector<glm::uvec2> m_edges;
  std::vector<float> m_radii;
};

} // namespace nim
