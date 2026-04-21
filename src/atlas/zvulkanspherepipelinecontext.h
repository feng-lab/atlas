#pragma once

#include "z3drendercommands.h"
#include "z3drendererstates.h"
#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkanstreamcachecoordinator.h"
#include "zvulkan.h"

#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace nim {

namespace vulkan {
struct AttachmentFormats;
}

class Z3DRendererBase;
class Z3DRendererVulkanBackend;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanBuffer;

class ZVulkanSpherePipelineContext
{
public:
  explicit ZVulkanSpherePipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanSpherePipelineContext();

  void resetFrame();
  void evictStream(uint64_t streamKey);
  [[nodiscard]] std::optional<Z3DRendererVulkanBackend::StaticPressureEvictionCandidate>
  oldestEvictableStaticStream(Z3DRendererVulkanBackend::StaticPressureDomain domain, uint64_t protectedEpoch) const;
  size_t evictStaticStreamForPressure(uint64_t streamKey);

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const SpherePayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  struct SphereVertex
  {
    glm::vec4 centerRadius{0.0f};
    glm::vec4 color{0.0f};
    float flags = 0.0f;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
    float pad2 = 0.0f;
  };

  struct FormatsKey
  {
    enum : size_t
    {
      kMaxColors = 8
    };
    std::array<vk::Format, kMaxColors> colorFormats{};
    uint32_t colorCount = 0;
    vk::Format depthFormat = vk::Format::eUndefined; // eUndefined means "no depth"

    static FormatsKey from(const vulkan::AttachmentFormats& formats);

    auto tie() const
    {
      return std::tuple(colorCount, colorFormats, depthFormat);
    }

    bool operator<(const FormatsKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };

  struct PipelineKey
  {
    bool dynamicMaterial = true;
    Z3DRendererBase::ShaderHookType shaderHookType = Z3DRendererBase::ShaderHookType::Normal;
    FormatsKey formats;

