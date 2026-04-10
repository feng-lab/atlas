#pragma once

#include "z3drendercommands.h"
#include "z3drendererstates.h"
#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanmeshstreamcoordinator.h"
#include "zvulkan.h"
#include "zglmutils.h"

#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace nim {

enum class FogMode;

namespace vulkan {
struct AttachmentFormats;
}

class Z3DRendererBase;
class Z3DRendererVulkanBackend;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanTexture;
class ZVulkanBuffer;
class ZMesh;
class Z3DMeshRenderer;

class ZVulkanMeshPipelineContext
{
public:
  explicit ZVulkanMeshPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanMeshPipelineContext();

  void resetFrame();
  void evictStream(uint64_t streamKey);
  [[nodiscard]] std::optional<Z3DRendererVulkanBackend::StaticPressureEvictionCandidate>
  oldestEvictableStaticStream(Z3DRendererVulkanBackend::StaticPressureDomain domain, uint64_t protectedEpoch) const;
  size_t evictStaticStreamForPressure(uint64_t streamKey);

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const MeshPayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  struct PipelineKey
  {
    MeshPayload::ColorSource colorSource = MeshPayload::ColorSource::MeshColor;
    ZMesh::Type meshType;
    bool wireframe = false;
    FogMode fogMode = FogMode::None;
    Z3DRendererBase::ShaderHookType shaderHookType = Z3DRendererBase::ShaderHookType::Normal;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(static_cast<int>(colorSource),
                        static_cast<int>(meshType),
                        wireframe,
                        static_cast<int>(fogMode),
                        static_cast<int>(shaderHookType),
                        colorFormats,
                        depthFormat);
    }

    bool operator<(const PipelineKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };

  struct PipelineInstance
  {
    std::unique_ptr<ZVulkanShader> shader;
    std::unique_ptr<ZVulkanPipeline> pipeline;
  };

  struct MeshDraw
  {
    ZMesh* mesh = nullptr;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    bool indexed = false;
    size_t payloadMeshIndex = 0;
    bool useFallbackColor = false;
    glm::vec4 fallbackColor{1.0f};
  };

  // Matches layout(push_constant) MeshBindlessPC in Resources/shader/vulkan/include/mesh_func.glslinc.
  struct MeshBindlessPushConstants
  {
    uint32_t tex1D = 0;
    uint32_t tex2D = 0;
    uint32_t tex3D = 0;
    uint32_t ddpDepthBlender = 0;
    uint32_t ddpFrontBlender = 0;
  };

  Z3DRendererVulkanBackend& m_backend;

  // Static SoA promotion cache + same-submission upload reuse for mesh streams.
  ZVulkanMeshStreamCoordinator m_streamCoordinator;

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  // Retain last-frame UBOs until the active submission fence signals to
  // prevent read-after-free artifacts when frames overlap.
  std::vector<std::shared_ptr<ZVulkanBuffer>> m_retainedUbos;
  void retainUbo(std::unique_ptr<ZVulkanBuffer>& ubo)
  {
    if (ubo) {
      m_retainedUbos.emplace_back(std::shared_ptr<ZVulkanBuffer>(std::move(ubo)));
    }
  }
  void flushRetainedUbos();

  // Dynamic UBO offsets for this draw
  vk::DeviceSize m_dynLightingOffset{0};
  vk::DeviceSize m_dynFrameTransformsOffset{0};
  vk::DeviceSize m_dynObjectTransformsOffset{0};
  vk::DeviceSize m_dynMaterialOffset{0};
  // Cache per-stream dynamic UBO offsets in the persistent uniform arena so
  // command buffers can be reused across frames (steady-state).
  struct MaterialKey
  {
    enum class Kind : uint8_t
    {
      SharedSurface = 0,
      SharedSurfaceFallback = 1,
      SharedWireframe = 2,
      PerMesh = 3,
    };

    Kind kind = Kind::SharedSurface;
    bool pickingPass = false;
    // Used only when kind == PerMesh. (When kind != PerMesh, meshIndex is ignored.)
    size_t meshIndex = 0;

    bool operator==(const MaterialKey& rhs) const
    {
      return kind == rhs.kind && pickingPass == rhs.pickingPass && meshIndex == rhs.meshIndex;
    }
  };

