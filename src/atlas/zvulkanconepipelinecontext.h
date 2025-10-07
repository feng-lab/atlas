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
class Z3DConeRenderer;

class ZVulkanConePipelineContext
{
public:
  explicit ZVulkanConePipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanConePipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const ConePayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  friend class Z3DRendererVulkanBackend; // allow backend to prime descriptor sets
  struct ConeVertex
  {
    glm::vec4 origin{0.0f};
    glm::vec4 axis{0.0f};
    glm::vec4 colorBase{0.0f};
    glm::vec4 colorTop{0.0f};
    float flags = 0.0f;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
    float pad2 = 0.0f;
  };

  struct PipelineKey
  {
    bool dynamicMaterial = true;
    int capsMode = 1;
    Z3DRendererBase::ShaderHookType shaderHookType = Z3DRendererBase::ShaderHookType::Normal;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(dynamicMaterial,
                        capsMode,
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
  // Upload arena-backed SoA slices
  vk::Buffer m_vbBuffer{VK_NULL_HANDLE};
  vk::DeviceSize m_originOffset{0};
  vk::DeviceSize m_axisOffset{0};
  vk::DeviceSize m_flagsOffset{0};
  vk::DeviceSize m_baseColorOffset{0};
  vk::DeviceSize m_topColorOffset{0};
  vk::Buffer m_indexUploadBuffer{VK_NULL_HANDLE};
  vk::DeviceSize m_indexUploadOffset{0};

  // Static promotion cache
  struct CacheKey
  {
    Z3DConeRenderer* renderer = nullptr;
    bool picking = false;
    auto tie() const { return std::tuple(renderer, picking); }
    bool operator<(const CacheKey& rhs) const { return tie() < rhs.tie(); }
  };
  struct CacheEntry
  {
    vk::Buffer vb = VK_NULL_HANDLE;
    vk::DeviceSize originOffset = 0;
    vk::DeviceSize axisOffset = 0;
    vk::DeviceSize flagsOffset = 0;
    vk::DeviceSize baseColorOffset = 0;
    vk::DeviceSize topColorOffset = 0;
    vk::Buffer ib = VK_NULL_HANDLE;
    vk::DeviceSize ibOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    // Last observed gens
    uint32_t baseGen = 0, axisGen = 0, baseColorGen = 0, topColorGen = 0, pickingColorsGen = 0, flagsGen = 0, indexGen = 0;
    int unchangedFrames = 0;
    bool promoted = false;
  };
  std::map<CacheKey, CacheEntry> m_staticCache;

  void ensureDescriptorLayouts();
  void resetDescriptors();
  void ensureDescriptorSets();
  void ensureOITResources();
  void updateOITParamsUBO(Z3DRendererBase& renderer, const RenderBatch& batch,
                          const glm::vec2& screenDimRcp);
  void ensurePlaceholderTexture();
  void updateLightingUBO(Z3DRendererBase& renderer,
                         const RenderBatch& batch,
                         const ConePayload& payload,
                         bool pickingPass);
  void updateTransformUBO(Z3DRendererBase& renderer,
                          const RenderBatch& batch,
                          const ConePayload& payload,
                          bool pickingPass);
  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  void uploadGeometry(const ConePayload& payload);
};

} // namespace nim
