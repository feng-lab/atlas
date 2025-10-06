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

class ZVulkanTextureDualPeelPipelineContext
{
public:
  explicit ZVulkanTextureDualPeelPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanTextureDualPeelPipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const TextureDualPeelPayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  friend class Z3DRendererVulkanBackend; // allow backend to pre-prime OIT resources
  enum class Stage
  {
    Blend,
    Final
  };

  struct PipelineKey
  {
    Stage stage = Stage::Blend;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(static_cast<int>(stage), colorFormats, depthFormat);
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
    Stage stage = Stage::Blend;
  };

  struct DualPeelPushConstants
  {
    glm::vec2 screenDimRcp{1.0f};
    glm::vec2 _pad{0.0f};
  };

  Z3DRendererVulkanBackend& m_backend;

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  std::optional<vk::raii::DescriptorSetLayout> m_blendSetLayout;
  std::optional<vk::raii::DescriptorSetLayout> m_finalSetLayout;
  std::optional<vk::raii::DescriptorSetLayout> m_setPlaceholder; // for set 1/2 alignment
  std::optional<vk::raii::DescriptorSetLayout> m_setOIT;         // set = 3 OIT params
  std::unique_ptr<ZVulkanDescriptorPool> m_descriptorPool;
  std::unique_ptr<ZVulkanDescriptorSet> m_blendDescriptor;
  std::unique_ptr<ZVulkanDescriptorSet> m_finalDescriptor;
  std::unique_ptr<ZVulkanDescriptorSet> m_descriptorOIT;
  std::unique_ptr<ZVulkanBuffer> m_uboOIT;

  std::unique_ptr<ZVulkanBuffer> m_vertexBuffer;
  size_t m_vertexCapacity = 0;
  size_t m_vertexCount = 0;

  void ensureDescriptorLayouts();
  void ensureDescriptorPool();
  void resetDescriptors();
  ZVulkanDescriptorSet* ensureDescriptor(Stage stage);
  void ensureOITResources();
  void updateOITParamsUBO(Z3DRendererBase& renderer, const RenderBatch& batch,
                          const glm::vec2& fallbackScreenDimRcp);
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;
  void ensureVertexCapacity(size_t vertexCount);
  void uploadGeometry();

  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
};

} // namespace nim
