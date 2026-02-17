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
class ZVulkanTexture;
class ZVulkanBuffer;

namespace vulkan {
struct AttachmentFormats;
}

class ZVulkanTextureDualPeelPipelineContext
{
public:
  explicit ZVulkanTextureDualPeelPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanTextureDualPeelPipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const TextureDualPeelPayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  enum class Stage
  {
    Carry,
    Blend,
    Final
  };

  struct PipelineKey
  {
    Stage stage = Stage::Blend;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(static_cast<int>(stage), colorFormats, depthFormat);
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
    Stage stage = Stage::Blend;
  };

  // Matches push-constant blocks in dual_peeling_{carry,blend,final}.frag.
  // Each stage consumes a prefix of this struct.
  struct DualPeelPushConstants
  {
    uint32_t tex0 = 0;
    uint32_t tex1 = 0;
    uint32_t tex2 = 0;
  };

  Z3DRendererVulkanBackend& m_backend;

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  size_t m_vertexCount = 0;

  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
};

} // namespace nim
