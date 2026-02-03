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
  void evictStream(uint64_t streamKey);

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
    bool useConeShader2 = false;
    int capsMode = 1;
    Z3DRendererBase::ShaderHookType shaderHookType = Z3DRendererBase::ShaderHookType::Normal;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(dynamicMaterial,
                        useConeShader2,
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
  // Upload arena-backed SoA slices
  vk::Buffer m_originBuffer{};
  vk::Buffer m_axisBuffer{};
  vk::Buffer m_flagsBuffer{};
  vk::Buffer m_baseColorBuffer{};
  vk::Buffer m_topColorBuffer{VK_NULL_HANDLE};
  vk::DeviceSize m_originOffset{0};
  vk::DeviceSize m_axisOffset{0};
  vk::DeviceSize m_flagsOffset{0};
  vk::DeviceSize m_baseColorOffset{0};
  vk::DeviceSize m_topColorOffset{0};
  vk::Buffer m_indexUploadBuffer{};
  vk::DeviceSize m_indexUploadOffset{0};

  // UBO retention until the active submission fence to prevent flicker when
  // frames overlap on the GPU.
  std::vector<std::shared_ptr<ZVulkanBuffer>> m_retainedUbos;
  void retainUbo(std::unique_ptr<ZVulkanBuffer>& ubo)
  {
    if (ubo) {
      m_retainedUbos.emplace_back(std::shared_ptr<ZVulkanBuffer>(std::move(ubo)));
    }
  }
  void flushRetainedUbos();

  // Dynamic UBO offsets for this draw
  vk::DeviceSize m_dynLightingOffset{0};
  vk::DeviceSize m_dynTransformsOffset{0};
  vk::DeviceSize m_dynMaterialOffset{0};
  // Freeze dynamic UBOs during DDP passes to avoid per-pass allocations
  bool m_ddpTransformsFrozen{false};
  bool m_ddpMaterialFrozen{false};
  // Device-local indirect args; prepared during DDP init
  bool m_ddpArgsPrepared{false};
  vk::DeviceSize m_ddpArgsOffset{0};

  // Static promotion cache
  struct CacheKey
  {
    uint64_t streamKey = 0;
    bool picking = false;
    auto tie() const
    {
      return std::tuple(streamKey, picking);
    }
    bool operator<(const CacheKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };
  struct CacheEntry
  {
    // Separate static buffers for each attribute stream
    Z3DRendererVulkanBackend::StaticSlice vbOrigin{};
    Z3DRendererVulkanBackend::StaticSlice vbAxis{};
    Z3DRendererVulkanBackend::StaticSlice vbFlags{};
    Z3DRendererVulkanBackend::StaticSlice vbBaseColor{};
    Z3DRendererVulkanBackend::StaticSlice vbTopColor{};
    Z3DRendererVulkanBackend::StaticSlice ib{};
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    // Last observed gens
    uint32_t baseGen = 0, axisGen = 0, baseColorGen = 0, topColorGen = 0, pickingColorsGen = 0, flagsGen = 0,
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
  void
  updateTransformUBO(Z3DRendererBase& renderer, const RenderBatch& batch, const ConePayload& payload, bool pickingPass);
  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  void uploadGeometry(const ConePayload& payload);
};

} // namespace nim
