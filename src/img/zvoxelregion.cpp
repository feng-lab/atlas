#include "zvoxelregion.h"

namespace nim {

void ZVoxelRegion::getBoundBox(ZVoxelCoordinate& minCoord, ZVoxelCoordinate& maxCoord) const
{
  BoxType box;
  for (const auto& mbox : m_boxes) {
    box.expand(mbox);
  }

  minCoord = box.minCorner;
  maxCoord = box.maxCorner;
}

bool ZVoxelRegion::containsVoxel(const ZVoxelCoordinate& v)
{
  for (auto& box : m_boxes) {
    if (box.contains(v)) {
      return true;
    }
  }
  return false;
}

ZVoxelRegion& ZVoxelRegion::unite(const ZVoxelRegion& other)
{
  for (const auto& box : other.m_boxes) {
    addBox(box);
  }
  return *this;
}

ZVoxelRegion ZVoxelRegion::unite(const ZVoxelRegion& r1, const ZVoxelRegion& r2)
{
  ZVoxelRegion res = r1;
  res.unite(r2);
  return res;
}

ZVoxelRegion& ZVoxelRegion::intersect(const ZVoxelRegion& other)
{
  std::vector<BoxType> currentBoxes;
  currentBoxes.swap(m_boxes);
  for (const auto& box : other.m_boxes) {
    for (auto& currentBox : currentBoxes) {
      addBox(nim::intersect(box, currentBox));
    }
  }
  return *this;
}

ZVoxelRegion ZVoxelRegion::intersect(const ZVoxelRegion& r1, const ZVoxelRegion& r2)
{
  ZVoxelRegion res = r1;
  res.intersect(r2);
  return res;
}

} // namespace nim
