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
  explicit ZImgTile(const ZImgSubBlock& img, const ZVoxelCoordinate& loc = ZVoxelCoordinate())
    : m_img(img)
  {
    m_info = img.readInfo();
    if (m_info.isEmpty()) throw ZImgException("No max coord for empty img");
    ZVoxelCoordinate maxCoord(m_info.width - 1, m_info.height - 1, m_info.depth - 1, m_info.numChannels - 1,
                              m_info.numTimes - 1);
    m_box = BoxType(loc, loc + maxCoord);
  }

  void createImgCache()
  { if (!m_imgCache) m_imgCache = m_img.read(); }

  void clearImgCache()
  { m_imgCache.reset(); }

  inline const ZImgSubBlock& imgBlock() const
  { return m_img; }

  inline const ZImg& img() const
  { return *m_imgCache; }

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
    return *(m_imgCache->data<TVoxel>(v - m_box.minCorner()));
  }

private:
  const ZImgSubBlock& m_img;

  using BoxType = ZBBox<ZVoxelCoordinate>;
  BoxType m_box;

  ZImgInfo m_info;
  std::shared_ptr<ZImg> m_imgCache;
};

} // namespace nim


