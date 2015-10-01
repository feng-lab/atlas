#ifndef ZVOXELREGION_H
#define ZVOXELREGION_H

#include "zvoxelcoordinate.h"
#include "zbbox.h"
#include <boost/iterator/iterator_facade.hpp>
#include <type_traits>

namespace nim {

namespace impl {

template<class TVoxelRegion>
class voxel_iter
    : public boost::iterator_facade<
    voxel_iter<TVoxelRegion>
    , ZVoxelCoordinate
    , boost::forward_traversal_tag
    , ZVoxelCoordinate
    >
{
  struct enabler {};
public:
  voxel_iter()
    : m_region(nullptr), m_boxIdx(0), m_voxel() {}

  explicit voxel_iter(const TVoxelRegion *region, size_t boxIdx)
    : m_region(region), m_boxIdx(boxIdx)
  {
    if (m_region && m_boxIdx < m_region->m_boxes.size())
      m_voxel = m_region->m_boxes[m_boxIdx].minCorner();
  }

  template <class OtherValue>
  voxel_iter(voxel_iter<OtherValue> const& other, typename std::enable_if<
             std::is_convertible<OtherValue*,TVoxelRegion*>::value
             , enabler
             >::type = enabler()
      )
    : m_region(other.m_region), m_boxIdx(other.m_boxIdx), m_voxel(other.m_voxel)
  {}

private:
  friend class boost::iterator_core_access;
  template <class> friend class voxel_iter;

  template <class OtherValue>
  bool equal(voxel_iter<OtherValue> const& other) const
  {
    return this->m_region == other.m_region &&
        this->m_boxIdx == other.m_boxIdx &&
        this->m_voxel == other.m_voxel;
  }

  bool visited() const
  {
    for (size_t i=0; i<m_boxIdx; ++i) {
      if (m_region->m_boxes[i].contains(m_voxel))
        return true;
    }
    return false;
  }

  void increment()
  {
    if (!m_region || m_boxIdx == m_region->m_boxes.size()) {
      return;
    }

    do {
      if (++m_voxel[0] > m_region->m_boxes[m_boxIdx].maxCorner()[0]) {
        m_voxel[0] = m_region->m_boxes[m_boxIdx].minCorner()[0];
        if (++m_voxel[1] > m_region->m_boxes[m_boxIdx].maxCorner()[1]) {
          m_voxel[1] = m_region->m_boxes[m_boxIdx].minCorner()[1];
          if (++m_voxel[2] > m_region->m_boxes[m_boxIdx].maxCorner()[2]) {
            m_voxel[2] = m_region->m_boxes[m_boxIdx].minCorner()[2];
            if (++m_voxel[3] > m_region->m_boxes[m_boxIdx].maxCorner()[3]) {
              m_voxel[3] = m_region->m_boxes[m_boxIdx].minCorner()[3];
              if (++m_voxel[4] > m_region->m_boxes[m_boxIdx].maxCorner()[4]) {
                m_voxel[4] = m_region->m_boxes[m_boxIdx].minCorner()[4];
                if (++m_voxel[5] > m_region->m_boxes[m_boxIdx].maxCorner()[5]) {
                  m_boxIdx++;
                  // goto min corner of next box
                  if (m_boxIdx == m_region->m_boxes.size())
                    m_voxel.set(0,0,0,0,0);
                  else {
                    m_voxel = m_region->m_boxes[m_boxIdx].minCorner();
                  }
                }
              }
            }
          }
        }
      }
    } while (m_boxIdx < m_region->m_boxes.size() && visited());
  }

  ZVoxelCoordinate dereference() const
  {
    return m_voxel;
  }

  const TVoxelRegion* m_region;
  size_t m_boxIdx;
  ZVoxelCoordinate m_voxel;
};

} // namespace impl

// axis aligned region contains many voxel coordinates, can be seen as a voxel coordinates collection
// it is usually made of one or more multidimensional boxes
// this class provide iterator to go through all voxel coordinates within region
class ZVoxelRegion
{
public:
  //typedef impl::voxel_iter<ZVoxelRegion> iterator;
  typedef impl::voxel_iter<ZVoxelRegion const> const_iterator;

public:
  ZVoxelRegion();

  inline void addBox(const ZVoxelCoordinate& minCoord, const ZVoxelCoordinate& maxCoord) { addBox(BoxType(minCoord, maxCoord)); }

  inline void clear() { m_boxes.clear(); }

  inline bool isEmpty() const { return m_boxes.empty(); }
  void getBoundBox(ZVoxelCoordinate& minCoord, ZVoxelCoordinate& maxCoord) const;

  inline const_iterator begin() const { return const_iterator(this, 0); }
  inline const_iterator end() const { return const_iterator(this, m_boxes.size()); }

  bool containsVoxel(const ZVoxelCoordinate& v);

  ZVoxelRegion& unite(const ZVoxelRegion& other);
  static ZVoxelRegion unite(const ZVoxelRegion& r1, const ZVoxelRegion& r2);

  ZVoxelRegion& intersect(const ZVoxelRegion& other);
  static ZVoxelRegion intersect(const ZVoxelRegion& r1, const ZVoxelRegion& r2);

public:
  friend std::ostream& operator<< (std::ostream &s, const ZVoxelRegion &m);

protected:
  typedef ZBBox<ZVoxelCoordinate> BoxType;

  inline void addBox(const BoxType& box)
  {
    if (!containsBox(box)) {
      m_boxes.push_back(box);
    }
  }
  inline bool containsBox(const BoxType& box)
  {
    if (box.empty()) {
      return true;
    } else {
      for (size_t i=0; i<m_boxes.size(); ++i)
        if (m_boxes[i].contains(box))
          return true;
    }
    return false;
  }
  inline bool containsBox(const ZVoxelCoordinate& minCoord, const ZVoxelCoordinate& maxCoord)
  {
    return containsBox(BoxType(minCoord, maxCoord));
  }

private:
  template<typename T> friend class impl::voxel_iter;
  std::vector<BoxType> m_boxes;
};

std::ostream& operator << (std::ostream& s, const ZVoxelRegion& m);
QDebug operator << (QDebug s, const ZVoxelRegion& m);

} // namespace nim


#endif // ZVOXELREGION_H
