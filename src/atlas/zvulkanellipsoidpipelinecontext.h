#pragma once

#include "z3drendercommands.h"
#include "z3drendererstates.h"
#include "z3drendererbase.h"
#include "zvulkan.h"

#include <map>
#include <memory>
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
class ZVulkanBuffer;

class ZVulkanEllipsoidPipelineContext
{
public:
  explicit ZVulkanEllipsoidPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanEllipsoidPipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const EllipsoidPayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  friend class Z3DRendererVulkanBackend; // allow backend to prime descriptor sets
  struct EllipsoidVertex
  {
    glm::vec4 axis1{0.0f};
    glm::vec4 axis2{0.0f};
    glm::vec4 axis3{0.0f};
    glm::vec4 center{0.0f};
    glm::vec4 color{0.0f};
    float flags = 0.0f;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
    float pad2 = 0.0f;
    glm::vec4 specularShininess{0.0f};
  };

  struct PipelineKey
  {
    bool dynamicMaterial = false;
    FogMode fogMode = FogMode::None;
    Z3DRendererBase::ShaderHookType shaderHookType = Z3DRendererBase::ShaderHookType::Normal;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(dynamicMaterial,
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

  Z3DRendererVulkanBackend& m_backend;

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  std::optional<vk::raii::DescriptorSetLayout> m_setPlaceholder;
  std::optional<vk::raii::DescriptorSetLayout> m_setLighting;
  std::optional<vk::raii::DescriptorSetLayout> m_setTransforms;
  std::optional<vk::raii::DescriptorSetLayout> m_setOIT; // set = 3
  std::unique_ptr<ZVulkanDescriptorPool> m_descriptorPool;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsPlaceholder;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsLighting;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTransforms;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsOIT;

  std::unique_ptr<ZVulkanTexture> m_placeholderTexture;
  std::optional<vk::raii::Sampler> m_sampler;

  std::unique_ptr<ZVulkanBuffer> m_uboLighting;
  std::unique_ptr<ZVulkanBuffer> m_uboTransforms;
  std::unique_ptr<ZVulkanBuffer> m_uboMaterial;
  std::unique_ptr<ZVulkanBuffer> m_uboOIT;

  size_t m_vertexCount = 0;
  size_t m_indexCount = 0;
  // Upload arena-backed slices
  vk::Buffer m_vertexUploadBuffer{VK_NULL_HANDLE};
  vk::DeviceSize m_vertexUploadOffset{0};
  vk::Buffer m_indexUploadBuffer{VK_NULL_HANDLE};
  vk::DeviceSize m_indexUploadOffset{0};

  void ensureDescriptorLayouts();
  void resetDescriptors();
  void ensureDescriptorSets();
  void ensureOITResources();
  void updateOITParamsUBO(Z3DRendererBase& renderer, const RenderBatch& batch,
                          const glm::vec2& screenDimRcp);
  void ensurePlaceholderTexture();
  void updateLightingUBO(Z3DRendererBase& renderer,
                         const RenderBatch& batch,
                         const EllipsoidPayload& payload,
                         bool pickingPass);
  void updateTransformUBO(Z3DRendererBase& renderer,
                          const RenderBatch& batch,
                          const EllipsoidPayload& payload,
                          bool pickingPass);
  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  void uploadGeometry(const EllipsoidPayload& payload);
};

} // namespace nim
