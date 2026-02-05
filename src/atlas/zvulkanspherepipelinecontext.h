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

class ZVulkanSpherePipelineContext
{
public:
  explicit ZVulkanSpherePipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanSpherePipelineContext();

  void resetFrame();
  void evictStream(uint64_t streamKey);

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

  vk::DescriptorSetLayout m_setPlaceholder{};
  vk::DescriptorSetLayout m_setLighting{};
  vk::DescriptorSetLayout m_setTransforms{};
  vk::DescriptorSetLayout m_setOIT{}; // set = 3 (DDP flag only)
  std::unique_ptr<ZVulkanDescriptorSet> m_dsPlaceholder;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsLighting;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTransforms;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsOIT;

  std::unique_ptr<ZVulkanTexture> m_placeholderTexture;
  std::optional<vk::raii::Sampler> m_sampler;

  // No OIT UBO retained; only DDP flag SSBO remains on set 3

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

  // UBO retention across frames: keep last-frame UBOs alive until the active
  // submission fence signals to prevent transient flicker when frames overlap.
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
    Z3DEye eye = MonoEye;
    ClipPlanesState clipPlanes;
    vk::DeviceSize transformsOffset = 0;
    vk::DeviceSize materialOffset = 0;
  };
  std::unordered_map<uint64_t, std::vector<DDPUboCacheEntry>> m_ddpUboCache;
  // Device-local indirect args prepared during DDP init
  bool m_ddpArgsPrepared{false};
  vk::DeviceSize m_ddpArgsOffset{0};

  void ensureDescriptorLayouts();
  void resetDescriptors();
  void ensureDescriptorSets();
  void ensureOITResources();
  void ensurePlaceholderTexture();
  // Lighting UBO is shared per-frame; no per-batch update is needed.
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
    Z3DRendererVulkanBackend::StaticSlice vbCenterRadius{};
    Z3DRendererVulkanBackend::StaticSlice vbColor{};
    Z3DRendererVulkanBackend::StaticSlice vbSpecular{};
    Z3DRendererVulkanBackend::StaticSlice vbFlags{};
    Z3DRendererVulkanBackend::StaticSlice ib{};
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
  // Guard: if we scheduled upload->static copies for a stream within the
  // current submission, we must not bind the static buffers again until the
  // next submission because copies are flushed after rendering ends.
  std::set<CacheKey> m_staticCopyPendingKeys;
};

} // namespace nim
