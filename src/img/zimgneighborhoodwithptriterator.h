#pragma once

#include "zimg.h"
#include "zneighborhood.h"
#include <boost/stl_interfaces/iterator_interface.hpp>

namespace nim {

namespace impl {

template<typename TImg, typename TVoxel>
class img_neighborhood_with_ptr_iter
  : public boost::stl_interfaces::iterator_interface<img_neighborhood_with_ptr_iter<TImg, TVoxel>,
  std::random_access_iterator_tag, TVoxel>
{
  using base_type = boost::stl_interfaces::iterator_interface<img_neighborhood_with_ptr_iter<TImg, TVoxel>,
    std::random_access_iterator_tag, TVoxel>;

  template<typename, typename> friend
  class img_neighborhood_with_ptr_iter;
public:
  // empty constructor not useful
  constexpr img_neighborhood_with_ptr_iter() noexcept = default;

  explicit img_neighborhood_with_ptr_iter(const ZNeighborhood& nb, TImg& img, const ZImgRegion& region = ZImgRegion(),
                                          PadOption padOption = PadOption::Constant, TVoxel padValue = TVoxel(0))
    : m_neighborhood(nb), m_img(&img), m_region(region), m_idx(0)
    , m_padOption(padOption), m_padValue(padValue), m_centerVoxelPtr(nullptr)
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

      initNbInfo();
    }

    // only need first 3
    m_wrapOffsets.resize(3);
    for (size_t i = 0; i < 3; ++i)
      m_wrapOffsets[i] = m_img->info().stride(i) * (m_img->info()[i] - m_regionInfo[i]);
  }

  template<
    typename OtherTImg,
    typename OtherTVoxel,
    typename E = std::enable_if_t<
      std::is_convertible_v<OtherTImg*, TImg*> && std::is_convertible_v<OtherTVoxel*, TVoxel*>>>
  constexpr img_neighborhood_with_ptr_iter(const img_neighborhood_with_ptr_iter<OtherTImg, OtherTVoxel>& other) noexcept
    : m_img(other.m_img), m_region(other.m_region), m_regionInfo(other.m_regionInfo)
    , m_endIdx(other.m_endIdx), m_idx(other.m_idx), m_coord(other.m_coord)
    , m_padOption(other.m_padOption), m_padValue(other.m_padValue)
    , m_neighborhood(other.m_neighborhood)
    , m_nbIndexOffsets(other.m_nbIndexOffsets)
    , m_innerBoundLow(other.m_innerBoundLow), m_innerBoundHigh(other.m_innerBoundHigh)
    , m_allNbInBound(other.m_allNbInBound), m_nbCoords(other.m_nbCoords), m_nbCoordsIsValid(other.m_nbCoordsIsValid)
    , m_isNbInBound(other.m_isNbInBound), m_nbValues(other.m_nbValues)
    , m_centerVoxelPtr(other.m_centerVoxelPtr), m_wrapOffsets(other.m_wrapOffsets)
    , m_nbPtrOffsets(other.m_nbPtrOffsets)
  {}

  void setNeighborhood(const ZNeighborhood& nb)
  {
    m_neighborhood = nb;
    if (m_endIdx > 0)
      initNbInfo();
  }

