#pragma once

#include "zneuroglancerprecomputed.h"
#include "zglmutils.h"

#include <QUrl>
#include <QString>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace nim {

class ZMesh;

class ZNeuroglancerPrecomputedMeshSource
{
public:
  enum class MeshType
  {
    Legacy,
    MultiLodDraco,
  };

  enum class LodPolicy
  {
    Coarsest,
    Finest,
  };

  static std::shared_ptr<ZNeuroglancerPrecomputedMeshSource> open(
    const QUrl& meshDirUrl,
    std::array<double, 3> baseResolutionNm,
    std::array<int64_t, 3> baseVoxelOffset,
    std::chrono::milliseconds timeout);

  [[nodiscard]] const QUrl& meshDirUrl() const
  {
    return m_meshDirUrl;
  }

  [[nodiscard]] MeshType meshType() const
  {
    return m_meshType;
  }

  [[nodiscard]] std::shared_ptr<ZMesh> loadMeshBlocking(uint64_t segmentId, LodPolicy lodPolicy) const;

private:
  struct MultiLodInfo
  {
    double lodScaleMultiplier = 1.0;
    int vertexQuantizationBits = 10;
    glm::mat4 transform{1.0F};
    std::optional<ZNeuroglancerPrecomputedVolume::Scale::Sharding> sharding;
  };

  struct MultiLodManifest
  {
    glm::vec3 chunkShape{0.0F};
    glm::vec3 gridOrigin{0.0F};
    std::vector<float> lodScales;
    std::vector<glm::vec3> vertexOffsets;
    std::vector<std::vector<glm::uvec3>> fragmentPositions;
    std::vector<std::vector<uint32_t>> fragmentSizes;
    std::vector<uint64_t> lodStartOffsets;
    std::vector<uint64_t> lodTotalSizes;

    [[nodiscard]] uint64_t totalFragmentBytes() const;
  };

private:
  [[nodiscard]] folly::coro::Task<std::shared_ptr<ZMesh>> loadLegacyMeshAsync(uint64_t segmentId) const;
  [[nodiscard]] folly::coro::Task<std::shared_ptr<ZMesh>> loadMultiLodMeshAsync(uint64_t segmentId, LodPolicy lodPolicy) const;

  [[nodiscard]] folly::coro::Task<std::optional<std::vector<uint8_t>>> getHttpBytesAsync(const std::string& url) const;

  [[nodiscard]] folly::coro::Task<std::optional<std::vector<uint8_t>>> getHttpRangeBytesAsync(const std::string& url,
                                                                                              uint64_t offset,
                                                                                              uint64_t length) const;

  [[nodiscard]] folly::coro::Task<std::optional<std::vector<uint8_t>>> getShardedManifestBytesAsync(uint64_t segmentId,
                                                                                                    uint64_t* manifestStart,
                                                                                                    const ZNeuroglancerPrecomputedVolume::Scale::Sharding& sharding) const;

  [[nodiscard]] MultiLodManifest parseMultiLodManifest(std::span<const uint8_t> bytes, const MultiLodInfo& info) const;

  void convertLegacyMeshVerticesNmToLocalVoxel(ZMesh& mesh) const;
  void convertMultiLodFragmentToLocalVoxel(ZMesh& fragment,
                                          size_t lod,
                                          const glm::uvec3& fragmentPos,
                                          const MultiLodManifest& manifest,
                                          const MultiLodInfo& info) const;

private:
  QUrl m_meshDirUrl;
  MeshType m_meshType = MeshType::Legacy;
  std::optional<MultiLodInfo> m_multiLodInfo;

  std::array<double, 3> m_baseResolutionNm{};
  std::array<int64_t, 3> m_baseVoxelOffset{};

  std::chrono::milliseconds m_timeout{30000};
};

} // namespace nim
