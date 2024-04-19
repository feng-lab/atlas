#pragma once

#include "zbbox.h"
#include "zvoxelcoordinate.h"
#include <boost/stl_interfaces/iterator_interface.hpp>
#include <type_traits>
#include <vector>

namespace nim {

namespace impl {

template<class TVoxelRegion>
class voxel_iter
  : public boost::stl_interfaces::iterator_interface<
#if !BOOST_STL_INTERFACES_USE_DEDUCED_THIS
      voxel_iter<TVoxelRegion>,
#endif
      std::forward_iterator_tag,
      ZVoxelCoordinate>
{
  using base_type = boost::stl_interfaces::iterator_interface<
#if !BOOST_STL_INTERFACES_USE_DEDUCED_THIS
    voxel_iter<TVoxelRegion>,
#endif
    std::forward_iterator_tag,
    ZVoxelCoordinate>;

public:
  constexpr voxel_iter() noexcept
    : m_region(nullptr)
    , m_boxIdx(0)
    , m_voxel()
  {}

  constexpr explicit voxel_iter(const TVoxelRegion* region, size_t boxIdx) noexcept
    : m_region(region)
    , m_boxIdx(boxIdx)
  {
    if (m_region && m_boxIdx < m_region->m_boxes.size()) {
      m_voxel = m_region->m_boxes[m_boxIdx].minCorner;
    }
  }

  template<typename OtherValue>
    requires std::is_convertible_v<OtherValue*, TVoxelRegion*>
  constexpr voxel_iter(const voxel_iter<OtherValue>& other) noexcept
    : m_region(other.m_region)
    , m_boxIdx(other.m_boxIdx)
    , m_voxel(other.m_voxel)
  {}

  template<class OtherValue>
  constexpr bool operator==(const voxel_iter<OtherValue>& other) const noexcept
  {
    return this->m_region == other.m_region && this->m_boxIdx == other.m_boxIdx && this->m_voxel == other.m_voxel;
  }

  [[nodiscard]] bool visited() const noexcept
  {
    for (size_t i = 0; i < m_boxIdx; ++i) {
      if (m_region->m_boxes[i].contains(m_voxel)) {
        return true;
      }
    }
    return false;
  }

  constexpr ZVoxelCoordinate operator*() const noexcept
  {
    return m_voxel;
  }

  constexpr voxel_iter& operator++() noexcept
  {
    if (!m_region || m_boxIdx == m_region->m_boxes.size()) {
      return *this;
    }

    do {
      if (++m_voxel[0] > m_region->m_boxes[m_boxIdx].maxCorner[0]) {
        m_voxel[0] = m_region->m_boxes[m_boxIdx].minCorner[0];
        if (++m_voxel[1] > m_region->m_boxes[m_boxIdx].maxCorner[1]) {
          m_voxel[1] = m_region->m_boxes[m_boxIdx].minCorner[1];
          if (++m_voxel[2] > m_region->m_boxes[m_boxIdx].maxCorner[2]) {
            m_voxel[2] = m_region->m_boxes[m_boxIdx].minCorner[2];
            if (++m_voxel[3] > m_region->m_boxes[m_boxIdx].maxCorner[3]) {
              m_voxel[3] = m_region->m_boxes[m_boxIdx].minCorner[3];
              if (++m_voxel[4] > m_region->m_boxes[m_boxIdx].maxCorner[4]) {
                m_voxel[4] = m_region->m_boxes[m_boxIdx].minCorner[4];
                if (++m_voxel[5] > m_region->m_boxes[m_boxIdx].maxCorner[5]) {
                  m_boxIdx++;
                  // goto min corner of next box
                  if (m_boxIdx == m_region->m_boxes.size()) {
                    m_voxel.set(0, 0, 0, 0, 0);
                  } else {
                    m_voxel = m_region->m_boxes[m_boxIdx].minCorner;
                  }
                }
              }
            }
          }
        }
      }
    } while (m_boxIdx < m_region->m_boxes.size() && visited());
    return *this;
  }
  using base_type::operator++;

private:
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
  // using iterator = impl::voxel_iter<ZVoxelRegion>;
  using const_iterator = impl::voxel_iter<ZVoxelRegion const>;

  inline void addBox(const ZVoxelCoordinate& minCoord, const ZVoxelCoordinate& maxCoord)
  {
    addBox(BoxType(minCoord, maxCoord));
  }

  inline void clear()
  {
    m_boxes.clear();
  }

  [[nodiscard]] inline bool isEmpty() const
  {
    return m_boxes.empty();
  }

  void getBoundBox(ZVoxelCoordinate& minCoord, ZVoxelCoordinate& maxCoord) const;

  [[nodiscard]] inline const_iterator begin() const
  {
    return const_iterator(this, 0);
  }

  [[nodiscard]] inline const_iterator end() const
  {
    return const_iterator(this, m_boxes.size());
  }

  bool containsVoxel(const ZVoxelCoordinate& v);

  ZVoxelRegion& unite(const ZVoxelRegion& other);

  static ZVoxelRegion unite(const ZVoxelRegion& r1, const ZVoxelRegion& r2);

  ZVoxelRegion& intersect(const ZVoxelRegion& other);

  static ZVoxelRegion intersect(const ZVoxelRegion& r1, const ZVoxelRegion& r2);

protected:
  using BoxType = ZBBox<ZVoxelCoordinate>;

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
    }
    for (const auto& mbox : m_boxes) {
      if (mbox.contains(box)) {
        return true;
      }
    }

    return false;
  }

  inline bool containsBox(const ZVoxelCoordinate& minCoord, const ZVoxelCoordinate& maxCoord)
  {
    return containsBox(BoxType(minCoord, maxCoord));
  }

private:
  friend void tag_invoke(const json::value_from_tag&, json::value& jv, const ZVoxelRegion& vr);

  friend ZVoxelRegion tag_invoke(const json::value_to_tag<ZVoxelRegion>&, const json::value& jv);

  template<typename T>
  friend class impl::voxel_iter;

  std::vector<BoxType> m_boxes;
};

inline void tag_invoke(const json::value_from_tag&, json::value& jv, const ZVoxelRegion& vr)
{
  auto& ja = jv.emplace_array();
  ja.reserve(vr.m_boxes.size());
  for (const auto& box : vr.m_boxes) {
    ja.push_back(json::value_from(box));
  }
}

inline ZVoxelRegion tag_invoke(const json::value_to_tag<ZVoxelRegion>&, const json::value& jv)
{
  ZVoxelRegion res;
  const auto& ja = jv.as_array();
  res.m_boxes.reserve(ja.size());
  for (const auto& v : ja) {
    res.m_boxes.push_back(json::value_to<ZVoxelRegion::BoxType>(v));
  }
  return res;
}

} // namespace nim
