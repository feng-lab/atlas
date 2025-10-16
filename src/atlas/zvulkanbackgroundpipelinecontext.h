#pragma once

#include "z3drendercommands.h"
#include "z3drendererstates.h"
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
class ZVulkanBuffer;

class ZVulkanBackgroundPipelineContext
{
public:
  explicit ZVulkanBackgroundPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanBackgroundPipelineContext();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const BackgroundPayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  friend class Z3DRendererVulkanBackend; // backend drives record()

  struct PipelineKey
  {
    BackgroundMode mode = BackgroundMode::Gradient;
    BackgroundGradientOrientation orientation = BackgroundGradientOrientation::BottomToTop;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(mode, orientation, colorFormats, depthFormat);
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

  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;
};

} // namespace nim
