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

class ZVulkanSpherePipelineContext
{
public:
  explicit ZVulkanSpherePipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanSpherePipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const SpherePayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  friend class Z3DRendererVulkanBackend; // allow backend to prime descriptor sets
  struct SphereVertex
  {
    glm::vec4 centerRadius{0.0f};
    glm::vec4 color{0.0f};
    float flags = 0.0f;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
    float pad2 = 0.0f;
  };

  struct PipelineKey
  {
    bool dynamicMaterial = true;
    Z3DRendererBase::ShaderHookType shaderHookType = Z3DRendererBase::ShaderHookType::Normal;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(dynamicMaterial, static_cast<int>(shaderHookType), colorFormats, depthFormat);
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

  void ensureDescriptorLayouts();
  void resetDescriptors();
  void ensureDescriptorSets();
  void ensureOITResources();
  void updateOITParamsUBO(Z3DRendererBase& renderer, const RenderBatch& batch, const glm::vec2& screenDimRcp);
  void ensurePlaceholderTexture();
  void updateLightingUBO(Z3DRendererBase& renderer,
                         const RenderBatch& batch,
                         const SpherePayload& payload,
                         bool pickingPass);
  void updateTransformUBO(Z3DRendererBase& renderer,
                          const RenderBatch& batch,
                          const SpherePayload& payload,
                          bool pickingPass);
  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  void uploadGeometry(const SpherePayload& payload);

  // Static promotion cache (SoA)
  struct CacheKey
  {
    uint64_t streamKey = 0; // stable identity of source stream/renderer
    bool picking = false;
    bool dynamicMaterial = true;
    auto tie() const
    {
      return std::tuple(streamKey, picking, dynamicMaterial);
    }
    bool operator<(const CacheKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };
  struct CacheEntry
  {
    // Separate static buffers for each attribute stream (SoA)
    vk::Buffer vbCenterRadius = VK_NULL_HANDLE;
    vk::Buffer vbColor = VK_NULL_HANDLE;
    vk::Buffer vbSpecular = VK_NULL_HANDLE;
    vk::Buffer vbFlags = VK_NULL_HANDLE;
    vk::DeviceSize centerRadiusOffset = 0;
    vk::DeviceSize colorOffset = 0;
    vk::DeviceSize specularOffset = 0;
    vk::DeviceSize flagsOffset = 0;
    vk::Buffer ib = VK_NULL_HANDLE;
    vk::DeviceSize ibOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t centersGen = 0;
    uint32_t colorsGen = 0; // or picking
    uint32_t specularGen = 0;
    uint32_t flagsGen = 0;
    uint32_t indexGen = 0;
    int unchangedFrames = 0;
    bool promoted = false;
  };
  std::map<CacheKey, CacheEntry> m_staticCache;
};

} // namespace nim
