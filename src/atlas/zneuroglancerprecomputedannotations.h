#pragma once

#include "zneuroglancerprecomputed.h"

#include "zglmutils.h"

#include <QUrl>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nim {

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

  struct Annotation
  {
    uint64_t id = 0;
    AnnotationType type = AnnotationType::Point;
    // Geometry in *voxel* coordinates (same convention as ZNeuroglancerPrecomputedSkeletonSource):
    // - Point: 1 point
    // - Line: 2 points
    // - Polyline: N points
    // - AABB: 2 points (min/max)
    // - Ellipsoid: 1 point (center) (radii not stored yet)
    std::vector<glm::vec3> points;
    std::optional<std::array<uint8_t, 4>> rgba8;
  };

  static std::shared_ptr<ZNeuroglancerPrecomputedAnnotationsSource> open(const QUrl& annotationRootUrl,
                                                                         std::array<double, 3> baseResolutionNm,
                                                                         std::array<int64_t, 3> baseVoxelOffset,
                                                                         std::chrono::milliseconds timeout);

  // Exposed for unit tests: parses an annotations/info JSON without performing network I/O.
  static std::shared_ptr<ZNeuroglancerPrecomputedAnnotationsSource> parseInfoJsonText(const QUrl& annotationRootUrl,
                                                                                      const std::string& infoText,
                                                                                      std::array<double, 3> baseResolutionNm,
                                                                                      std::array<int64_t, 3> baseVoxelOffset,
                                                                                      std::chrono::milliseconds timeout);

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

  [[nodiscard]] std::vector<Annotation> loadAnnotationsForRelatedObjectBlocking(const QString& relationshipId,
                                                                                uint64_t objectId) const;

  // Exposed for unit tests: decodes a related-object/spatial index entry (multiple annotation encoding).
  [[nodiscard]] std::vector<Annotation> decodeMultipleAnnotationBytes(std::span<const uint8_t> bytes) const;

private:
  [[nodiscard]] std::optional<RelationshipSpec> findRelationship(const QString& id) const;

  [[nodiscard]] std::vector<uint8_t> loadIndexEntryBlocking(const QUrl& dirUrl,
                                                            const std::optional<ZNeuroglancerPrecomputedVolume::Scale::Sharding>& sharding,
                                                            uint64_t key) const;

  [[nodiscard]] Annotation decodeAnnotationPayload(std::span<const uint8_t> bytes, size_t& off) const;

  [[nodiscard]] glm::vec3 voxelFromCoordUnits(const glm::vec3& coord) const;

  [[nodiscard]] std::vector<const PropertySpec*> props4Byte() const;
  [[nodiscard]] std::vector<const PropertySpec*> props2Byte() const;
  [[nodiscard]] std::vector<const PropertySpec*> props1Byte() const;

private:
  QUrl m_rootUrl;
  AnnotationType m_annotationType = AnnotationType::Point;

  std::array<double, 3> m_dimScaleNm{1.0, 1.0, 1.0};
  std::array<double, 3> m_baseResolutionNm{1.0, 1.0, 1.0};
  std::array<int64_t, 3> m_baseVoxelOffset{0, 0, 0};

  std::vector<PropertySpec> m_properties;
  std::vector<RelationshipSpec> m_relationships;
  std::optional<IndexSpec> m_byId;

  std::chrono::milliseconds m_timeout{30000};
};

} // namespace nim

