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
  std::optional<vk::raii::DescriptorSetLayout> m_setLighting;
  std::optional<vk::raii::DescriptorSetLayout> m_setTransforms;
  std::unique_ptr<ZVulkanDescriptorPool> m_descriptorPool;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTexture;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsLighting;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTransforms;

  std::unique_ptr<ZVulkanTexture> m_placeholderTexture;
  std::optional<vk::raii::Sampler> m_sampler;

  std::unique_ptr<ZVulkanBuffer> m_uboLighting;
  std::unique_ptr<ZVulkanBuffer> m_uboTransforms;
  std::unique_ptr<ZVulkanBuffer> m_uboMaterial;

  // All line geometry uses the per-frame upload arena; no per-context VBOs

  // Upload arena-backed slice for thin line vertices (per-draw)
  vk::Buffer m_thinUploadBuffer{VK_NULL_HANDLE};
  vk::DeviceSize m_thinUploadOffset{0};
  uint32_t m_thinUploadVertexCount{0};
  vk::Buffer m_thinUploadIndexBuffer{VK_NULL_HANDLE};
  vk::DeviceSize m_thinUploadIndexOffset{0};
  uint32_t m_thinUploadIndexCount{0};

  // Upload arena-backed slices for wide line vertices/indices (per-draw)
  vk::Buffer m_wideUploadBuffer{VK_NULL_HANDLE};
  vk::DeviceSize m_wideUploadVertexOffset{0};
  vk::DeviceSize m_wideUploadIndexOffset{0};
  uint32_t m_wideUploadIndexCount{0};

  void ensureDescriptorLayouts();
  void ensurePlaceholderTexture();
  void ensureDescriptorSets(Z3DRendererBase& renderer);
  void updateUBOs(Z3DRendererBase& renderer, const RenderBatch& batch);
  PipelineInstance& ensurePipeline(const PipelineKey& key,
                                   const LinePayload& payload,
                                   const vulkan::AttachmentFormats& formats);
  void bindDescriptorSets(vk::raii::CommandBuffer& cmd,
                          const PipelineInstance& pipeline,
                          vk::DescriptorSet textureOverride = {}) const;
  void uploadWideGeometry(const LinePayload& payload, bool pickingPass);
  void uploadThinGeometry(const LinePayload& payload, bool pickingPass);
  void resetDescriptors();
};

} // namespace nim
