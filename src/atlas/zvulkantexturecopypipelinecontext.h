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
class ZVulkanDescriptorSet;
class ZVulkanTexture;
class ZVulkanBuffer;

namespace vulkan {
struct AttachmentFormats;
}

class ZVulkanTextureCopyPipelineContext
{
public:
  explicit ZVulkanTextureCopyPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanTextureCopyPipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const TextureCopyPayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  // Allow backend to pre-prime descriptor/UBO resources before recording
  friend class Z3DRendererVulkanBackend;
  struct QuadVertex
  {
    glm::vec3 position{0.0f};
    glm::vec2 uv{0.0f};
  };

  struct PipelineKey
  {
    bool discardTransparent = true;
    TextureCopyPayload::OutputMode mode = TextureCopyPayload::OutputMode::NoChange;
    bool flipY = false;
    bool waInit = false; // use WA-init image shader (writes 2 color attachments with additive blend)
    bool wbInit = false; // use WB-init image shader (writes 2 color attachments with specific blends)
    bool ddpInit = false; // use DDP-init image shader (writes depth blender values)
    bool ddpPeel = false; // use DDP-peel image shader (reads blender tex, updates front/back)
    bool ppllCount = false; // exact OIT PPLL count pass (writes to SSBO, no color outputs)
    bool ppllStore = false; // exact OIT PPLL store pass (writes to SSBO, no color outputs)
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(discardTransparent,
                        static_cast<int>(mode),
                        flipY,
                        waInit,
                        wbInit,
                        ddpInit,
                        ddpPeel,
                        ppllCount,
                        ppllStore,
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

  std::optional<vk::raii::DescriptorSetLayout> m_setTextures;
  vk::DescriptorSetLayout m_setLighting{}; // set 1: lighting UBO (used by WB init image shader)
  vk::DescriptorSetLayout m_setOIT{}; // set 3: OIT SSBOs (DDP flag / PPLL buffers)
  std::unique_ptr<ZVulkanDescriptorSet> m_descriptorSetOIT;
  std::unique_ptr<ZVulkanDescriptorSet> m_dsLighting;

  std::unique_ptr<ZVulkanBuffer> m_vertexBuffer;
  size_t m_vertexCapacity = 0;
  size_t m_vertexCount = 0;

  void ensureDescriptorLayout();
  void resetDescriptors();
  void ensureDescriptorSet();
  void ensureLightingResources();
  void ensureOITResources();
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  void ensureVertexCapacity(size_t vertexCount);
  void uploadGeometry();

  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
};

} // namespace nim
