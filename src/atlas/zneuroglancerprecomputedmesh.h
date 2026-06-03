#pragma once

#include "zneuroglancerprecomputed.h"
#include "zglmutils.h"
#include "zbbox.h"
#include "zfolly.h"

#include <QUrl>
#include <QString>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace nim {

class ZMesh;
class ZRemoteObjectStore;
class ZNeuroglancerRemoteContext;

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

  // Controls the coordinate space of chunk meshes returned by loadChunkMeshBlocking()/loadChunkMeshAsync().
  //
  // - LocalVoxel: vertices are converted into Atlas local-voxel coordinates on CPU (legacy behavior).
  // - Quantized: vertices remain in Neuroglancer's stored quantized fragment coordinate space.
  //   The returned chunk mesh carries a per-submesh transform that maps these stored vertices to
  //   Atlas local-voxel space. Callers must apply it at render time.
  enum class ChunkVertexSpace
  {
    LocalVoxel,
    Quantized,
  };

  static std::shared_ptr<ZNeuroglancerPrecomputedMeshSource>
  open(const QUrl& meshDirUrl,
       std::array<double, 3> baseResolutionNm,
       std::array<int64_t, 3> baseVoxelOffset,
       std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext,
       const folly::CancellationToken& cancellationToken = {});

  static folly::coro::Task<std::shared_ptr<ZNeuroglancerPrecomputedMeshSource>>
  openAsync(const QUrl& meshDirUrl,
            std::array<double, 3> baseResolutionNm,
            std::array<int64_t, 3> baseVoxelOffset,
            std::shared_ptr<const ZNeuroglancerRemoteContext> remoteContext);

  static std::shared_ptr<ZNeuroglancerPrecomputedMeshSource>
  open(const QUrl& meshDirUrl,
       std::array<double, 3> baseResolutionNm,
       std::array<int64_t, 3> baseVoxelOffset,
       std::chrono::milliseconds timeout,
       std::shared_ptr<const ZRemoteObjectStore> objectStore = nullptr,
       const folly::CancellationToken& cancellationToken = {});

  static folly::coro::Task<std::shared_ptr<ZNeuroglancerPrecomputedMeshSource>>
  openAsync(const QUrl& meshDirUrl,
            std::array<double, 3> baseResolutionNm,
            std::array<int64_t, 3> baseVoxelOffset,
            std::chrono::milliseconds timeout,
            std::shared_ptr<const ZRemoteObjectStore> objectStore = nullptr);

  [[nodiscard]] const QUrl& meshDirUrl() const
  {
    CHECK(m_sharedMeshInfo);
    return m_sharedMeshInfo->meshDirUrl;
  }

  [[nodiscard]] MeshType meshType() const
  {
    CHECK(m_sharedMeshInfo);
    return m_sharedMeshInfo->meshType;
  }

  struct MultiLodOctreeNode
  {
    glm::uvec3 gridPosition{0U};
    uint32_t childBegin = 0;
    uint32_t childEndAndEmpty = 0;

    [[nodiscard]] uint32_t childEnd() const
    {
      return childEndAndEmpty & 0x7fffffffU;
    }

    [[nodiscard]] bool empty() const
    {
      return (childEndAndEmpty >> 31U) != 0U;
    }
  };

  struct MultiLodManifest
  {
    glm::vec3 chunkShape{0.0F};
    glm::vec3 gridOrigin{0.0F};
    glm::vec3 clipLowerBound{0.0F};
    glm::vec3 clipUpperBound{0.0F};
    std::vector<float> lodScales;
    std::vector<glm::vec3> vertexOffsets;
    std::vector<MultiLodOctreeNode> octreeNodes;
    std::vector<uint64_t> offsets;
    std::vector<uint32_t> rowLods;

    [[nodiscard]] uint32_t rootRow() const
    {
      CHECK(!octreeNodes.empty());
      return static_cast<uint32_t>(octreeNodes.size() - 1);
    }

    [[nodiscard]] std::optional<uint32_t> coarsestStoredLod() const;

    [[nodiscard]] uint64_t totalFragmentBytes() const;
  };

  struct MultiLodDesiredChunk
  {
    uint32_t lod = 0;
    uint32_t row = 0;
    float renderScale = 0.0F;
    bool empty = false;
  };

  struct MultiLodDrawChunk
  {
    uint32_t lod = 0;
    uint32_t row = 0;
    uint32_t subChunkBegin = 0;
    uint32_t subChunkEnd = 0;
    float renderScale = 0.0F;
  };

  struct MultiLodChunkMesh
  {
    std::vector<std::shared_ptr<ZMesh>> subMeshes;
    // Optional per-submesh transform from the mesh's stored vertex space to Atlas local-voxel space.
    // When empty, vertices are already in local-voxel space.
    std::vector<glm::mat4> vertexToLocalVoxelTransforms;
    std::vector<glm::mat3> vertexToLocalVoxelNormalMatrices;

    [[nodiscard]] bool empty() const;

    [[nodiscard]] bool hasVertexToLocalVoxelTransforms() const
    {
      return !vertexToLocalVoxelTransforms.empty();
    }
  };

  [[nodiscard]] bool supportsRuntimeLod() const
  {
    return meshType() == MeshType::MultiLodDraco;
  }

  // Loads multi-LOD mesh manifest metadata. This may perform network I/O and suspend.
  folly::coro::Task<std::shared_ptr<const MultiLodManifest>> loadManifestAsync(uint64_t segmentId) const;

  [[nodiscard]] std::shared_ptr<const MultiLodManifest>
  loadManifestBlocking(uint64_t segmentId, const folly::CancellationToken& cancellationToken) const;

  [[nodiscard]] std::shared_ptr<const MultiLodManifest> loadManifestBlocking(uint64_t segmentId) const;

  // Return the full clip bounds (AABB) of a runtime multi-LOD mesh in Atlas local-voxel coordinates.
  //
  // This is derived from the manifest's clipLowerBound/clipUpperBound (computed from LOD0 fragment grid coverage)
  // and the dataset's voxelFromStored transform, and does not require chunk vertices to be CPU-transformed.
  //
  // Intended use:
  // - camera framing / clipping range when the mesh is the only visible object
  // - selection/bound-box rendering for runtime-NG meshes whose vertices remain quantized
  [[nodiscard]] ZBBox<glm::dvec3> multiLodClipBoundsLocalVoxel(const MultiLodManifest& manifest) const;

  // Transform Neuroglancer multi-LOD stored manifest coordinates into Atlas local-voxel coordinates.
  //
  // Runtime LOD selection traverses manifest chunk bounds, which are expressed in the mesh's stored coordinate
  // system. Callers that project/cull those bounds must include this transform before the filter's coordTransform.
  [[nodiscard]] glm::mat4 multiLodStoredToLocalVoxelTransform() const;

  // Loads a multi-LOD chunk mesh. This may perform network I/O and suspend.
  folly::coro::Task<std::shared_ptr<const MultiLodChunkMesh>> loadChunkMeshAsync(uint64_t segmentId,
                                                                                 uint32_t row) const;

  // Loads a multi-LOD chunk mesh in the requested vertex space. This may perform network I/O and suspend.
  folly::coro::Task<std::shared_ptr<const MultiLodChunkMesh>>
  loadChunkMeshAsync(uint64_t segmentId, uint32_t row, ChunkVertexSpace vertexSpace) const;

  [[nodiscard]] std::shared_ptr<const MultiLodChunkMesh>
  loadChunkMeshBlocking(uint64_t segmentId, uint32_t row, const folly::CancellationToken& cancellationToken) const;

  [[nodiscard]] std::shared_ptr<const MultiLodChunkMesh> loadChunkMeshBlocking(uint64_t segmentId, uint32_t row) const;

  [[nodiscard]] std::shared_ptr<const MultiLodChunkMesh>
  loadChunkMeshBlocking(uint64_t segmentId,
                        uint32_t row,
                        ChunkVertexSpace vertexSpace,
                        const folly::CancellationToken& cancellationToken) const;

  [[nodiscard]] std::shared_ptr<const MultiLodChunkMesh>
  loadChunkMeshBlocking(uint64_t segmentId, uint32_t row, ChunkVertexSpace vertexSpace) const;

  [[nodiscard]] std::vector<std::shared_ptr<const MultiLodChunkMesh>>
  loadChunkMeshesBlocking(uint64_t segmentId, const std::vector<uint32_t>& rows) const;

  [[nodiscard]] static std::array<float, 24> getFrustumPlanes(const glm::mat4& modelViewProjection);

  [[nodiscard]] static std::vector<MultiLodDesiredChunk>
  desiredChunksForView(const MultiLodManifest& manifest,
                       const glm::mat4& modelViewProjection,
                       const std::array<float, 24>& clippingPlanes,
                       float detailCutoff,
                       uint32_t viewportWidth,
                       uint32_t viewportHeight);

  [[nodiscard]] static std::vector<MultiLodDrawChunk>
  chunksToDrawForView(const MultiLodManifest& manifest,
                      const glm::mat4& modelViewProjection,
                      const std::array<float, 24>& clippingPlanes,
                      float detailCutoff,
                      uint32_t viewportWidth,
                      uint32_t viewportHeight,
                      const std::function<bool(uint32_t lod, uint32_t row, float renderScale)>& hasChunk);

  // Loads and merges a full mesh for the requested segment. This may perform network I/O and suspend.
  folly::coro::Task<std::shared_ptr<ZMesh>> loadMeshAsync(uint64_t segmentId, LodPolicy lodPolicy) const;

  [[nodiscard]] std::shared_ptr<ZMesh> loadMeshBlocking(uint64_t segmentId, LodPolicy lodPolicy) const;

