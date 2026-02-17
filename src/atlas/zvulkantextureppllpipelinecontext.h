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

  // Matches layout(push_constant) PPLLResolvePC in Resources/shader/vulkan/ppll_resolve.frag.
  struct PPLLResolvePushConstants
  {
    uint32_t opaqueDepthTexture = 0;
  };

  Z3DRendererVulkanBackend& m_backend;

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
};

} // namespace nim
