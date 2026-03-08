#include "zswcgeometrymaskvolume.h"

#include "zlog.h"

#include <cmath>

namespace nim {

ZSwcGeometryMaskVolume::ZSwcGeometryMaskVolume(std::shared_ptr<ZSwcSpatialIndex> index,
                                               size_t w,
                                               size_t h,
                                               size_t d,
                                               double zScale)
  : ZSwcGeometryMaskVolume(std::move(index), w, h, d, zScale, glm::dvec3{0.0, 0.0, 0.0})
{}

ZSwcGeometryMaskVolume::ZSwcGeometryMaskVolume(std::shared_ptr<ZSwcSpatialIndex> index,
                                               size_t w,
                                               size_t h,
                                               size_t d,
                                               double zScale,
                                               glm::dvec3 origin)
  : m_index(std::move(index))
  , m_width(w)
  , m_height(h)
  , m_depth(d)
  , m_zScale(zScale)
  , m_origin(origin)
{
  CHECK(m_index != nullptr);
  CHECK(std::isfinite(m_zScale));
  CHECK(m_zScale > 0.0);
}

std::shared_ptr<ZSwcSpatialIndex> ZSwcGeometryMaskVolume::sharedIndex() const
{
  CHECK(m_index != nullptr);
  return m_index;
}

bool ZSwcGeometryMaskVolume::isEmpty() const
{
  return m_width == 0 || m_height == 0 || m_depth == 0 || m_index == nullptr || m_index->empty();
}

size_t ZSwcGeometryMaskVolume::width() const
{
  return m_width;
}

size_t ZSwcGeometryMaskVolume::height() const
{
  return m_height;
}

size_t ZSwcGeometryMaskVolume::depth() const
{
  return m_depth;
}

double ZSwcGeometryMaskVolume::voxelSizeX() const
{
  return 1.0;
}

double ZSwcGeometryMaskVolume::voxelSizeY() const
{
  return 1.0;
}

double ZSwcGeometryMaskVolume::voxelSizeZ() const
{
  return 1.0;
}

ZVoxelValueType ZSwcGeometryMaskVolume::valueType() const
{
  return ZVoxelValueType::Uint8;
}

double ZSwcGeometryMaskVolume::valueAsDouble(int x, int y, int z) const
{
  if (isEmpty()) {
    return 0.0;
  }

  if (x < 0 || y < 0 || z < 0) {
    return 0.0;
  }
  const size_t ux = static_cast<size_t>(x);
  const size_t uy = static_cast<size_t>(y);
  const size_t uz = static_cast<size_t>(z);
  if (ux >= m_width || uy >= m_height || uz >= m_depth) {
    return 0.0;
  }

  return m_index->containsPoint(static_cast<double>(x) + m_origin.x,
                                static_cast<double>(y) + m_origin.y,
                                static_cast<double>(z) / m_zScale + m_origin.z)
           ? 1.0
           : 0.0;
}

void ZSwcGeometryMaskVolume::setValueU8(int /*x*/, int /*y*/, int /*z*/, std::uint8_t /*value*/)
{
  CHECK(false) << "ZSwcGeometryMaskVolume is read-only";
}

void ZSwcGeometryMaskVolume::clearU8(std::uint8_t /*value*/)
{
  CHECK(false) << "ZSwcGeometryMaskVolume is read-only";
}

} // namespace nim