private:
  struct MultiLodInfo
  {
    double lodScaleMultiplier = 1.0;
    int vertexQuantizationBits = 10;
    glm::mat4 transform{1.0F};
    // Precomputed constant transform that maps from stored-model coordinates to
    // Atlas local-voxel coordinates.
    //
    // This equals: voxelFromModel(baseResolutionNm/baseVoxelOffset) * transform.
    glm::mat4 voxelFromStored{1.0F};
    std::optional<ZNeuroglancerPrecomputedVolume::Scale::Sharding> sharding;
  };

  // Immutable mesh-source metadata keyed by dataset identity plus remote-store content scope.
  //
  // This is the data that may be shared across opens through different remote
  // contexts only when the underlying store declares that they expose the same
  // content for a given URL. A live ZNeuroglancerPrecomputedMeshSource still owns its own
  // remote context and row/manifest caches, so global reuse stays store-agnostic
  // without aliasing one caller's transport state into another caller's source.
  struct SharedMeshInfo
  {
    QUrl meshDirUrl;
    MeshType meshType = MeshType::Legacy;
    std::optional<MultiLodInfo> multiLodInfo;
    std::array<double, 3> baseResolutionNm{};
    std::array<int64_t, 3> baseVoxelOffset{};

    // Precomputed constant transform from model (nanometer) coordinates to
    // Atlas local-voxel coordinates. Matches the legacy mesh conversion.
    glm::mat4 voxelFromModel{1.0F};
  };

  struct CachedMultiLodManifest
  {
    std::shared_ptr<const MultiLodManifest> manifest;
    std::string fragmentDataUrl;
    uint64_t fragmentDataBaseOffset = 0;
  };

  struct ShardedManifestBytes
  {
    std::vector<uint8_t> manifestBytes;
    uint64_t manifestStart = 0;
    std::string dataUrl;
  };

  struct ChunkCacheKey
  {
    uint64_t segmentId = 0;
    uint32_t row = 0;
    ChunkVertexSpace vertexSpace = ChunkVertexSpace::LocalVoxel;

    bool operator==(const ChunkCacheKey& other) const = default;
  };

  struct ChunkCacheKeyHash
  {
    size_t operator()(const ChunkCacheKey& key) const noexcept
    {
      const size_t h0 = std::hash<uint64_t>{}(key.segmentId);
      const size_t h1 = static_cast<size_t>(key.row) << 1U;
      const size_t h2 = static_cast<size_t>(static_cast<uint8_t>(key.vertexSpace)) << 24U;
      return h0 ^ h1 ^ h2;
    }
  };

