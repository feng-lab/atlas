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

class ZVulkanTextureBlendPipelineContext
{
public:
  explicit ZVulkanTextureBlendPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanTextureBlendPipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const TextureBlendPayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  struct QuadVertex
  {
    glm::vec3 position{0.0f};
  };

  struct PipelineKey
  {
    TextureBlendMode mode = TextureBlendMode::DepthTestBlending;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(static_cast<int>(mode), colorFormats, depthFormat);
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

  struct TextureBlendPushConstants
  {
    glm::vec2 screenDimRcp{1.0f};
    glm::vec2 _pad{0.0f};
  };

  Z3DRendererVulkanBackend& m_backend;

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  std::optional<vk::raii::DescriptorSetLayout> m_setTextures;
  std::unique_ptr<ZVulkanDescriptorPool> m_descriptorPool;
  std::unique_ptr<ZVulkanDescriptorSet> m_descriptorSet;

  std::unique_ptr<ZVulkanBuffer> m_vertexBuffer;
  size_t m_vertexCapacity = 0;
  size_t m_vertexCount = 0;

  void ensureDescriptorLayout();
  void resetDescriptors();
  void ensureDescriptorSet();
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  void ensureVertexCapacity(size_t vertexCount);
  void uploadGeometry();

  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
};

} // namespace nim
