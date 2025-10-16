#pragma once

#include "z3drendercommands.h"
#include "z3drendererstates.h"
#include "z3drendererbase.h"
#include "zvulkan.h"
#include "zglmutils.h"

#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

namespace nim {

enum class FogMode;

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
class ZMesh;
class Z3DMeshRenderer;

class ZVulkanMeshPipelineContext
{
public:
  explicit ZVulkanMeshPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanMeshPipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const MeshPayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  friend class Z3DRendererVulkanBackend; // allow backend to prime descriptor sets
  struct PipelineKey
  {
    MeshPayload::ColorSource colorSource = MeshPayload::ColorSource::MeshColor;
    ZMesh::Type meshType;
    bool wireframe = false;
    FogMode fogMode = FogMode::None;
    Z3DRendererBase::ShaderHookType shaderHookType = Z3DRendererBase::ShaderHookType::Normal;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(static_cast<int>(colorSource),
                        static_cast<int>(meshType),
                        wireframe,
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

  struct MeshDraw
  {
    ZMesh* mesh = nullptr;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    bool indexed = false;
    size_t payloadMeshIndex = 0;
    bool useFallbackColor = false;
    glm::vec4 fallbackColor{1.0f};
  };

  struct TextureBinding
  {
    ZVulkanTexture* texture = nullptr;
    uint32_t descriptorBinding = 0;
  };

  Z3DRendererVulkanBackend& m_backend;

  // Static SoA promotion cache (device-local)
  struct CacheKey
  {
    uint64_t streamKey = 0;
    MeshPayload::ColorSource colorSource = MeshPayload::ColorSource::MeshColor;
    bool picking = false;
    auto tie() const
    {
      return std::tuple(streamKey, static_cast<int>(colorSource), picking);
    }
    bool operator<(const CacheKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };
  struct CacheEntry
  {
    // Device-local static buffers per attribute (SoA)
    vk::Buffer vbPos{};
    vk::Buffer vbNorm{};
    vk::Buffer vbColor{};
    vk::Buffer vbTex{}; // optional
    vk::DeviceSize posOffset = 0;
    vk::DeviceSize normOffset = 0;
    vk::DeviceSize colorOffset = 0;
    vk::DeviceSize texOffset = 0;
    bool hasTex = false;
    vk::Buffer ib{};
    vk::DeviceSize indexOffset = 0;

    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    // Last observed generation counters
    uint32_t posGen = 0, normGen = 0, colorGen = 0, texGen = 0, indexGen = 0;
    // Promotion
    int unchangedFrames = 0;
    bool promoted = false;
  };
  std::map<CacheKey, CacheEntry> m_staticCache;

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  std::optional<vk::raii::DescriptorSetLayout> m_setTextures;
  std::optional<vk::raii::DescriptorSetLayout> m_setDDPPeel;
  std::optional<vk::raii::DescriptorSetLayout> m_setLighting;
  std::optional<vk::raii::DescriptorSetLayout> m_setTransforms;
  std::optional<vk::raii::DescriptorSetLayout> m_setOIT; // set = 3 (OIT params)
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTextures;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsLighting;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTransforms;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsOIT;
  // Per-draw override sets are owned by the backend's per-frame arena.
  // Track raw pointers here only if needed; do not take ownership.
  std::vector<ZVulkanDescriptorSet*> m_transientDescriptorSets;
  bool m_texturesSetInitialized = false;

  std::unique_ptr<ZVulkanTexture> m_placeholder1D;
  std::unique_ptr<ZVulkanTexture> m_placeholder2D;
  std::unique_ptr<ZVulkanTexture> m_placeholder3D;

  std::unique_ptr<ZVulkanBuffer> m_uboLighting;
  std::unique_ptr<ZVulkanBuffer> m_uboTransforms;
  std::unique_ptr<ZVulkanBuffer> m_uboMaterial;
  std::unique_ptr<ZVulkanBuffer> m_uboOIT;

  size_t m_vertexCount = 0;
  size_t m_indexCount = 0;

  std::vector<MeshDraw> m_draws;

  // No GL texture bridging: Vulkan mesh pipeline uses placeholders or
  // backend-native textures only.

  // SoA upload arena-backed streams (per-attribute buffers)
  vk::Buffer m_posBuffer{};
  vk::Buffer m_normBuffer{};
  vk::Buffer m_colorBuffer{};
  vk::Buffer m_texBuffer{}; // 1D/2D/3D depending on colorSource
  vk::DeviceSize m_posOffset{0};
  vk::DeviceSize m_normOffset{0};
  vk::DeviceSize m_colorOffset{0};
  vk::DeviceSize m_texOffset{0}; // 1D/2D/3D depending on colorSource
  enum class TexBinding
  {
    None,
    Tex1D,
    Tex2D,
    Tex3D
  } m_texBinding = TexBinding::None;
  vk::Buffer m_indexUploadBuffer{};
  vk::DeviceSize m_indexUploadOffset{0};

  void ensureDescriptorLayouts();
  void ensurePlaceholderTextures();
  void ensureDescriptorSets();
  void ensureOITResources();
  void updateOITParamsUBO(Z3DRendererBase& renderer, const RenderBatch& batch, const glm::vec2& fallbackScreenDimRcp);
  void
  updateLightingUBO(Z3DRendererBase& renderer, const RenderBatch& batch, const MeshPayload& payload, bool pickingPass);
  void updateTransformUBO(Z3DRendererBase& renderer, const RenderBatch& batch, const MeshPayload& payload);
  void updateMaterialUBO(Z3DRendererBase& renderer,
                         const MeshPayload& payload,
                         size_t meshIndex,
                         bool useFallbackColor,
                         const glm::vec4& fallbackColor,
                         bool pickingPass);
  void bindDescriptorSets(vk::raii::CommandBuffer& cmd,
                          const PipelineInstance& pipeline,
                          ZVulkanDescriptorSet* texturesOverride = nullptr) const;
  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  void uploadGeometry(const MeshPayload& payload);

  std::optional<TextureBinding> bindTextureIfNeeded(const MeshPayload& /*payload*/)
  {
    return std::nullopt;
  }
  void resetDescriptors();
};

} // namespace nim
