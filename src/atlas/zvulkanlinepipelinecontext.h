#pragma once

#include "z3drendercommands.h"
#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkan.h"

#include <array>
#include <memory>
#include <map>
#include <optional>
#include <set>
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
class ZVulkanTexture;
class ZVulkanBuffer;
class Z3DLineRenderer;

struct VulkanThinLineVertex
{
  glm::vec3 pos;
  glm::vec4 color;
};

class ZVulkanLinePipelineContext
{
public:
  explicit ZVulkanLinePipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanLinePipelineContext();

  void resetFrame();
  void evictStream(uint64_t streamKey);

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const LinePayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  // Matches layout(push_constant) DDPPeelPC in Resources/shader/vulkan/dual_peeling_peel_line.frag.
  struct DDPPeelPushConstants
  {
    uint32_t ddpDepthBlender = 0;
    uint32_t ddpFrontBlender = 0;
  };

  // Matches layout(push_constant) WideLinePC in Resources/shader/vulkan/include/wideline_common.glslinc.
  struct WideLinePushConstants
  {
    glm::mat4 viewport_matrix{1.0f};
    glm::mat4 viewport_matrix_inverse{1.0f};
    float line_width = 1.0f;
    float size_scale = 1.0f;
    uint32_t line_texture = 0;
    uint32_t ddpDepthBlender = 0;
    uint32_t ddpFrontBlender = 0;
    uint32_t _pad = 0;
  };