private:
  [[nodiscard]] const SharedMeshInfo& sharedMeshInfo() const
  {
    CHECK(m_sharedMeshInfo);
    return *m_sharedMeshInfo;
  }

  [[nodiscard]] folly::coro::Task<std::shared_ptr<ZMesh>> loadLegacyMeshAsync(uint64_t segmentId) const;
  [[nodiscard]] folly::coro::Task<std::shared_ptr<ZMesh>> loadMultiLodMeshAsync(uint64_t segmentId, LodPolicy lodPolicy) const;
  [[nodiscard]] folly::coro::Task<std::shared_ptr<const CachedMultiLodManifest>>
  loadCachedManifestAsync(uint64_t segmentId) const;
  [[nodiscard]] folly::coro::Task<std::shared_ptr<const MultiLodChunkMesh>>
  loadMultiLodChunkMeshAsync(uint64_t segmentId, uint32_t row, ChunkVertexSpace vertexSpace) const;

  [[nodiscard]] folly::coro::Task<std::optional<std::vector<uint8_t>>> getHttpBytesAsync(const std::string& url) const;

  [[nodiscard]] folly::coro::Task<std::optional<std::vector<uint8_t>>> getHttpRangeBytesAsync(const std::string& url,
                                                                                              uint64_t offset,
                                                                                              uint64_t length) const;

  [[nodiscard]] folly::coro::Task<std::optional<ShardedManifestBytes>>
  getShardedManifestBytesAsync(uint64_t segmentId,
                               const ZNeuroglancerPrecomputedVolume::Scale::Sharding& sharding) const;

  [[nodiscard]] MultiLodManifest parseMultiLodManifest(std::span<const uint8_t> bytes, const MultiLodInfo& info) const;

  void convertLegacyMeshVerticesNmToLocalVoxel(ZMesh& mesh) const;

  struct VertexToLocalVoxelTransform
  {
    glm::mat4 vertexToLocalVoxel{1.0F};
    glm::mat3 vertexToLocalVoxelNormalMatrix{1.0F};
  };

  [[nodiscard]] VertexToLocalVoxelTransform computeMultiLodVertexToLocalVoxelTransform(size_t lod,
                                                                                       const glm::uvec3& fragmentPos,
                                                                                       const MultiLodManifest& manifest,
                                                                                       const MultiLodInfo& info) const;
  // Converts multi-LOD vertices from stored quantized coordinates into Atlas local-voxel coordinates.
  //
  // If |normals| is non-null, it is assumed to be in the same pre-transform space as |vertices|
  // and will be transformed using the correct normal matrix (inverse-transpose of the vertex
  // linear transform). If the normals are malformed or become invalid, returns false and the
  // caller should treat them as absent and fall back to CPU normal generation.
  [[nodiscard]] bool convertMultiLodVerticesToLocalVoxel(std::vector<glm::vec3>& vertices,
                                                         std::vector<glm::vec3>* normals,
                                                         size_t lod,
                                                         const glm::uvec3& fragmentPos,
                                                         const MultiLodManifest& manifest,
                                                         const MultiLodInfo& info,
                                                         const folly::CancellationToken& cancellationToken) const;

private:
  std::shared_ptr<const SharedMeshInfo> m_sharedMeshInfo;
  std::shared_ptr<const ZNeuroglancerRemoteContext> m_remoteContext;

  mutable std::mutex m_manifestCacheMutex;
  mutable std::unordered_map<uint64_t, std::weak_ptr<const CachedMultiLodManifest>> m_manifestCache;

  mutable std::mutex m_chunkCacheMutex;
  mutable std::unordered_map<ChunkCacheKey, std::weak_ptr<const MultiLodChunkMesh>, ChunkCacheKeyHash> m_chunkCache;
};

} // namespace nim
