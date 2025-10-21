#pragma once

#include "z3drendercommands.h"
#include "z3drendererbase.h"
#include "zvulkan.h"

#include <memory>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

namespace nim {

namespace vulkan {
struct AttachmentFormats;
}

class Z3DRendererBase;
class Z3DRendererVulkanBackend;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanDescriptorPool;
class ZVulkanDescriptorSet;
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

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const LinePayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  friend class Z3DRendererVulkanBackend; // allow backend to prime descriptor sets
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

  std::optional<vk::raii::DescriptorSetLayout> m_setTexture;
  vk::DescriptorSetLayout m_setLighting{};
  vk::DescriptorSetLayout m_setTransforms{};
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTexture;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsLighting;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTransforms;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsOIT;

  std::unique_ptr<ZVulkanTexture> m_placeholderTexture;
  std::optional<vk::raii::Sampler> m_sampler;

  std::unique_ptr<ZVulkanBuffer> m_uboOIT;

  vk::DescriptorSetLayout m_setOIT{};

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
  struct ThinCacheKey
  {
    uint64_t streamKey = 0;
    bool picking = false;
    bool lineStrip = false;
    auto tie() const
    {
      return std::tuple(streamKey, picking, lineStrip);
    }
    bool operator<(const ThinCacheKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };
  struct ThinCacheEntry
  {
    // Separate static VBs for positions and colors
    vk::Buffer vbPos{};
    vk::Buffer vbColor{};
    vk::DeviceSize posOffset = 0;
    vk::DeviceSize colorOffset = 0;
    uint32_t vertexCount = 0;
    vk::Buffer ib{}; // only for line strip
    vk::DeviceSize ibOffset = 0;
    uint32_t indexCount = 0;
    uint32_t positionsGen = 0;
    uint32_t colorsGen = 0; // picking or regular colors depending on pass
    uint32_t indexGen = 0;
    int unchangedFrames = 0;
    bool promoted = false;
  };
  std::map<ThinCacheKey, ThinCacheEntry> m_thinStaticCache;

  struct WideCacheKey
  {
    uint64_t streamKey = 0;
    bool picking = false;
    auto tie() const
    {
      return std::tuple(streamKey, picking);
    }
    bool operator<(const WideCacheKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };
  struct WideCacheEntry
  {
    // Static VBs per attribute (SoA)
    vk::Buffer vbP0{};
    vk::Buffer vbP1{};
    vk::Buffer vbC0{};
    vk::Buffer vbC1{};
    vk::Buffer vbFlags{};
    vk::DeviceSize p0Offset = 0;
    vk::DeviceSize p1Offset = 0;
    vk::DeviceSize c0Offset = 0;
    vk::DeviceSize c1Offset = 0;
    vk::DeviceSize flagsOffset = 0;
    vk::Buffer ib{};
    vk::DeviceSize ibOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t p0Gen = 0;
    uint32_t p1Gen = 0;
    uint32_t c0Gen = 0;
    uint32_t c1Gen = 0;
    uint32_t pickGen = 0;
    uint32_t flagsGen = 0;
    uint32_t indexGen = 0;
    int unchangedFrames = 0;
    bool promoted = false;
  };
  std::map<WideCacheKey, WideCacheEntry> m_wideStaticCache;

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
  vk::DeviceSize m_dynTransformsOffset{0};
  vk::DeviceSize m_dynMaterialOffset{0};

  void ensureDescriptorLayouts();
  void ensurePlaceholderTexture();
  void ensureDescriptorSets(Z3DRendererBase& renderer);
  void updateUBOs(Z3DRendererBase& renderer, const RenderBatch& batch, const LinePayload& payload);
  PipelineInstance&
  ensurePipeline(const PipelineKey& key, const LinePayload& payload, const vulkan::AttachmentFormats& formats);
  void uploadWideGeometry(const LinePayload& payload, bool pickingPass);
  void uploadThinGeometry(const LinePayload& payload, bool pickingPass);
  void resetDescriptors();
};

} // namespace nim
