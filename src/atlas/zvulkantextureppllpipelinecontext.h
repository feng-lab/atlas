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

namespace vulkan {
struct AttachmentFormats;
}

// Fullscreen resolve pipeline for exact OIT via per-pixel fragment lists (PPLL).
// This pass reads the PPLL SSBOs (set = 3) and outputs premultiplied RGBA so the
// pipeline can blend over the existing color attachment (ONE, ONE_MINUS_SRC_ALPHA).
class ZVulkanTexturePPLLPipelineContext
{
public:
  explicit ZVulkanTexturePPLLPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanTexturePPLLPipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const TexturePPLLResolvePayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  friend class Z3DRendererVulkanBackend; // allow backend to pre-prime OIT resources

  struct PipelineKey
  {
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(colorFormats, depthFormat);
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

  // Descriptor sets:
  // - set 0: opaque depth texture (used to cull transparent fragments behind opaque)
  // - set 1/2: placeholders (kept to match other pipeline layouts)
  // - set 3: OIT SSBO bindings (PPLL buffers)
  std::optional<vk::raii::DescriptorSetLayout> m_setOpaqueDepth; // set = 0 (sampler2D)
  std::optional<vk::raii::DescriptorSetLayout> m_setPlaceholder; // set = 1/2 (empty)
  vk::DescriptorSetLayout m_setOIT{}; // set = 3 OIT SSBO layout (params + PPLL buffers)
  std::unique_ptr<ZVulkanDescriptorSet> m_descriptorOIT;

  void ensureDescriptorLayouts();
  void resetDescriptors();
  void ensureOITResources();
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
};

} // namespace nim
