#pragma once

#include "zvoxelcoordinate.h"
#include <vector>

namespace nim {

// manage neighborhood definition which is a collection of voxel offsets
class ZNeighborhood
{
public:
  ZNeighborhood() = default;

  // 2D
  // 4 4-connected neighborhood without center
  // 5 4-connected neighborhood with center
  // 8 8-connected neighborhood without center
  // 9 8-connected neighborhood with center
  // 3D
  // 6 6-connected neighborhood without center
  // 7 6-connected neighborhood with center
  // 18 18-connected neighborhood without center
  // 19 18-connected neighborhood with center
  // 26 26-connected neighborhood without center
  // 27 26-connected neighborhood with center
  explicit ZNeighborhood(size_t nb);

  // symmetrical box neighborhood, with or without center
  ZNeighborhood(size_t xRadius, size_t yRadius, size_t zRadius, bool includeCenter);

  // box neighborhood, with or without center
  ZNeighborhood(size_t left, size_t right, size_t up, size_t down, size_t front, size_t back, bool includeCenter);

  // arbitrary
  explicit ZNeighborhood(const std::vector<ZVoxelCoordinate>& offsets);

  // same as constructor, function version

  void set(size_t nb);

  void set(size_t xRadius, size_t yRadius, size_t zRadius, bool includeCenter);

  void set(size_t left, size_t right, size_t up, size_t down, size_t front, size_t back, bool includeCenter);

  // arbitrary
  void set(const std::vector<ZVoxelCoordinate>& offsets);

  // leave only one offset if two offsets are symmetrical about center,
  // can be useful if we only need to process one connection once
  void removeSymmetricalOffsets();

  // remove center
  void removeCenter();

  // access
  inline ZVoxelCoordinate& operator[](size_t n)
  { return m_offsets[n]; }

  inline const ZVoxelCoordinate& operator[](size_t n) const
  { return m_offsets[n]; }

  inline ZVoxelCoordinate& offset(size_t n)
  { return m_offsets[n]; }

  [[nodiscard]] inline const ZVoxelCoordinate& offset(size_t n) const
  { return m_offsets[n]; }

  [[nodiscard]] inline const std::vector<ZVoxelCoordinate>& offsets() const
  { return m_offsets; }

  [[nodiscard]] inline size_t size() const
  { return m_offsets.size(); }

  [[nodiscard]] inline bool empty() const
  { return m_offsets.empty(); }

  [[nodiscard]] inline size_t leftExtend() const
  { return m_leftExtend; }

  [[nodiscard]] inline size_t rightExtend() const
  { return m_rightExtend; }

  [[nodiscard]] inline size_t upExtend() const
  { return m_upExtend; }

  [[nodiscard]] inline size_t downExtend() const
  { return m_downExtend; }

  [[nodiscard]] inline size_t frontExtend() const
  { return m_frontExtend; }

  [[nodiscard]] inline size_t backExtend() const
  { return m_backExtend; }

private:
  std::vector<ZVoxelCoordinate> m_offsets;
  size_t m_leftExtend = 0;
  size_t m_rightExtend = 0;
  size_t m_upExtend = 0;
  size_t m_downExtend = 0;
  size_t m_frontExtend = 0;
  size_t m_backExtend = 0;
};

} // namespace nim