  const ZNeighborhood& neighborhood() const
  { return m_neighborhood; }

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
      updateNbInfoOfCurrentVoxel();
      m_centerVoxelPtr = m_img->template data<TVoxel>(m_coord);
    }
  }

  // go to last voxel
  __forceinline void goToLast()
  {
    if (m_endIdx > 0) {
      m_coord = m_region.end - 1;
      m_idx = m_endIdx - 1;
      updateNbInfoOfCurrentVoxel();
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
    updateNbInfoOfCurrentVoxel();
    m_centerVoxelPtr = m_img->template data<TVoxel>(m_coord);
  }

  // return center voxel coord of this iterator
  __forceinline ZVoxelCoordinate coord() const
  { return m_coord; }

  // return index of current voxel of this region
  // negative index means before the region
  __forceinline int64_t index() const
  { return m_idx; }

  // access nb info
  __forceinline size_t numNeighbors() const
  { return m_neighborhood.size(); }

  __forceinline bool isInBound(size_t n) const
  { return m_allNbInBound || m_isNbInBound[n]; }
  // returned index is only meanlingful when that neighbor is within region since the index is idx of voxel in region
  __forceinline int64_t index(size_t n) const
  { return m_idx + m_nbIndexOffsets[n]; }

  __forceinline ZVoxelCoordinate coord(size_t n)
  {
    if (!m_nbCoordsIsValid)
      updateNbCoords();
    return m_nbCoords[n];
  }

  __forceinline TVoxel* valuePtr(size_t n)
  {
    if (m_allNbInBound || m_isNbInBound[n])
      return m_centerVoxelPtr + m_nbPtrOffsets[n];
    else
      return &m_nbValues[n];
  }

  __forceinline TVoxel& valueRef(size_t n)
  {
    if (m_allNbInBound || m_isNbInBound[n])
      return *(m_centerVoxelPtr + m_nbPtrOffsets[n]);
    else
      return m_nbValues[n];
  }

  template<typename OtherTImg, typename OtherTVoxel>
  __forceinline constexpr bool operator==(img_neighborhood_with_ptr_iter<OtherTImg, OtherTVoxel> const& other) const noexcept
  {
    return this->m_img == other.m_img &&
           this->m_region == other.m_region &&
           this->m_regionInfo == other.m_regionInfo &&
           this->m_endIdx == other.m_endIdx &&
           this->m_idx == other.m_idx &&
           this->m_coord == other.m_coord &&
           this->m_padOption == other.m_padOption &&
           this->m_padValue == other.m_padValue &&
           this->m_neighborhood == other.m_neighborhood &&
           this->m_nbIndexOffsets == other.m_nbIndexOffsets &&
           this->m_innerBoundLow == other.m_innerBoundLow &&
           this->m_innerBoundHigh == other.m_innerBoundHigh &&
           this->m_allNbInBound == other.m_allNbInBound &&
           this->m_centerVoxelPtr == other.m_centerVoxelPtr &&
           this->m_wrapOffsets == other.m_wrapOffsets &&
           this->m_nbPtrOffsets == other.m_nbPtrOffsets;
  }

  __forceinline constexpr img_neighborhood_with_ptr_iter& operator++() noexcept
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
              updateNbInfoOfCurrentVoxel();
            }
            return *this;
          }
        }
      }
    }
    updateNbInfoOfCurrentVoxel();
    return *this;
  }
  using base_type::operator++;

  __forceinline constexpr img_neighborhood_with_ptr_iter& operator--() noexcept
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
              updateNbInfoOfCurrentVoxel();
            }
            return *this;
          }
        }
      }
    }
    updateNbInfoOfCurrentVoxel();
    return *this;
  }
  using base_type::operator--;

  __forceinline TVoxel& operator*() const noexcept
  {
    return *m_centerVoxelPtr;
  }

  __forceinline constexpr img_neighborhood_with_ptr_iter& operator+=(std::ptrdiff_t n) noexcept
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
      updateNbInfoOfCurrentVoxel();
    }
    return *this;
  }

  template<typename OtherTImg, typename OtherTVoxel>
  __forceinline auto operator-(const img_neighborhood_with_ptr_iter<OtherTImg, OtherTVoxel>& other) const noexcept
  {
    return m_idx - other.m_idx;
  }

  void initNbInfo()
  {
    m_nbCoordsIsValid = false;

    if (m_neighborhood.empty()) {
      m_nbPtrOffsets.clear();
      m_nbIndexOffsets.clear();
      m_nbCoords.clear();
      m_isNbInBound.clear();
      m_nbValues.clear();
      return;
    }

    m_nbPtrOffsets.resize(this->m_neighborhood.size());
    m_nbIndexOffsets.resize(m_neighborhood.size());
    m_nbCoords.resize(m_neighborhood.size());
    m_isNbInBound.resize(m_neighborhood.size());
    m_nbValues.resize(m_neighborhood.size());
    for (size_t i = 0; i < m_neighborhood.size(); ++i) {
      m_nbIndexOffsets[i] = m_neighborhood[i].x +
                            m_neighborhood[i].y * m_regionInfo.stride(1) +
                            m_neighborhood[i].z * m_regionInfo.stride(2);
      m_nbPtrOffsets[i] = m_neighborhood[i].x +
                          m_neighborhood[i].y * m_img->info().stride(1) +
                          m_neighborhood[i].z * m_img->info().stride(2);
    }

    // bound
    m_innerBoundLow = ZVoxelCoordinate(m_neighborhood.leftExtend(), m_neighborhood.upExtend(),
                                       m_neighborhood.frontExtend());
    m_innerBoundHigh = m_img->maxCoord() -
                       ZVoxelCoordinate(m_neighborhood.rightExtend(), m_neighborhood.downExtend(),
                                        m_neighborhood.backExtend());

    updateNbInfoOfCurrentVoxel();
  }

  __forceinline void updateNbInfoOfCurrentVoxel()
  {
    m_nbCoordsIsValid = false;
    // update nb info of current voxel
    if (m_coord.x >= m_innerBoundLow.x && m_coord.y >= m_innerBoundLow.y && m_coord.z >= m_innerBoundLow.z &&
        m_coord.x <= m_innerBoundHigh.x && m_coord.y <= m_innerBoundHigh.y && m_coord.z <= m_innerBoundHigh.z) {
      m_allNbInBound = true;
    } else {
      m_allNbInBound = false;
      for (size_t i = 0; i < m_nbCoords.size(); ++i) {
        m_nbCoords[i] = m_coord + m_neighborhood[i];
        if (m_nbCoords[i].x >= 0 && m_nbCoords[i].y >= 0 && m_nbCoords[i].z >= 0 &&
            m_nbCoords[i].x < static_cast<int>(m_img->width()) &&
            m_nbCoords[i].y < static_cast<int>(m_img->height()) &&
            m_nbCoords[i].z < static_cast<int>(m_img->depth())) {
          m_isNbInBound[i] = true;
        } else {
          m_isNbInBound[i] = false;
          m_nbValues[i] = m_img->outBoundValue(m_nbCoords[i], m_padOption, m_padValue);
        }
      }
      m_nbCoordsIsValid = true;
    }
  }

  __forceinline void updateNbCoords()
  {
    for (size_t i = 0; i < m_nbCoords.size(); ++i) {
      m_nbCoords[i] = m_coord + m_neighborhood[i];
    }
    m_nbCoordsIsValid = true;
  }

