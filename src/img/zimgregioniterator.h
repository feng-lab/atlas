#pragma once

#include "zimg.h"
#include <boost/iterator/iterator_facade.hpp>

namespace nim {

namespace impl {

template<typename TImg, typename TVoxel>
class img_region_iter
  : public boost::iterator_facade<
    img_region_iter<TImg, TVoxel>, TVoxel, boost::random_access_traversal_tag, TVoxel&, int64_t
  >
{
  struct enabler
  {
  };
public:
  // empty constructor not useful
  img_region_iter() = default;

  explicit img_region_iter(TImg& img, const ZImgRegion& region = ZImgRegion())
    : m_img(&img), m_region(region), m_idx(0), m_centerVoxelPtr(nullptr)
  {
    if (!m_img->template isType<TVoxel>()) {
      throw ZImgException(QString("Iterator type doesn't match image type <%1>").arg(m_img->info().toQString()));
    }
    if (m_img->isEmpty()) {
      m_endIdx = -1;
    } else if (!m_region.isValid(m_img->info())) {
      throw ZImgException(
        QString("Can not construct iterator over invalid image region. Image info: '%1', region: '%2'")
          .arg(m_img->info().toQString()).arg(m_region.toQString()));
    } else if (m_region.isEmpty()) {
      m_endIdx = -1;
    } else {
      // init internal data
      m_region.resolveRegionEnd(m_img->info());
      m_regionInfo = m_region.clip(m_img->info());
      m_coord = m_region.start;
      m_endIdx = m_regionInfo.voxelNumber();

      m_centerVoxelPtr = m_img->template data<TVoxel>(m_coord);
    }

    // only need first 3
    m_wrapOffsets.resize(3);
    for (size_t i = 0; i < 3; ++i)
      m_wrapOffsets[i] = m_img->info().stride(i) * (m_img->info()[i] - m_regionInfo[i]);
  }

  template<typename OtherTImg, typename OtherTVoxel>
  img_region_iter(img_region_iter<OtherTImg, OtherTVoxel> const& other, typename std::enable_if<
    std::is_convertible<OtherTImg*, TImg*>::value && std::is_convertible<OtherTVoxel*, TVoxel*>::value, enabler
  >::type = enabler()
  )
    : m_img(other.m_img), m_region(other.m_region), m_regionInfo(other.m_regionInfo), m_endIdx(other.m_endIdx), m_idx(
    other.m_idx), m_coord(other.m_coord), m_centerVoxelPtr(other.m_centerVoxelPtr), m_wrapOffsets(other.m_wrapOffsets)
  {}

  // return true if before first voxel
  __forceinline bool isBeforeBegin() const
  { return m_idx < 0; }
  // return true if at first voxel
  __forceinline bool isAtBegin() const
  { return m_idx == 0; }
  // return true if past last voxel, similar to stl "== container.end()"
  __forceinline bool isAtEnd() const
  { return m_idx >= m_endIdx; }

  // go to first voxel
  __forceinline void goToBegin()
  {
    if (m_endIdx > 0) {
      m_coord = m_region.start;
      m_idx = 0;
      m_centerVoxelPtr = m_img->template data<TVoxel>(m_coord);
    }
  }

  // go to last voxel
  __forceinline void goToLast()
  {
    if (m_endIdx > 0) {
      m_coord = m_region.end - 1;
      m_idx = m_endIdx - 1;
      m_centerVoxelPtr = m_img->template data<TVoxel>(m_coord);
    }
  }

  // go to one past last voxel
  __forceinline void goToEnd()
  {
    m_coord = m_region.start;
    m_coord.t = m_region.end.t;
    m_idx = m_endIdx;
  }

  // go to idx of this region
  __forceinline void goToIndex(int64_t idx)
  {
    m_idx = idx;
    m_coord = ZImg::indexToCoord(idx, m_regionInfo);
    if (m_idx >= 0 && m_idx < m_endIdx) {
      m_centerVoxelPtr = m_img->template data<TVoxel>(m_coord);
    }
  }

  // return voxel coord of this iterator
  __forceinline ZVoxelCoordinate coord() const
  { return m_coord; }

  // return index of current voxel of this region
  // negative index means before the region
  __forceinline int64_t index() const
  { return m_idx; }

private:
  friend class boost::iterator_core_access;

  template<typename, typename> friend
  class img_region_iter;

  template<typename OtherTImg, typename OtherTVoxel>
  __forceinline bool equal(img_region_iter<OtherTImg, OtherTVoxel> const& other) const
  {
    return this->m_img == other.m_img &&
           this->m_region == other.m_region &&
           this->m_regionInfo == other.m_regionInfo &&
           this->m_endIdx == other.m_endIdx &&
           this->m_idx == other.m_idx &&
           this->m_coord == other.m_coord &&
           this->m_wrapOffsets == other.m_wrapOffsets;
  }

  __forceinline void increment()
  {
    ++m_idx;
    ++m_centerVoxelPtr;
    if (++m_coord.x >= m_region.end.x) { // hit row bound
      // find next coord
      m_coord.x = m_region.start.x;
      m_centerVoxelPtr += m_wrapOffsets[0];
      if (++m_coord.y >= m_region.end.y) {
        m_coord.y = m_region.start.y;
        m_centerVoxelPtr += m_wrapOffsets[1];
        if (++m_coord.z >= m_region.end.z) {
          m_coord.z = m_region.start.z;
          m_centerVoxelPtr += m_wrapOffsets[2];
          if (++m_coord.c >= m_region.end.c) {
            m_coord.c = m_region.start.c;
            ++m_coord.t;
            // img memory is not contiguous, we have jumped to another chunk of memory
            // recalculate voxel pointer
            if (m_idx < m_endIdx && m_idx >= 0) {
              m_centerVoxelPtr = m_img->template data<TVoxel>(m_coord);
            }
          }
        }
      }
    }
  }

  __forceinline void decrement()
  {
    --m_idx;
    --m_centerVoxelPtr;
    if (--m_coord.x < m_region.start.x) { // hit row bound
      // find previous coord
      m_coord.x = m_region.end.x - 1;
      m_centerVoxelPtr -= m_wrapOffsets[0];
      if (--m_coord.y < m_region.start.y) {
        m_coord.y = m_region.end.y - 1;
        m_centerVoxelPtr -= m_wrapOffsets[1];
        if (--m_coord.z < m_region.start.z) {
          m_coord.z = m_region.end.z - 1;
          m_centerVoxelPtr -= m_wrapOffsets[2];
          if (--m_coord.c < m_region.start.c) {
            m_coord.c = m_region.end.c - 1;
            --m_coord.t;
            // img memory is not contiguous, we have jumped to another chunk of memory
            // recalculate voxel pointer
            if (m_idx >= 0 && m_idx < m_endIdx) {
              m_centerVoxelPtr = m_img->template data<TVoxel>(m_coord);
            }
          }
        }
      }
    }
  }

  __forceinline TVoxel& dereference() const
  {
    return *m_centerVoxelPtr;
  }

  __forceinline void advance(int64_t n)
  {
    m_idx += n;
    ZVoxelCoordinate coordAdvance = ZImg::indexToCoord(n, m_regionInfo);
    ZVoxelCoordinate::value_type carry = 0;
    m_coord -= m_region.start;
    for (size_t i = 0; i < m_coord.size() - 1; ++i) {
      m_coord[i] += coordAdvance[i];
      m_coord[i] += carry;
      if (m_coord[i] >= static_cast<ZVoxelCoordinate::value_type>(m_regionInfo.size(i))) {
        carry = 1;
        m_coord[i] -= m_regionInfo.size(i);
      } else {
        carry = 0;
      }
    }
    m_coord.t += coordAdvance.t;
    m_coord.t += carry;
    m_coord += m_region.start;

    if (m_idx >= 0 && m_idx < m_endIdx) {
      m_centerVoxelPtr = m_img->template data<TVoxel>(m_coord);
    }
  }

  template<typename OtherTImg, typename OtherTVoxel>
  __forceinline int64_t distance_to(img_region_iter<OtherTImg, OtherTVoxel> const& other) const
  {
    return other.m_idx - m_idx;
  }

private:
  TImg* m_img;
  ZImgRegion m_region;
  ZImgInfo m_regionInfo;
  int64_t m_endIdx;
  // dynamic info of current voxel
  int64_t m_idx;  // current voxel idx of region
  ZVoxelCoordinate m_coord; // current voxel coord of img

  TVoxel* m_centerVoxelPtr; // current voxel address

  // img info fixed
  std::vector<std::ptrdiff_t> m_wrapOffsets;
};

} // namespace impl

// random_access_iterator that walk throught a sub region of img of type TVoxel, similar to itk iterator
// if input img is not type TVoxel, a ZImgException will be thrown.
// if input region is an invalid region (can not fit into img), a ZImgException will be thrown

// for (ZImgRegionIterator<uint8_t> it = ZImgRegionIterator<uint8_t>(img, region);
//      !it.isAtEnd(); it += 2) {
//   *it = 0;
// }

// to walk throught the whole img, use ZImg::unaryOperation instead which should be clearer and faster than this

template<typename TVoxel>
using ZImgRegionConstIterator = impl::img_region_iter<const ZImg, const TVoxel>;

template<typename TVoxel>
using ZImgRegionIterator = impl::img_region_iter<ZImg, TVoxel>;

} // namespace nim

