#pragma once

#include "z3drendercommands.h"
#include "z3drendererstates.h"
#include "zvulkan.h"
#include "zglmutils.h"

#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

namespace nim {

enum class FogMode;

class Z3DRendererBase;
class Z3DRendererVulkanBackend;
class ZVulkanShader;
class ZVulkanPipeline;
class ZVulkanDescriptorPool;
class ZVulkanDescriptorSet;
class ZVulkanTexture;
class ZVulkanBuffer;
class ZMesh;

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
  struct PipelineKey
  {
    MeshPayload::ColorSource colorSource = MeshPayload::ColorSource::MeshColor;
    ZMesh::Type meshType;
    bool wireframe = false;
    FogMode fogMode = FogMode::None;

    auto tie() const
    {
      return std::tuple(static_cast<int>(colorSource), static_cast<int>(meshType), wireframe, static_cast<int>(fogMode));
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

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  std::optional<vk::raii::DescriptorSetLayout> m_setTextures;
  std::optional<vk::raii::DescriptorSetLayout> m_setLighting;
  std::optional<vk::raii::DescriptorSetLayout> m_setTransforms;
  std::unique_ptr<ZVulkanDescriptorPool> m_descriptorPool;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTextures;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsLighting;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsTransforms;

  std::unique_ptr<ZVulkanTexture> m_placeholder1D;
  std::unique_ptr<ZVulkanTexture> m_placeholder2D;
  std::unique_ptr<ZVulkanTexture> m_placeholder3D;

  std::unique_ptr<ZVulkanBuffer> m_uboLighting;
  std::unique_ptr<ZVulkanBuffer> m_uboTransforms;
  std::unique_ptr<ZVulkanBuffer> m_uboMaterial;

  std::unique_ptr<ZVulkanBuffer> m_vertexBuffer;
  std::unique_ptr<ZVulkanBuffer> m_indexBuffer;
  size_t m_vertexCapacity = 0;
  size_t m_indexCapacity = 0;
  size_t m_vertexCount = 0;
  size_t m_indexCount = 0;

  std::vector<MeshDraw> m_draws;

  std::map<const Z3DTexture*, std::unique_ptr<ZVulkanTexture>> m_textureCache;

  void ensureDescriptorLayouts();
  void ensurePlaceholderTextures();
  void ensureDescriptorSets();
  void updateLightingUBO(Z3DRendererBase& renderer, const RenderBatch& batch, const MeshPayload& payload);
  void updateTransformUBO(Z3DRendererBase& renderer, const RenderBatch& batch);
  void updateMaterialUBO(Z3DRendererBase& renderer,
                         const MeshPayload& payload,
                         size_t meshIndex,
                         bool useFallbackColor,
                         const glm::vec4& fallbackColor);
  void bindDescriptorSets(vk::raii::CommandBuffer& cmd, const PipelineInstance& pipeline) const;
  PipelineInstance& ensurePipeline(const PipelineKey& key);
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  void ensureVertexCapacity(size_t vertexCount);
  void ensureIndexCapacity(size_t indexCount);
  void uploadGeometry(const MeshPayload& payload);

  std::optional<TextureBinding> bindTextureIfNeeded(const MeshPayload& payload);
  ZVulkanTexture* ensureTextureUpload(const Z3DTexture& source);
};

} // namespace nim
