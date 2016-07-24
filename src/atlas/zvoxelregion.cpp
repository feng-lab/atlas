#include "zvoxelregion.h"

namespace nim {

ZVoxelRegion::ZVoxelRegion()
{
}

void ZVoxelRegion::getBoundBox(ZVoxelCoordinate &minCoord, ZVoxelCoordinate &maxCoord) const
{
  BoxType box;
  for (size_t i=0; i<m_boxes.size(); ++i) {
    box.expand(m_boxes[i]);
  }

  minCoord = box.minCorner();
  maxCoord = box.maxCorner();
}

bool ZVoxelRegion::containsVoxel(const ZVoxelCoordinate &v)
{
  for (size_t i=0; i<m_boxes.size(); ++i)
    if (m_boxes[i].contains(v))
      return true;
  return false;
}

ZVoxelRegion &ZVoxelRegion::unite(const ZVoxelRegion &other)
{
  for (size_t i=0; i<other.m_boxes.size(); ++i)
    addBox(other.m_boxes[i]);
  return *this;
}

ZVoxelRegion ZVoxelRegion::unite(const ZVoxelRegion &r1, const ZVoxelRegion &r2)
{
  ZVoxelRegion res = r1;
  res.unite(r2);
  return res;
}

ZVoxelRegion &ZVoxelRegion::intersect(const ZVoxelRegion &other)
{
  std::vector<BoxType> currentBoxes;
  currentBoxes.swap(m_boxes);
  for (size_t i=0; i<other.m_boxes.size(); ++i)
    for (size_t j=0; j<currentBoxes.size(); ++j) {
      addBox(nim::intersect(other.m_boxes[i], currentBoxes[j]));
    }
  return *this;
}

ZVoxelRegion ZVoxelRegion::intersect(const ZVoxelRegion &r1, const ZVoxelRegion &r2)
{
  ZVoxelRegion res = r1;
  res.intersect(r2);
  return res;
}

std::ostream& operator << (std::ostream& s, const ZVoxelRegion& m)
{
  for (size_t i=0; i<m.m_boxes.size(); ++i) {
    s << "Box " << i << ": " << m << std::endl;
  }
  return s;
}

#ifdef _USE_QSLOG_
QDebug operator << (QDebug s, const ZVoxelRegion& m)
{
  std::ostringstream oss;
  oss << m;
  s.nospace() << oss.str().c_str();
  return s.space();
}
#endif


} // namespace nim