  struct PipelineKey
  {
    bool useSmooth = true;
    bool picking = false;
    bool roundCap = true;
    bool screenAligned = false;
    bool useTextureColor = false;
    bool lineStrip = false;
    Z3DRendererBase::ShaderHookType shaderHookType = Z3DRendererBase::ShaderHookType::Normal;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(useSmooth,
                        picking,
                        roundCap,
                        screenAligned,
                        useTextureColor,
                        lineStrip,
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

  Z3DRendererVulkanBackend& m_backend;

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  // All line geometry uses the per-frame upload arena; no per-context VBOs

  // Upload arena-backed SoA for thin line (per-draw, per-attribute buffers)
  vk::Buffer m_thinPosBuffer{};
  vk::Buffer m_thinColorBuffer{};
  vk::DeviceSize m_thinPosOffset{0};
  vk::DeviceSize m_thinColorOffset{0};
  uint32_t m_thinUploadVertexCount{0};
  vk::Buffer m_thinUploadIndexBuffer{};
  vk::DeviceSize m_thinUploadIndexOffset{0};
  uint32_t m_thinUploadIndexCount{0};
  bool m_usedThinStaticVBThisFrame{false};

  // Upload arena-backed SoA for wide line (per-draw, per-attribute buffers)
  vk::Buffer m_wideP0Buffer{};
  vk::Buffer m_wideP1Buffer{};
  vk::Buffer m_wideC0Buffer{};
  vk::Buffer m_wideC1Buffer{};
  vk::Buffer m_wideFlagsBuffer{};
  vk::DeviceSize m_wideP0Offset{0};
  vk::DeviceSize m_wideP1Offset{0};
  vk::DeviceSize m_wideC0Offset{0};
  vk::DeviceSize m_wideC1Offset{0};
  vk::DeviceSize m_wideFlagsOffset{0};
  vk::DeviceSize m_wideUploadIndexOffset{0};
  uint32_t m_wideUploadIndexCount{0};
  // Separate index buffer handle for wide path (arena uses same buffer; static uses IB buffer)
  vk::Buffer m_wideIndexBuffer{};

  // Static promotion caches
  struct ThinGeometryCacheKey
  {
    uint64_t streamKey = 0;
    bool lineStrip = false;
    auto tie() const
    {
      return std::tuple(streamKey, lineStrip);
    }
    bool operator<(const ThinGeometryCacheKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };
  struct ThinGeometryCacheEntry
  {
    Z3DRendererVulkanBackend::StaticSlice vbPos{};
    Z3DRendererVulkanBackend::StaticSlice ib{};
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t positionsGen = 0;
    uint32_t indexGen = 0;
    int unchangedFrames = 0;
    bool promoted = false;
    bool usedStaticOnce = false;
  };
  struct ThinAppearanceCacheKey
  {
    uint64_t streamKey = 0;
    bool picking = false;
    bool lineStrip = false;
    auto tie() const
    {
      return std::tuple(streamKey, picking, lineStrip);
    }
    bool operator<(const ThinAppearanceCacheKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };
  struct ThinAppearanceCacheEntry
  {
    Z3DRendererVulkanBackend::StaticSlice vbColor{};
    uint32_t vertexCount = 0;
    uint32_t colorsGen = 0;
    int unchangedFrames = 0;
    bool promoted = false;
    bool usedStaticOnce = false;
  };
  struct ThinPendingGeometryUploadBinding
  {
    Z3DRendererVulkanBackend::UploadSlice pos{};
    Z3DRendererVulkanBackend::UploadSlice index{};
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t positionsGen = 0;
    uint32_t indexGen = 0;
  };
  struct ThinPendingAppearanceUploadBinding
  {
    Z3DRendererVulkanBackend::UploadSlice color{};
    uint32_t vertexCount = 0;
    uint32_t colorsGen = 0;
  };
  std::map<ThinGeometryCacheKey, ThinGeometryCacheEntry> m_thinGeometryStaticCache;
  std::map<ThinAppearanceCacheKey, ThinAppearanceCacheEntry> m_thinAppearanceStaticCache;
  std::set<ThinGeometryCacheKey> m_thinGeometryStaticCopyPendingKeys;
  std::set<ThinAppearanceCacheKey> m_thinAppearanceStaticCopyPendingKeys;
  std::map<ThinGeometryCacheKey, ThinPendingGeometryUploadBinding> m_thinGeometryStaticCopyPendingUploads;
  std::map<ThinAppearanceCacheKey, ThinPendingAppearanceUploadBinding> m_thinAppearanceStaticCopyPendingUploads;

  struct WideGeometryCacheKey
  {
    uint64_t streamKey = 0;
    auto tie() const
    {
      return std::tuple(streamKey);
    }
    bool operator<(const WideGeometryCacheKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };
  struct WideGeometryCacheEntry
  {
    Z3DRendererVulkanBackend::StaticSlice vbP0{};
    Z3DRendererVulkanBackend::StaticSlice vbP1{};
    Z3DRendererVulkanBackend::StaticSlice vbFlags{};
    Z3DRendererVulkanBackend::StaticSlice ib{};
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t p0Gen = 0;
    uint32_t p1Gen = 0;
    uint32_t flagsGen = 0;
    uint32_t indexGen = 0;
    int unchangedFrames = 0;
    bool promoted = false;
    bool usedStaticOnce = false;
  };
  struct WideAppearanceCacheKey
  {
    uint64_t streamKey = 0;
    bool picking = false;
    auto tie() const
    {
      return std::tuple(streamKey, picking);
    }
    bool operator<(const WideAppearanceCacheKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };
  struct WideAppearanceCacheEntry
  {
    Z3DRendererVulkanBackend::StaticSlice vbC0{};
    Z3DRendererVulkanBackend::StaticSlice vbC1{};
    uint32_t vertexCount = 0;
    uint32_t c0Gen = 0;
    uint32_t c1Gen = 0;
    uint32_t pickGen = 0;
    int unchangedFrames = 0;
    bool promoted = false;
    bool usedStaticOnce = false;
  };
  struct WidePendingGeometryUploadBinding
  {
    Z3DRendererVulkanBackend::UploadSlice p0{};
    Z3DRendererVulkanBackend::UploadSlice p1{};
    Z3DRendererVulkanBackend::UploadSlice flags{};
    Z3DRendererVulkanBackend::UploadSlice index{};
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t p0Gen = 0;
    uint32_t p1Gen = 0;
    uint32_t flagsGen = 0;
    uint32_t indexGen = 0;
  };
  struct WidePendingAppearanceUploadBinding
  {
    Z3DRendererVulkanBackend::UploadSlice c0{};
    Z3DRendererVulkanBackend::UploadSlice c1{};
    uint32_t vertexCount = 0;
    uint32_t c0Gen = 0;
    uint32_t c1Gen = 0;
    uint32_t pickGen = 0;
  };
  std::map<WideGeometryCacheKey, WideGeometryCacheEntry> m_wideGeometryStaticCache;
  std::map<WideAppearanceCacheKey, WideAppearanceCacheEntry> m_wideAppearanceStaticCache;
  std::set<WideGeometryCacheKey> m_wideGeometryStaticCopyPendingKeys;
  std::set<WideAppearanceCacheKey> m_wideAppearanceStaticCopyPendingKeys;
  std::map<WideGeometryCacheKey, WidePendingGeometryUploadBinding> m_wideGeometryStaticCopyPendingUploads;
  std::map<WideAppearanceCacheKey, WidePendingAppearanceUploadBinding> m_wideAppearanceStaticCopyPendingUploads;

  // Cached per-draw secondary command buffers (steady-state optimization).
  struct ThinSecondaryCacheKey
  {
    void* frameKey = nullptr;
    uint64_t streamKey = 0;
    bool picking = false;
    bool lineStrip = false;
    Z3DRendererBase::ShaderHookType shaderHookType = Z3DRendererBase::ShaderHookType::Normal;
    Z3DEye eye = MonoEye;
    bool hasOit = false;
    uint32_t oitRingIndex = 0;

    bool operator==(const ThinSecondaryCacheKey& rhs) const
    {
      return frameKey == rhs.frameKey && streamKey == rhs.streamKey && picking == rhs.picking &&
             lineStrip == rhs.lineStrip && shaderHookType == rhs.shaderHookType && eye == rhs.eye &&
             hasOit == rhs.hasOit && oitRingIndex == rhs.oitRingIndex;
    }
  };

  struct ThinSecondaryCacheKeyHash
  {
    size_t operator()(const ThinSecondaryCacheKey& key) const noexcept
    {
      size_t h = std::hash<uintptr_t>{}(reinterpret_cast<uintptr_t>(key.frameKey));
      h ^= std::hash<uint64_t>{}(key.streamKey) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.picking)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.lineStrip)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<int>{}(static_cast<int>(key.shaderHookType)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<int>{}(static_cast<int>(key.eye)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.hasOit)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      h ^= std::hash<uint32_t>{}(key.oitRingIndex) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
      return h;
    }
  };

  struct ThinSecondarySignature
  {
    vk::Pipeline pipeline{};
    vk::PipelineLayout layout{};
    std::array<vk::DescriptorSet, 3> baseDescriptorSets{};
    std::array<uint64_t, 3> baseDescriptorGenerations{};
    bool hasOit = false;
    vk::DescriptorSet oitDescriptorSet{};
    uint64_t oitDescriptorGeneration = 0;
    std::array<uint32_t, 4> dynamicOffsets{};
    std::array<vk::Buffer, 2> vertexBuffers{};
    std::array<vk::DeviceSize, 2> vertexOffsets{};
    std::array<uint64_t, 2> vertexBufferSegmentIds{};
    vk::Buffer indexBuffer{};
    vk::DeviceSize indexOffset{0};
    uint64_t indexBufferSegmentId = 0;
    vk::IndexType indexType{vk::IndexType::eUint32};
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    vk::Viewport viewport{};
    vk::Rect2D scissor{};

    bool operator==(const ThinSecondarySignature& rhs) const
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
             indexCount == rhs.indexCount && vertexCount == rhs.vertexCount &&
             indexBufferSegmentId == rhs.indexBufferSegmentId && viewportEq && scissorEq;
    }
  };

  struct ThinSecondaryCacheEntry
  {
    ThinSecondarySignature signature{};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    bool recorded = false;
  };

  std::unordered_map<ThinSecondaryCacheKey, ThinSecondaryCacheEntry, ThinSecondaryCacheKeyHash> m_thinSecondaryCache;

  // UBO lifetime guard: retain previous frame UBOs until the active submission
  // fence signals to avoid read-after-free glitches. We collect them here in
  // resetFrame() and hand them to the backend at the first record() call.
  std::vector<std::shared_ptr<ZVulkanBuffer>> m_retainedUbos;
  void retainUbo(std::unique_ptr<ZVulkanBuffer>& ubo)
  {
    if (ubo) {
      m_retainedUbos.emplace_back(std::shared_ptr<ZVulkanBuffer>(std::move(ubo)));
    }
  }
  void flushRetainedUbos();

  // Dynamic UBO offsets (per-draw)
  vk::DeviceSize m_dynLightingOffset{0};
  vk::DeviceSize m_dynFrameTransformsOffset{0};
  vk::DeviceSize m_dynObjectTransformsOffset{0};
  vk::DeviceSize m_dynMaterialOffset{0};
  // Cache per-stream dynamic UBO offsets in the persistent uniform arena so
  // command buffers can be reused across frames (steady-state).
  struct UboCacheEntry
  {
    bool pickingPass = false;
    RendererParameterState params{};
    bool followCoordTransform = true;
    bool followSizeScale = true;
    bool followOpacity = true;
    ClipPlanesState clipPlanes{};
    vk::DeviceSize objectTransformsOffset = 0;
    vk::DeviceSize materialOffset = 0;
  };
  struct FrameUboCache
  {
    std::unordered_map<uint64_t, std::vector<UboCacheEntry>> byStream;
  };
  std::unordered_map<void*, FrameUboCache> m_uboCacheByFrameKey;
  // DDP indirect-count gating requires stable per-stream indirect args prepared
  // during the init pass; peel passes must not schedule upload->device copies
  // inside dynamic rendering (copies are flushed after segments end).
  struct DDPArgs
  {
    bool widePrepared = false;
    bool widePerSegment = false;
    std::vector<vk::DeviceSize> wideOffsets;
    uint32_t wideSegments = 0;
    uint32_t wideIndexCount = 0;

    bool thinPrepared = false;
    bool thinIndexed = false;
    vk::DeviceSize thinOffset = 0;
    uint32_t thinVertexCount = 0;
    uint32_t thinIndexCount = 0;
  };
  std::unordered_map<uint64_t, DDPArgs> m_ddpArgsByStream;

  void updateUBOs(Z3DRendererBase& renderer, const RenderBatch& batch, const LinePayload& payload);
  PipelineInstance&
  ensurePipeline(const PipelineKey& key, const LinePayload& payload, const vulkan::AttachmentFormats& formats);
  void uploadWideGeometry(const LinePayload& payload, bool pickingPass);
  void uploadThinGeometry(const LinePayload& payload, bool pickingPass);
};

} // namespace nim
