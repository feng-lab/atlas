#pragma once

#include "zneuroglancerprecomputed.h"

#include "zglmutils.h"
#include "zskeleton.h"

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

class ZRemoteObjectStore;
class ZNeuroglancerRemoteContext;

class ZNeuroglancerPrecomputedSkeletonSource
{
public:
  static std::shared_ptr<ZNeuroglancerPrecomputedSkeletonSource>
  open(const QUrl& skeletonDirUrl,
       std::array<double, 3> baseResolutionNm,
       std::array<int64_t, 3> baseVoxelOffset,
       std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext);

  static std::shared_ptr<ZNeuroglancerPrecomputedSkeletonSource>
  open(const QUrl& skeletonDirUrl,
       std::array<double, 3> baseResolutionNm,
       std::array<int64_t, 3> baseVoxelOffset,
       std::chrono::milliseconds timeout,
       std::shared_ptr<const ZRemoteObjectStore> objectStore = nullptr);

  // Exposed for unit tests: parses a skeletons/info JSON without performing network I/O.
  static std::shared_ptr<ZNeuroglancerPrecomputedSkeletonSource>
  parseInfoJsonText(const QUrl& skeletonDirUrl,
                    const std::string& infoText,
                    std::array<double, 3> baseResolutionNm,
                    std::array<int64_t, 3> baseVoxelOffset,
                    std::chrono::milliseconds timeout,
                    std::shared_ptr<const ZRemoteObjectStore> objectStore = nullptr);

  // Internal reader-facing overload: use an existing remote context instead of rebuilding timeout/store state.
  static std::shared_ptr<ZNeuroglancerPrecomputedSkeletonSource>
  parseInfoJsonText(const QUrl& skeletonDirUrl,
                    const std::string& infoText,
                    std::array<double, 3> baseResolutionNm,
                    std::array<int64_t, 3> baseVoxelOffset,
                    std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext = nullptr);

  [[nodiscard]] const QUrl& skeletonDirUrl() const
  {
    return m_skeletonDirUrl;
  }

  [[nodiscard]] bool hasSharding() const
  {
    return m_sharding.has_value();
  }

  [[nodiscard]] std::shared_ptr<ZSkeleton> loadSkeletonBlocking(uint64_t segmentId) const;

  // Exposed for unit tests: decodes a single encoded skeleton file.
  [[nodiscard]] std::shared_ptr<ZSkeleton> decodeSkeletonBytes(std::span<const uint8_t> bytes) const;

private:
  enum class VertexAttributeDataType
  {
    Float32,
    Int8,
    Uint8,
    Int16,
    Uint16,
    Int32,
    Uint32,
  };

  struct VertexAttribute
  {
    std::string id;
    VertexAttributeDataType dataType = VertexAttributeDataType::Float32;
    size_t numComponents = 1;
  };

private:
  QUrl m_skeletonDirUrl;
  glm::mat4 m_transform{1.0F};
  std::vector<VertexAttribute> m_vertexAttributes;
  bool m_hasRadiusAttribute = false;
  std::optional<ZNeuroglancerPrecomputedVolume::Scale::Sharding> m_sharding;

  std::array<double, 3> m_baseResolutionNm{};
  std::array<int64_t, 3> m_baseVoxelOffset{};

  std::shared_ptr<const ZNeuroglancerRemoteContext> m_remoteContext;
};

} // namespace nim