  struct MaterialKeyHash
  {
    size_t operator()(const MaterialKey& key) const noexcept
    {
      size_t h = std::hash<uint8_t>{}(static_cast<uint8_t>(key.kind));
      h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.pickingPass)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<size_t>{}(key.meshIndex) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      return h;
    }
  };

  struct MaterialCacheEntry
  {
    RendererParameterState params{};
    bool followOpacity = true;
    bool pickingPass = false;
    bool useCustomColor = false;
    glm::vec4 customColor{1.0f};
    vk::DeviceSize materialOffset = 0;
  };

  struct StreamUboCache
  {
    RendererParameterState params{};
    bool followCoordTransform = true;
    bool followSizeScale = true;
    ClipPlanesState clipPlanes{};
    vk::DeviceSize objectTransformsOffset = 0;
    bool objectTransformsValid = false;

    std::unordered_map<MaterialKey, MaterialCacheEntry, MaterialKeyHash> materials;
  };

  struct FrameUboCache
  {
    std::unordered_map<uint64_t, StreamUboCache> byStream;
  };

  std::unordered_map<void*, FrameUboCache> m_uboCacheByFrameKey;

  // Cached per-pipeline-variant secondary command buffers (steady-state optimization).
  struct SecondaryCacheKey
  {
    void* frameKey = nullptr;
    uint64_t streamKey = 0;
    uint32_t streamSegmentOrdinal = 0;
    bool picking = false;
    MeshPayload::ColorSource colorSource = MeshPayload::ColorSource::MeshColor;
    ZMesh::Type meshType = ZMesh::Type::TRIANGLES;
    bool wireframe = false;
    FogMode fogMode = FogMode::None;
    Z3DRendererBase::ShaderHookType shaderHookType = Z3DRendererBase::ShaderHookType::Normal;
    Z3DEye eye = MonoEye;
    uint32_t oitRingIndex = 0;

    bool operator==(const SecondaryCacheKey& rhs) const
    {
      return frameKey == rhs.frameKey && streamKey == rhs.streamKey &&
             streamSegmentOrdinal == rhs.streamSegmentOrdinal && picking == rhs.picking &&
             colorSource == rhs.colorSource && meshType == rhs.meshType && wireframe == rhs.wireframe &&
             fogMode == rhs.fogMode && shaderHookType == rhs.shaderHookType && eye == rhs.eye &&
             oitRingIndex == rhs.oitRingIndex;
    }
  };

  struct SecondaryCacheKeyHash
  {
    size_t operator()(const SecondaryCacheKey& key) const noexcept
    {
      size_t h = std::hash<uintptr_t>{}(reinterpret_cast<uintptr_t>(key.frameKey));
      h ^= std::hash<uint64_t>{}(key.streamKey) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<uint32_t>{}(key.streamSegmentOrdinal) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.picking)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<int>{}(static_cast<int>(key.colorSource)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<int>{}(static_cast<int>(key.meshType)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.wireframe)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<int>{}(static_cast<int>(key.fogMode)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<int>{}(static_cast<int>(key.shaderHookType)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<int>{}(static_cast<int>(key.eye)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<uint32_t>{}(key.oitRingIndex) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      return h;
    }
  };

  struct SecondarySignature
  {
    vk::Pipeline pipeline{};
    vk::PipelineLayout layout{};
    // Descriptor sets and generations:
    //   [0] bindless sampled images
    //   [1] lighting (per-frame uniform arena)
    //   [2] transforms (frame uniform + persistent object/material)
    //   [3] OIT (per-ring slot, stable handle)
    std::array<vk::DescriptorSet, 4> descriptorSets{};
    std::array<uint64_t, 4> descriptorGenerations{};
    // Fixed dynamic offsets shared by all draws in this variant:
    //   lighting, frame transforms, object transforms.
    std::array<uint32_t, 3> fixedDynamicOffsets{};
    std::array<vk::Buffer, 6> vertexBuffers{};
    std::array<vk::DeviceSize, 6> vertexOffsets{};
    std::array<uint64_t, 6> vertexBufferSegmentIds{};
    vk::Buffer indexBuffer{};
    vk::DeviceSize indexOffset{0};
    uint64_t indexBufferSegmentId = 0;
    vk::IndexType indexType{vk::IndexType::eUint32};
    MeshBindlessPushConstants pushConstants{};

    struct Draw
    {
      uint32_t materialOffset = 0;
      bool indexed = false;
      uint32_t indexCount = 0;
      uint32_t firstIndex = 0;
      int32_t vertexOffset = 0; // for indexed draws
      uint32_t vertexCount = 0;
      uint32_t firstVertex = 0; // for non-indexed draws

      bool operator==(const Draw& rhs) const
      {
        return materialOffset == rhs.materialOffset && indexed == rhs.indexed && indexCount == rhs.indexCount &&
               firstIndex == rhs.firstIndex && vertexOffset == rhs.vertexOffset && vertexCount == rhs.vertexCount &&
               firstVertex == rhs.firstVertex;
      }
    };

    std::vector<Draw> draws;
    vk::Viewport viewport{};
    vk::Rect2D scissor{};

    bool operator==(const SecondarySignature& rhs) const
    {
      const bool viewportEq = (viewport.x == rhs.viewport.x) && (viewport.y == rhs.viewport.y) &&
                              (viewport.width == rhs.viewport.width) && (viewport.height == rhs.viewport.height) &&
                              (viewport.minDepth == rhs.viewport.minDepth) &&
                              (viewport.maxDepth == rhs.viewport.maxDepth);
      const bool scissorEq = (scissor.offset.x == rhs.scissor.offset.x) && (scissor.offset.y == rhs.scissor.offset.y) &&
                             (scissor.extent.width == rhs.scissor.extent.width) &&
                             (scissor.extent.height == rhs.scissor.extent.height);
      return pipeline == rhs.pipeline && layout == rhs.layout && descriptorSets == rhs.descriptorSets &&
             descriptorGenerations == rhs.descriptorGenerations && fixedDynamicOffsets == rhs.fixedDynamicOffsets &&
             vertexBuffers == rhs.vertexBuffers && vertexOffsets == rhs.vertexOffsets &&
             vertexBufferSegmentIds == rhs.vertexBufferSegmentIds && indexBuffer == rhs.indexBuffer &&
             indexOffset == rhs.indexOffset && indexBufferSegmentId == rhs.indexBufferSegmentId &&
             indexType == rhs.indexType &&
             std::memcmp(&pushConstants, &rhs.pushConstants, sizeof(pushConstants)) == 0 && draws == rhs.draws &&
             viewportEq && scissorEq;
    }
  };

  struct SecondaryCacheEntry
  {
    SecondarySignature signature{};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    bool recorded = false;
  };

  std::unordered_map<SecondaryCacheKey, SecondaryCacheEntry, SecondaryCacheKeyHash> m_secondaryCache;

  size_t m_vertexCount = 0;
  size_t m_indexCount = 0;
  bool m_usedStaticVBThisFrame{false};

  std::vector<MeshDraw> m_draws;
  // Device-local indirect args per mesh for DDP peel; prepared during init and
  // reused for subsequent peel passes in the same frame.
  std::vector<vk::DeviceSize> m_ddpArgsOffsets;
  std::vector<uint8_t> m_ddpArgsPrepared;

  // No GL texture bridging: Vulkan mesh pipeline uses placeholders or
  // backend-native textures only.

  // SoA upload arena-backed streams (per-attribute buffers)
  vk::Buffer m_posBuffer{};
  vk::Buffer m_normBuffer{};
  vk::Buffer m_colorBuffer{};
  vk::Buffer m_texBuffer{}; // 1D/2D/3D depending on colorSource
  vk::DeviceSize m_posOffset{0};
  vk::DeviceSize m_normOffset{0};
  vk::DeviceSize m_colorOffset{0};
  vk::DeviceSize m_texOffset{0}; // 1D/2D/3D depending on colorSource
  enum class TexBinding
  {
    None,
    Tex1D,
    Tex2D,
    Tex3D
  } m_texBinding = TexBinding::None;
  vk::Buffer m_indexUploadBuffer{};
  vk::DeviceSize m_indexUploadOffset{0};

  // Lighting UBO is shared per-frame; no per-batch update is needed.
  void updateTransformUBO(Z3DRendererBase& renderer, const RenderBatch& batch, const MeshPayload& payload);
  void updateMaterialUBO(Z3DRendererBase& renderer,
                         const MeshPayload& payload,
                         size_t meshIndex,
                         bool useFallbackColor,
                         const glm::vec4& fallbackColor,
                         bool pickingPass,
                         bool wireframe,
                         bool usePersistentTransforms,
                         Z3DRendererBase::ShaderHookType shaderHook);
  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  void uploadGeometry(const MeshPayload& payload);
};

} // namespace nim
