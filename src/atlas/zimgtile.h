#pragma once

#include "zimg.h"
#include "zbbox.h"
#include "zvoxelcoordinate.h"
#include <sstream>
#include <vector>

namespace nim {

// a ZImg with its spatial location
class ZImgTile
{
public:
  ZImgTile(const ZImg* img, const ZVoxelCoordinate& loc = ZVoxelCoordinate())
    : m_img(img)
  {
    setLocation(loc);
  }

  inline void setLocation(const ZVoxelCoordinate& loc)
  {
    m_box = BoxType(loc, loc + m_img->maxCoord());
  }

  inline const ZImg& img() const
  { return *m_img; }

  inline const ZVoxelCoordinate& location() const
  { return m_box.minCorner(); }

  inline const ZVoxelCoordinate& maxCoord() const
  { return m_box.maxCorner(); }

  inline bool contains(const ZVoxelCoordinate& v) const
  { return m_box.contains(v); }

  // this function does not check whether coordinate or type of voxel is valid
  template<typename TVoxel>
  inline TVoxel value(const ZVoxelCoordinate& v) const
  {
    return *(m_img->data<TVoxel>(v - m_box.minCorner()));
  }

private:
  const ZImg* m_img;

  typedef ZBBox<ZVoxelCoordinate> BoxType;
  BoxType m_box;
};

} // namespace nim