    auto tie() const
    {
      return std::tuple(dynamicMaterial, static_cast<int>(shaderHookType), formats);
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

  // Matches layout(push_constant) DDPPeelPC in Resources/shader/vulkan/dual_peeling_peel_sphere.frag.
  struct DDPPeelPushConstants
  {
    uint32_t ddpDepthBlender = 0;
    uint32_t ddpFrontBlender = 0;
  };

  Z3DRendererVulkanBackend& m_backend;

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  size_t m_vertexCount = 0;
  size_t m_indexCount = 0;
  bool m_usedStaticVBThisFrame = false; // debug/telemetry: true if bound static VBs after uploadGeometry
  // Upload arena-backed SoA slices (per-attribute buffers)
  vk::Buffer m_centerRadiusBuffer{};
  vk::Buffer m_colorBuffer{};
  vk::Buffer m_specularBuffer{};
  vk::Buffer m_flagsBuffer{};
  vk::DeviceSize m_centerRadiusOffset{0};
  vk::DeviceSize m_colorOffset{0};
  vk::DeviceSize m_specularOffset{0};
  vk::DeviceSize m_flagsOffset{0};
  vk::Buffer m_indexUploadBuffer{};
  vk::DeviceSize m_indexUploadOffset{0};

  // UBO retention across frames: keep last-frame UBOs alive until the active
  // submission fence signals to prevent transient flicker when frames overlap.
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
  // Vulkan command buffers can be reused across frames even when per-frame draw
  // ordering changes (e.g. camera-driven sorting). This matches OpenGL's
  // steady-state behavior: keep the same bindings and only update UBO contents.
  struct UboCacheEntry
  {
    bool pickingPass = false;
    // Cache signatures: only rewrite persistent UBOs when the originating
    // renderer state or captured clip planes change.
    RendererParameterState params{};
    bool followCoordTransform = true;
    bool followSizeScale = true;
    bool followOpacity = true;
    ClipPlanesState clipPlanes{};
    vk::DeviceSize objectTransformsOffset = 0;
    vk::DeviceSize materialOffset = 0;
    uint32_t lastSubmissionId = 0;
  };
  struct FrameUboCache
  {
    std::unordered_map<uint64_t, std::vector<UboCacheEntry>> byStream;
  };
  std::unordered_map<void*, FrameUboCache> m_uboCacheByFrameKey;
  // Device-local indirect args prepared per stream during DDP init. The draw
  // args arena is reset per submission, so resetFrame() clears this map.
  struct DDPArgs
  {
    bool prepared = false;
    bool indexed = false;
    vk::DeviceSize offset = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
  };
  struct DDPStreamKey
  {
    uint64_t streamKey = 0;
    uint32_t streamSegmentOrdinal = 0;

    bool operator==(const DDPStreamKey& rhs) const
    {
      return streamKey == rhs.streamKey && streamSegmentOrdinal == rhs.streamSegmentOrdinal;
    }
  };
  struct DDPStreamKeyHash
  {
    size_t operator()(const DDPStreamKey& key) const noexcept
    {
      size_t h = std::hash<uint64_t>{}(key.streamKey);
      h ^= std::hash<uint32_t>{}(key.streamSegmentOrdinal) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      return h;
    }
  };
  std::unordered_map<DDPStreamKey, DDPArgs, DDPStreamKeyHash> m_ddpArgsByStream;

  void touchStaticStream(uint64_t streamKey);
  [[nodiscard]] size_t staticBytesForStream(uint64_t streamKey,
                                            Z3DRendererVulkanBackend::StaticPressureDomain domain) const;

  // Lighting UBO is shared per-frame; no per-batch update is needed.
  void updateTransformUBO(Z3DRendererBase& renderer,
                          const RenderBatch& batch,
                          const SpherePayload& payload,
                          bool pickingPass);
  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  void uploadGeometry(const SpherePayload& payload);

  // Static promotion cache (SoA)
  struct GeometryCacheKey
  {
    uint64_t streamKey = 0;
    uint32_t streamSegmentOrdinal = 0;
    auto tie() const
    {
      return std::tuple(streamKey, streamSegmentOrdinal);
    }
    bool operator<(const GeometryCacheKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };
  struct GeometryCacheEntry
  {
    Z3DRendererVulkanBackend::StaticSlice vbCenterRadius{};
    Z3DRendererVulkanBackend::StaticSlice vbFlags{};
    Z3DRendererVulkanBackend::StaticSlice ib{};
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t centersGen = 0;
    uint32_t flagsGen = 0;
    uint32_t indexGen = 0;
    int unchangedFrames = 0;
    bool promoted = false;
    bool usedStaticOnce = false;
  };
  struct AppearanceCacheKey
  {
    uint64_t streamKey = 0;
    uint32_t streamSegmentOrdinal = 0;
    bool picking = false;
    bool dynamicMaterial = true;
    auto tie() const
    {
      return std::tuple(streamKey, streamSegmentOrdinal, picking, dynamicMaterial);
    }
    bool operator<(const AppearanceCacheKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };
  struct AppearanceCacheEntry
  {
    Z3DRendererVulkanBackend::StaticSlice vbColor{};
    Z3DRendererVulkanBackend::StaticSlice vbSpecular{};
    uint32_t vertexCount = 0;
    uint32_t colorsGen = 0;
    uint32_t pickingColorsGen = 0;
    uint32_t specularGen = 0;
    int unchangedFrames = 0;
    bool promoted = false;
    bool usedStaticOnce = false;
  };
  struct PendingGeometryUploadBinding
  {
    Z3DRendererVulkanBackend::UploadSlice centerRadius{};
    Z3DRendererVulkanBackend::UploadSlice flags{};
    Z3DRendererVulkanBackend::UploadSlice index{};
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t centersGen = 0;
    uint32_t flagsGen = 0;
    uint32_t indexGen = 0;
  };
  struct PendingAppearanceUploadBinding
  {
    Z3DRendererVulkanBackend::UploadSlice color{};
    Z3DRendererVulkanBackend::UploadSlice specular{};
    uint32_t vertexCount = 0;
    uint32_t colorsGen = 0;
    uint32_t pickingColorsGen = 0;
    uint32_t specularGen = 0;
  };
  using GeometryStreamCacheCoordinator =
    ZVulkanStreamCacheCoordinator<GeometryCacheKey, GeometryCacheEntry, PendingGeometryUploadBinding>;
  using AppearanceStreamCacheCoordinator =
    ZVulkanStreamCacheCoordinator<AppearanceCacheKey, AppearanceCacheEntry, PendingAppearanceUploadBinding>;
  ZVulkanStaticStreamUsageTracker m_streamUsageTracker;
  GeometryStreamCacheCoordinator m_geometryStreamCache;
  AppearanceStreamCacheCoordinator m_appearanceStreamCache;

  // Cached per-draw secondary command buffers (steady-state optimization).
  // Keyed by frame-slot identity + stream identity, since descriptor sets
  // reference the frame-slot uniform arena buffer.
  struct SecondaryCacheKey
  {
    void* frameKey = nullptr;
    uint64_t streamKey = 0;
    uint32_t streamSegmentOrdinal = 0;
    bool picking = false;
    bool dynamicMaterial = true;
    Z3DRendererBase::ShaderHookType shaderHookType = Z3DRendererBase::ShaderHookType::Normal;
    Z3DEye eye = MonoEye;
    uint32_t oitRingIndex = 0;

    bool operator==(const SecondaryCacheKey& rhs) const
    {
      return frameKey == rhs.frameKey && streamKey == rhs.streamKey &&
             streamSegmentOrdinal == rhs.streamSegmentOrdinal && picking == rhs.picking &&
             dynamicMaterial == rhs.dynamicMaterial && shaderHookType == rhs.shaderHookType && eye == rhs.eye &&
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
      h ^=
        std::hash<uint8_t>{}(static_cast<uint8_t>(key.dynamicMaterial)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
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
    std::array<vk::DescriptorSet, 3> baseDescriptorSets{};
    // Descriptor set generations are included so cached secondary command
    // buffers are rebuilt whenever any bound descriptor set contents change.
    // This avoids executing a secondary recorded against resources that were
    // later destroyed/recreated (a common cause of validation errors like
    // VUID-vkCmdExecuteCommands-pCommandBuffers-00089).
    std::array<uint64_t, 3> baseDescriptorGenerations{};
    bool hasOit = false;
    vk::DescriptorSet oitDescriptorSet{};
    // OIT descriptor-set generation: changes whenever the shared OIT descriptor
    // set is updated (vkUpdateDescriptorSets). Cached secondary command buffers
    // recorded against the previous contents must be rebuilt, otherwise Vulkan
    // validation will flag VUID-vkCmdExecuteCommands-pCommandBuffers-00089 and
    // drivers may exhibit undefined behavior.
    uint64_t oitDescriptorGeneration = 0;
    std::array<uint32_t, 4> dynamicOffsets{};
    std::array<vk::Buffer, 4> vertexBuffers{};
    std::array<vk::DeviceSize, 4> vertexOffsets{};
    std::array<uint64_t, 4> vertexBufferSegmentIds{};
    vk::Buffer indexBuffer{};
    vk::DeviceSize indexOffset{0};
    uint64_t indexBufferSegmentId = 0;
    vk::IndexType indexType{vk::IndexType::eUint32};
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
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
      return pipeline == rhs.pipeline && layout == rhs.layout && baseDescriptorSets == rhs.baseDescriptorSets &&
             baseDescriptorGenerations == rhs.baseDescriptorGenerations && hasOit == rhs.hasOit &&
             oitDescriptorSet == rhs.oitDescriptorSet && oitDescriptorGeneration == rhs.oitDescriptorGeneration &&
             dynamicOffsets == rhs.dynamicOffsets && vertexBuffers == rhs.vertexBuffers &&
             vertexOffsets == rhs.vertexOffsets && vertexBufferSegmentIds == rhs.vertexBufferSegmentIds &&
             indexBuffer == rhs.indexBuffer && indexOffset == rhs.indexOffset && indexType == rhs.indexType &&
             indexCount == rhs.indexCount && indexBufferSegmentId == rhs.indexBufferSegmentId &&
             vertexCount == rhs.vertexCount && viewportEq && scissorEq;
    }
  };

  struct SecondaryCacheEntry
  {
    SecondarySignature signature{};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    bool recorded = false;
  };

  std::unordered_map<SecondaryCacheKey, SecondaryCacheEntry, SecondaryCacheKeyHash> m_secondaryCache;
};

} // namespace nim
