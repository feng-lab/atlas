#pragma once

#include "zglmutils.h"
#include "zswc.h"

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/point.hpp>
#include <boost/geometry/index/rtree.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace nim {

// Continuous-geometry spatial index over an SWC skeleton.
//
// Design goals:
// - Answer "is point inside traced SWC geometry?" without allocating a global voxel mask.
// - Be fast enough for hot-loop mask queries during tracing and seed filtering.
//
// Coordinate contract:
// - SWC node coordinates stay in image space (the same x/y/z stored in the SWC).
// - `containsPoint*()` therefore accepts image-space query coordinates too.
// - `zScale` is applied only internally as the anisotropic hit-test metric.
// - Callers that need the legacy z-scaled trace-mask interface should go through
//   `ZSwcGeometryMaskVolume`, which adapts mask-space z back into image-space queries.
//
// Geometry model (continuous):
// - Each SWC edge (parent->child) is treated as a line segment with linearly interpolated radius (tapered cylinder).
// - Root nodes are treated as spheres (implemented as a degenerate segment).
//
// Acceleration:
// - Boost.Geometry rtree over expanded AABBs (expanded by the max radius of the primitive).
//
// Threading:
// - `containsPoint*` are safe for concurrent reads as long as the index is not mutated concurrently.
class ZSwcSpatialIndex final
{
public:
  ZSwcSpatialIndex() = default;

  // zScale controls how Z is weighted when testing "inside SWC tube?" in image-space coordinates.
  //
  // Legacy NeuTu defines zScale as the voxel-size ratio:
  //   zScale = voxelSizeZ / voxelSizeXY
  // (usually > 1 for microscopy volumes where Z spacing is coarser than XY).
  //
  // Geometric meaning:
  // - We treat distances in an anisotropic metric where dz is scaled by zScale:
  //     dist^2 = dx^2 + dy^2 + (dz * zScale)^2
  //   so for zScale>1, image-space points farther apart in Z are considered farther away.
  //
  // Note: Changing zScale invalidates any existing primitives; callers should set it before rebuild/insert.
  void setZScale(double zScale);
  [[nodiscard]] double zScale() const;

  void clear();

  void rebuild(const ZSwc& swc);

  // Inserts a new primitive into the index.
  // Caller is responsible for maintaining consistency with any corresponding SWC edits.
  void insertSegment(glm::dvec3 a, glm::dvec3 b, double ra, double rb);

  [[nodiscard]] bool empty() const;

  [[nodiscard]] size_t primitiveCount() const;

  [[nodiscard]] bool containsPoint(double x, double y, double z) const;
  [[nodiscard]] bool containsPoint(glm::dvec3 p) const;

private:
  struct Primitive
  {
    glm::dvec3 a{};
    glm::dvec3 b{};
    double ra = 0.0;
    double rb = 0.0;
  };

  using Point3 = boost::geometry::model::point<double, 3, boost::geometry::cs::cartesian>;
  using Box3 = boost::geometry::model::box<Point3>;
  using RTreeValue = std::pair<Box3, size_t>;
  using RTree = boost::geometry::index::rtree<RTreeValue, boost::geometry::index::rstar<16>>;

  [[nodiscard]] static Box3 primitiveBox(const Primitive& p);
  [[nodiscard]] static bool primitiveContainsPoint(const Primitive& prim, const glm::dvec3& p);

private:
  double m_zScale = 1.0;

  std::vector<Primitive> m_prims;
  RTree m_rtree;
};

} // namespace nim