protected:
  ZNeighborhood m_neighborhood;
  TImg* m_img;
  ZImgRegion m_region;
  ZImgInfo m_regionInfo;
  int64_t m_endIdx;
  // dynamic info of current voxel
  int64_t m_idx;  // current voxel idx of region
  ZVoxelCoordinate m_coord; // current voxel coord of img

  // img info fixed
  PadOption m_padOption;  // how to get out of bound voxel value
  TVoxel m_padValue;

  // neighborhood info fixed
  std::vector<int64_t> m_nbIndexOffsets;
  ZVoxelCoordinate m_innerBoundLow;
  ZVoxelCoordinate m_innerBoundHigh;

  // dynamic info of nbs
  bool m_allNbInBound;
  std::vector<ZVoxelCoordinate> m_nbCoords;
  bool m_nbCoordsIsValid;
  std::vector<bool> m_isNbInBound;
  std::vector<typename std::remove_cv<TVoxel>::type> m_nbValues; // out of bound data are stored here so we can provide a reference when needed

  TVoxel* m_centerVoxelPtr; // current voxel address

  // img info fixed
  std::vector<std::ptrdiff_t> m_wrapOffsets;
  std::vector<std::ptrdiff_t> m_nbPtrOffsets;
};

} // namespace impl

// similar to ZImgNeighborhoodIterator, but also update a pointer to center voxel when moving iterator which
// makes dereference and neighbor value access faster

template<typename TVoxel>
using ZImgNeighborhoodWithPtrConstIterator = impl::img_neighborhood_with_ptr_iter<const ZImg, const TVoxel>;

template<typename TVoxel>
using ZImgNeighborhoodWithPtrIterator = impl::img_neighborhood_with_ptr_iter<ZImg, TVoxel>;

} // namespace nim

