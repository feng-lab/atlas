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

class ZVulkanTextureBlendPipelineContext
{
public:
  explicit ZVulkanTextureBlendPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanTextureBlendPipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const TextureBlendPayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  struct QuadVertex
  {
    glm::vec3 position{0.0f};
  };

  struct PipelineKey
  {
    TextureBlendMode mode = TextureBlendMode::DepthTestBlending;
    bool enableBlend = false; // fixed-function blend enable (premultiplied)
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(static_cast<int>(mode), enableBlend, colorFormats, depthFormat);
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

  struct TextureBlendPushConstants
  {
    glm::vec2 screenDimRcp{1.0f};
    uint32_t colorTexture0 = 0;
    uint32_t depthTexture0 = 0;
    uint32_t colorTexture1 = 0;
    uint32_t depthTexture1 = 0;
  };

  Z3DRendererVulkanBackend& m_backend;

  std::map<PipelineKey, PipelineInstance> m_pipelineCache;

  std::unique_ptr<ZVulkanBuffer> m_vertexBuffer;
  size_t m_vertexCount = 0;

  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  void ensureVertexCapacity(size_t vertexCount);
  void uploadGeometry();

  PipelineInstance& ensurePipeline(const PipelineKey& key, const vulkan::AttachmentFormats& formats);
};

} // namespace nim
