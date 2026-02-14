#pragma once

#include "z3drendercommands.h"
#include "z3drendererstates.h"
#include "z3drendererbase.h"
#include "z3drenderervulkanbackend.h"
#include "zvulkan.h"

#include <map>
#include <memory>
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
class ZVulkanDescriptorPool;
class ZVulkanDescriptorSet;
class ZVulkanBuffer;
class Z3DEllipsoidRenderer;

class ZVulkanEllipsoidPipelineContext
{
public:
  explicit ZVulkanEllipsoidPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanEllipsoidPipelineContext();

  void resetFrame();
  void evictStream(uint64_t streamKey);

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

  vk::DescriptorSetLayout m_setPlaceholder{};
  vk::DescriptorSetLayout m_setLighting{};
  vk::DescriptorSetLayout m_setTransforms{};
  vk::DescriptorSetLayout m_setOIT{}; // set = 3
  std::unique_ptr<ZVulkanDescriptorSet> m_dsPlaceholder;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsLighting;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTransforms;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsOIT;

  std::unique_ptr<ZVulkanTexture> m_placeholderTexture;
  std::optional<vk::raii::Sampler> m_sampler;

  // No OIT UBO retained; set 3 only carries DDP flag SSBO

  size_t m_vertexCount = 0;
  size_t m_indexCount = 0;
  // Upload arena-backed SoA slices (per-attribute buffers)
  vk::Buffer m_axis1Buffer{VK_NULL_HANDLE};
  vk::Buffer m_axis2Buffer{VK_NULL_HANDLE};
  vk::Buffer m_axis3Buffer{VK_NULL_HANDLE};
  vk::Buffer m_centerBuffer{VK_NULL_HANDLE};
  vk::Buffer m_colorBuffer{VK_NULL_HANDLE};
  vk::Buffer m_flagsBuffer{VK_NULL_HANDLE};
  vk::Buffer m_specularBuffer{VK_NULL_HANDLE};
  vk::DeviceSize m_axis1Offset{0};
  vk::DeviceSize m_axis2Offset{0};
  vk::DeviceSize m_axis3Offset{0};
  vk::DeviceSize m_centerOffset{0};
  vk::DeviceSize m_colorOffset{0};
  vk::DeviceSize m_flagsOffset{0};
  vk::DeviceSize m_specularOffset{0};
  vk::Buffer m_indexUploadBuffer{VK_NULL_HANDLE};
  vk::DeviceSize m_indexUploadOffset{0};

  // Dynamic UBO offsets for this draw
  vk::DeviceSize m_dynLightingOffset{0};
  vk::DeviceSize m_dynFrameTransformsOffset{0};
  vk::DeviceSize m_dynObjectTransformsOffset{0};
  vk::DeviceSize m_dynMaterialOffset{0};
  // Device-local indirect args for DDP
  bool m_ddpArgsPrepared{false};
  vk::DeviceSize m_ddpArgsOffset{0};
  // DDP (Dual Depth Peeling) can replay the same draw list across multiple peel
  // passes inside a single Vulkan submission (ddpOrchestrate). Cache per-stream
  // dynamic UBO offsets so we do not re-suballocate uniforms for each peel pass,
  // while still allowing distinct streams/clip-plane states to bind correct data.
  struct DDPUboCacheEntry
  {
    RendererParameterState params{};
    bool followCoordTransform = true;
    bool followSizeScale = true;
    bool followOpacity = true;
    bool pickingPass = false;
    ClipPlanesState clipPlanes;
    vk::DeviceSize objectTransformsOffset = 0;
    vk::DeviceSize materialOffset = 0;
  };
  std::unordered_map<uint64_t, std::vector<DDPUboCacheEntry>> m_ddpUboCache;

  // Static promotion cache
  struct CacheKey
  {
    uint64_t streamKey = 0;
    bool picking = false;
    bool dynamicMaterial = false;
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
    // Separate static buffers per attribute stream
    Z3DRendererVulkanBackend::StaticSlice vbAxis1{};
    Z3DRendererVulkanBackend::StaticSlice vbAxis2{};
    Z3DRendererVulkanBackend::StaticSlice vbAxis3{};
    Z3DRendererVulkanBackend::StaticSlice vbCenter{};
    Z3DRendererVulkanBackend::StaticSlice vbColor{};
    Z3DRendererVulkanBackend::StaticSlice vbFlags{};
    Z3DRendererVulkanBackend::StaticSlice vbSpecular{};
    Z3DRendererVulkanBackend::StaticSlice ib{};
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    // Last observed gens
    uint32_t centersGen = 0, axesGen = 0, colorsGen = 0, pickingColorsGen = 0, specularGen = 0, flagsGen = 0,
             indexGen = 0;
    int unchangedFrames = 0;
    bool promoted = false;
  };
  std::map<CacheKey, CacheEntry> m_staticCache;
  // Guard: if we scheduled upload->static copies for a stream within the
  // current submission, we must not bind the static buffers again until the
  // next submission because copies are flushed after rendering ends.
  std::set<CacheKey> m_staticCopyPendingKeys;

  void ensureDescriptorLayouts();
  void resetDescriptors();
  void ensureDescriptorSets();
  void ensureOITResources();
  void ensurePlaceholderTexture();
  // Lighting UBO is shared per-frame; no per-batch update is needed.
  void updateTransformUBO(Z3DRendererBase& renderer,
                          const RenderBatch& batch,
                          const EllipsoidPayload& payload,
                          bool pickingPass);
  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  void uploadGeometry(const EllipsoidPayload& payload);
};

} // namespace nim
