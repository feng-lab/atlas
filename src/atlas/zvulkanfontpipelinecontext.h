#pragma once

#include "z3drendercommands.h"
#include "zvulkan.h"

#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>
#include <unordered_map>

namespace nim {

class Z3DRendererBase;
class Z3DRendererVulkanBackend;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanDescriptorPool;
class ZVulkanDescriptorSet;
class ZVulkanTexture;
class ZVulkanBuffer;
class Z3DFontRenderer;

namespace vulkan {
struct AttachmentFormats;
}

class ZVulkanFontPipelineContext
{
public:
  explicit ZVulkanFontPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanFontPipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const FontPayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  struct FontVertex
  {
    glm::vec3 position{0.0f};
    glm::vec2 texcoord{0.0f};
    glm::vec4 color{1.0f};
  };

  struct PipelineKey
  {
    bool picking = false;
    bool showOutline = false;
    bool showShadow = false;
    int outlineMode = 0;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(picking, showOutline, showShadow, outlineMode, colorFormats, depthFormat);
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

  struct FontPushConstants
  {
    glm::mat4 mvp{1.0f};
    glm::vec4 outlineColor{1.0f};
    glm::vec4 shadowColor{0.0f, 0.0f, 0.0f, 1.0f};
    float softedgeScale = 80.0f;
    glm::vec3 _pad{0.0f};
    uint32_t flags = 0u; // bit0=picking, bit1=outline, bit2=shadow, bits8..=outlineMode
  };

  Z3DRendererVulkanBackend& m_backend;

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  std::optional<vk::raii::DescriptorSetLayout> m_setTexture;
  std::unique_ptr<ZVulkanDescriptorPool> m_descriptorPool;
  std::unique_ptr<ZVulkanDescriptorSet> m_descriptorSet;

  size_t m_vertexCount = 0;
  size_t m_indexCount = 0;
  // Upload arena-backed slices
  vk::Buffer m_vertexUploadBuffer{VK_NULL_HANDLE};
  vk::DeviceSize m_vertexUploadOffset{0};
  vk::Buffer m_indexUploadBuffer{VK_NULL_HANDLE};
  vk::DeviceSize m_indexUploadOffset{0};

  // Cache CPU-provided atlases keyed by pixel pointer
  std::unordered_map<const void*, std::unique_ptr<ZVulkanTexture>> m_atlasCache;

  // Static promotion cache for geometry
  struct CacheKey
  {
    Z3DFontRenderer* renderer = nullptr;
    bool picking = false;
    auto tie() const
    {
      return std::tuple(renderer, picking);
    }
    bool operator<(const CacheKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };
  struct CacheEntry
  {
    vk::Buffer vb = VK_NULL_HANDLE;
    vk::DeviceSize vbOffset = 0;
    vk::Buffer ib = VK_NULL_HANDLE;
    vk::DeviceSize ibOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t posGen = 0;
    uint32_t texGen = 0;
    uint32_t colorGen = 0; // or picking
    uint32_t indexGen = 0;
    int unchangedFrames = 0;
    bool promoted = false;
  };
  std::map<CacheKey, CacheEntry> m_staticCache;

  void ensureDescriptorLayout();
  void resetDescriptors();
  void ensureDescriptorSet();
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  void uploadGeometry(const FontPayload& payload);

  ZVulkanTexture* ensureAtlasFromPayload(const FontPayload& payload);

  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
};

} // namespace nim
