#pragma once

#include "zglmutils.h"
#include "zswcspatialindex.h"
#include "zvoxelvolume.h"

#include <memory>

namespace nim {

// Read-only ZVoxelVolume adapter backed by a continuous-geometry SWC spatial index.
//
// This lets existing tracing helpers (point sampling, hit-mask tests, trace workspace mask accessors)
// use SWC geometry as a mask without allocating a dense/sparse voxel label image.
//
// Coordinate contract:
// - The underlying `ZSwcSpatialIndex` operates in image space.
// - This adapter exposes the legacy trace-mask interface, where callers query Z in
//   z-scaled mask coordinates.
// - `origin` is therefore expressed in image-space coordinates, while queried local
//   `z` values are converted from mask space back to image space before hit testing.
class ZSwcGeometryMaskVolume final : public ZVoxelMaskMutable
{
public:
  ZSwcGeometryMaskVolume(std::shared_ptr<ZSwcSpatialIndex> index, size_t w, size_t h, size_t d, double zScale);
  ZSwcGeometryMaskVolume(std::shared_ptr<ZSwcSpatialIndex> index,
                         size_t w,
                         size_t h,
                         size_t d,
                         double zScale,
                         glm::dvec3 origin);

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
  double m_zScale = 1.0;
  glm::dvec3 m_origin{0.0, 0.0, 0.0};
};

} // namespace nim
