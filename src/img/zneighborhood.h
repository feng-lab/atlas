#ifndef ZNEIGHBORHOOD_H
#define ZNEIGHBORHOOD_H

#include "zvoxelcoordinate.h"
#include <vector>

namespace nim {

// manage neighborhood definition which is a collection of voxel offsets
class ZNeighborhood
{
public:
  explicit ZNeighborhood();

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
  ZNeighborhood(size_t nb);

  // symmetrical box neighborhood, with or without center
  ZNeighborhood(size_t xRadius, size_t yRadius, size_t zRadius, bool includeCenter);

  // box neighborhood, with or without center
  ZNeighborhood(size_t left, size_t right, size_t up, size_t down, size_t front, size_t back, bool includeCenter);

  // arbitrary
  ZNeighborhood(const std::vector<ZVoxelCoordinate>& offsets);

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

  inline const ZVoxelCoordinate& offset(size_t n) const
  { return m_offsets[n]; }

  inline const std::vector<ZVoxelCoordinate>& offsets() const
  { return m_offsets; }

  inline size_t size() const
  { return m_offsets.size(); }

  inline bool empty() const
  { return m_offsets.empty(); }

  inline size_t leftExtend() const
  { return m_leftExtend; }

  inline size_t rightExtend() const
  { return m_rightExtend; }

  inline size_t upExtend() const
  { return m_upExtend; }

  inline size_t downExtend() const
  { return m_downExtend; }

  inline size_t frontExtend() const
  { return m_frontExtend; }

  inline size_t backExtend() const
  { return m_backExtend; }

private:
  std::vector<ZVoxelCoordinate> m_offsets;
  size_t m_leftExtend;
  size_t m_rightExtend;
  size_t m_upExtend;
  size_t m_downExtend;
  size_t m_frontExtend;
  size_t m_backExtend;
};

} // namespace nim

#endif // ZNEIGHBORHOOD_H
