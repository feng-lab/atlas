#pragma once

#include "z3drendercommands.h"
#include "zvulkan.h"

#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

namespace nim {

class Z3DRendererBase;
class Z3DRendererVulkanBackend;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanDescriptorPool;
class ZVulkanDescriptorSet;
class ZVulkanTexture;
class ZVulkanBuffer;

namespace vulkan {
struct AttachmentFormats;
}

class ZVulkanTextureWeightedBlendedPipelineContext
{
public:
  explicit ZVulkanTextureWeightedBlendedPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanTextureWeightedBlendedPipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const TextureWeightedBlendedPayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  struct PipelineKey
  {
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(colorFormats, depthFormat);
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

  struct WeightedBlendedPushConstants
  {
    glm::vec2 screenDimRcp{1.0f};
    glm::vec2 _pad{0.0f};
  };

  Z3DRendererVulkanBackend& m_backend;

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  std::optional<vk::raii::DescriptorSetLayout> m_setLayout;
  std::unique_ptr<ZVulkanDescriptorPool> m_descriptorPool;
  std::unique_ptr<ZVulkanDescriptorSet> m_descriptorSet;

  std::unique_ptr<ZVulkanBuffer> m_vertexBuffer;
  size_t m_vertexCapacity = 0;
  size_t m_vertexCount = 0;

  void ensureDescriptorLayout();
  void ensureDescriptorPool();
  void ensureDescriptorSet();
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;
  void ensureVertexCapacity(size_t vertexCount);
  void uploadGeometry();

  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
};

} // namespace nim

