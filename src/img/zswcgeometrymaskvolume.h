#pragma once

#include "zglmutils.h"
#include "zswcspatialindex.h"
#include "zvoxelvolume.h"

#include <memory>

namespace nim {

enum class ZSwcGeometryMaskQuerySpace
{
  ImageSpace,
  LegacyScaledMaskSpace,
};

// Read-only ZVoxelVolume adapter backed by a continuous-geometry SWC spatial index.
//
// This lets existing tracing helpers (point sampling, hit-mask tests, trace workspace mask accessors)
// use SWC geometry as a mask without allocating a dense/sparse voxel label image.
//
// Coordinate contract:
// - The underlying `ZSwcSpatialIndex` operates in image space.
// - `origin` is always expressed in image-space coordinates.
// - `querySpace` controls whether incoming voxel queries are already in image space or still
//   in the older legacy "mask-space" convention where Z is multiplied by `zToXYRatio`.
class ZSwcGeometryMaskVolume final : public ZVoxelMaskMutable
{
public:
  ZSwcGeometryMaskVolume(std::shared_ptr<ZSwcSpatialIndex> index,
                         size_t w,
                         size_t h,
                         size_t d,
                         double zToXYRatio,
                         ZSwcGeometryMaskQuerySpace querySpace = ZSwcGeometryMaskQuerySpace::ImageSpace);
  ZSwcGeometryMaskVolume(std::shared_ptr<ZSwcSpatialIndex> index,
                         size_t w,
                         size_t h,
                         size_t d,
                         double zToXYRatio,
                         glm::dvec3 origin,
                         ZSwcGeometryMaskQuerySpace querySpace = ZSwcGeometryMaskQuerySpace::ImageSpace);

  [[nodiscard]] std::shared_ptr<ZSwcSpatialIndex> sharedIndex() const;

  [[nodiscard]] bool isEmpty() const override;

  [[nodiscard]] size_t width() const override;
  [[nodiscard]] size_t height() const override;
  [[nodiscard]] size_t depth() const override;

  [[nodiscard]] double voxelSizeX() const override;
  [[nodiscard]] double voxelSizeY() const override;
  [[nodiscard]] double voxelSizeZ() const override;

  [[nodiscard]] ZVoxelValueType valueType() const override;

  [[nodiscard]] double valueAsDouble(int x, int y, int z) const override;

  void setValueU8(int x, int y, int z, std::uint8_t value) override;
  void clearU8(std::uint8_t value) override;

private:
  std::shared_ptr<ZSwcSpatialIndex> m_index;
  size_t m_width = 0;
  size_t m_height = 0;
  size_t m_depth = 0;
  double m_zToXYRatio = 1.0;
  glm::dvec3 m_origin{0.0, 0.0, 0.0};
  ZSwcGeometryMaskQuerySpace m_querySpace = ZSwcGeometryMaskQuerySpace::ImageSpace;
};

} // namespace nim
