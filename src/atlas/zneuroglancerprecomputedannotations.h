#pragma once

#include "zneuroglancerprecomputed.h"

#include "zglmutils.h"

#include <QUrl>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace nim {

class ZRemoteObjectStore;
class ZNeuroglancerRemoteContext;

class ZNeuroglancerPrecomputedAnnotationsSource
{
public:
  enum class AnnotationType
  {
    Point,
    Line,
    AxisAlignedBoundingBox,
    Ellipsoid,
    Polyline,
  };

  enum class PropertyType
  {
    Rgb,
    Rgba,
    Uint8,
    Int8,
    Uint16,
    Int16,
    Uint32,
    Int32,
    Float32,
  };

  struct PropertySpec
  {
    QString id;
    PropertyType type = PropertyType::Uint8;
    QString description;
  };

  struct RelationshipSpec
  {
    QString id;
    QUrl indexDirUrl;
    std::optional<ZNeuroglancerPrecomputedVolume::Scale::Sharding> sharding;
  };

  struct IndexSpec
  {
    QUrl indexDirUrl;
    std::optional<ZNeuroglancerPrecomputedVolume::Scale::Sharding> sharding;
  };

  struct SpatialLevelSpec
  {
    QUrl indexDirUrl;
    std::optional<ZNeuroglancerPrecomputedVolume::Scale::Sharding> sharding;
    std::array<uint64_t, 3> gridShape{0, 0, 0};
    std::array<double, 3> chunkSize{0.0, 0.0, 0.0}; // in coordinate units defined by "dimensions"
    uint64_t limit = 0;
  };

  struct Annotation
  {
    using PropertyValue = std::variant<std::array<uint8_t, 3>,
                                       std::array<uint8_t, 4>,
                                       uint8_t,
                                       int8_t,
                                       uint16_t,
                                       int16_t,
                                       uint32_t,
                                       int32_t,
                                       float>;

    uint64_t id = 0;
    AnnotationType type = AnnotationType::Point;
    // Geometry in *voxel* coordinates (same convention as ZNeuroglancerPrecomputedSkeletonSource):
    // - Point: 1 point
    // - Line: 2 points
    // - Polyline: N points
    // - AABB: 2 points (min/max)
    // - Ellipsoid: 1 point (center) + optional radii (voxel units)
    std::vector<glm::vec3> points;
    std::optional<std::array<uint8_t, 4>> rgba8;
    std::optional<glm::vec3> ellipsoidRadiiVoxel;
    std::vector<PropertyValue> propertyValues;
  };

  static std::shared_ptr<ZNeuroglancerPrecomputedAnnotationsSource>
  open(const QUrl& annotationRootUrl,
       std::array<double, 3> baseResolutionNm,
       std::array<int64_t, 3> baseVoxelOffset,
       std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext);

  static std::shared_ptr<ZNeuroglancerPrecomputedAnnotationsSource>
  open(const QUrl& annotationRootUrl,
       std::array<double, 3> baseResolutionNm,
       std::array<int64_t, 3> baseVoxelOffset,
       std::chrono::milliseconds timeout,
       std::shared_ptr<const ZRemoteObjectStore> objectStore = nullptr);

  // Exposed for unit tests: parses an annotations/info JSON without performing network I/O.
  static std::shared_ptr<ZNeuroglancerPrecomputedAnnotationsSource>
  parseInfoJsonText(const QUrl& annotationRootUrl,
                    const std::string& infoText,
                    std::array<double, 3> baseResolutionNm,
                    std::array<int64_t, 3> baseVoxelOffset,
                    std::chrono::milliseconds timeout,
                    std::shared_ptr<const ZRemoteObjectStore> objectStore = nullptr);

  // Internal reader-facing overload: use an existing remote context instead of rebuilding timeout/store state.
  static std::shared_ptr<ZNeuroglancerPrecomputedAnnotationsSource>
  parseInfoJsonText(const QUrl& annotationRootUrl,
                    const std::string& infoText,
                    std::array<double, 3> baseResolutionNm,
                    std::array<int64_t, 3> baseVoxelOffset,
                    std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext = nullptr);

  [[nodiscard]] const QUrl& rootUrl() const
  {
    return m_rootUrl;
  }

  [[nodiscard]] AnnotationType annotationType() const
  {
    return m_annotationType;
  }

  [[nodiscard]] const std::vector<PropertySpec>& properties() const
  {
    return m_properties;
  }

  [[nodiscard]] const std::vector<RelationshipSpec>& relationships() const
  {
    return m_relationships;
  }

  [[nodiscard]] const std::vector<SpatialLevelSpec>& spatialLevels() const
  {
    return m_spatial;
  }

  [[nodiscard]] std::vector<Annotation> loadAnnotationsForRelatedObjectBlocking(const QString& relationshipId,
                                                                                uint64_t objectId) const;

  // Loads annotations intersecting the given voxel-space box using the multi-level spatial index.
  // This returns the full set of intersecting annotations (deduplicated by id), not a subsample.
  [[nodiscard]] std::vector<Annotation> loadAnnotationsIntersectingVoxelBoxBlocking(const glm::dvec3& voxelMin,
                                                                                    const glm::dvec3& voxelMax) const;

  struct SpatialLoadProgress
  {
    uint64_t totalCells = 0;
    uint64_t visitedCells = 0;
    uint64_t uniqueAnnotations = 0;
    size_t levelsTotal = 0;
    size_t levelIndex = 0; // [0, levelsTotal)
  };

  struct SpatialLoadUpdate
  {
    SpatialLoadProgress progress;
    std::vector<Annotation> newAnnotations;
  };

  using SpatialLoadUpdateCallback = std::function<void(SpatialLoadUpdate update)>;

  // Streams annotations intersecting the given voxel-space box using the multi-level spatial index,
  // invoking `onUpdate` as new unique annotations become available.
  //
  // This method is blocking and performs network I/O; callers should run it off the UI thread.
  // Updates are rate-limited by `minUpdateInterval` and `maxAnnotationsPerUpdate`.
  // Cancellation is cooperative: if `cancelFlag` is provided and becomes true, streaming stops as
  // soon as possible (but any in-flight HTTP request may still take time to return).
  void streamAnnotationsIntersectingVoxelBoxBlocking(const glm::dvec3& voxelMin,
                                                     const glm::dvec3& voxelMax,
                                                     const SpatialLoadUpdateCallback& onUpdate,
                                                     const std::atomic_bool* cancelFlag = nullptr,
                                                     std::chrono::milliseconds minUpdateInterval =
                                                       std::chrono::milliseconds{200},
                                                     size_t maxAnnotationsPerUpdate = 2048) const;

  // Exposed for unit tests: decodes a related-object/spatial index entry (multiple annotation encoding).
  [[nodiscard]] std::vector<Annotation> decodeMultipleAnnotationBytes(std::span<const uint8_t> bytes) const;

private:
  [[nodiscard]] std::optional<RelationshipSpec> findRelationship(const QString& id) const;

  [[nodiscard]] std::vector<uint8_t> loadIndexEntryBlocking(const QUrl& dirUrl,
                                                            const std::optional<ZNeuroglancerPrecomputedVolume::Scale::Sharding>& sharding,
                                                            uint64_t key) const;

  [[nodiscard]] std::optional<std::vector<uint8_t>> loadSpatialCellEntryBlocking(const SpatialLevelSpec& level,
                                                                                 const std::array<uint64_t, 3>& cell) const;

  [[nodiscard]] Annotation decodeAnnotationPayload(std::span<const uint8_t> bytes, size_t& off) const;

  [[nodiscard]] glm::vec3 voxelFromCoordUnits(const glm::vec3& coord) const;
  [[nodiscard]] glm::dvec3 coordUnitsFromVoxel(const glm::dvec3& voxel) const;

private:
  QUrl m_rootUrl;
  AnnotationType m_annotationType = AnnotationType::Point;

  std::array<double, 3> m_dimScaleNm{1.0, 1.0, 1.0};
  std::array<double, 3> m_baseResolutionNm{1.0, 1.0, 1.0};
  std::array<int64_t, 3> m_baseVoxelOffset{0, 0, 0};

  std::array<double, 3> m_lowerBoundCoord{0.0, 0.0, 0.0};
  std::array<double, 3> m_upperBoundCoord{0.0, 0.0, 0.0};
  std::vector<SpatialLevelSpec> m_spatial;

  std::vector<PropertySpec> m_properties;
  std::vector<RelationshipSpec> m_relationships;
  std::optional<IndexSpec> m_byId;

  std::shared_ptr<const ZNeuroglancerRemoteContext> m_remoteContext;
};

} // namespace nim
