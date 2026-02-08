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
class ZVulkanDescriptorPool;
class ZVulkanDescriptorSet;
class ZVulkanTexture;
class ZVulkanBuffer;

namespace vulkan {
struct AttachmentFormats;
}

class ZVulkanTextureGlowPipelineContext
{
public:
  explicit ZVulkanTextureGlowPipelineContext(Z3DRendererVulkanBackend& backend);
  ~ZVulkanTextureGlowPipelineContext();

  void resetFrame();

  void record(Z3DRendererBase& renderer,
              const RenderBatch& batch,
              const TextureGlowPayload& payload,
              const vk::Viewport& viewport,
              const vk::Rect2D& scissor,
              vk::raii::CommandBuffer& cmd);

private:
  struct QuadVertex
  {
    glm::vec3 position{0.0f};
  };

  struct BlurPipelineKey
  {
    bool horizontal = true;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(horizontal, colorFormats, depthFormat);
    }

    bool operator<(const BlurPipelineKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };

  struct GlowPipelineKey
  {
    GlowMode mode = GlowMode::Screen;
    std::vector<vk::Format> colorFormats;
    std::optional<vk::Format> depthFormat;

    auto tie() const
    {
      return std::tuple(static_cast<int>(mode), colorFormats, depthFormat);
    }

    bool operator<(const GlowPipelineKey& rhs) const
    {
      return tie() < rhs.tie();
    }
  };

  struct PipelineInstance
  {
    std::unique_ptr<ZVulkanShader> shader;
    std::unique_ptr<ZVulkanPipeline> pipeline;
  };

  struct TextureUpload
  {
    std::unique_ptr<ZVulkanTexture> texture;
    glm::uvec3 extent{0u};
    vk::Format format = vk::Format::eUndefined;
  };

  struct BlurIntermediate
  {
    TextureUpload color;
    TextureUpload depth;
  };

  struct BlurPushConstants
  {
    glm::vec2 screenDimRcp{1.0f};
    int blurRadius = 0;
    float blurScale = 1.0f;
    float blurStrength = 0.5f;
    float _pad = 0.0f;
  };

  struct GlowPushConstants
  {
    glm::vec2 screenDimRcp{1.0f};
    glm::vec2 _pad{0.0f};
  };

  Z3DRendererVulkanBackend& m_backend;

  std::map<BlurPipelineKey, PipelineInstance> m_blurPipelines;
  std::map<GlowPipelineKey, PipelineInstance> m_glowPipelines;

  std::optional<vk::raii::DescriptorSetLayout> m_blurSetLayout;
  std::optional<vk::raii::DescriptorSetLayout> m_glowSetLayout;

  void ensureDescriptorLayouts();
  void resetDescriptors();

  vk::PipelineVertexInputStateCreateInfo makeVertexInputState() const;

  PipelineInstance& ensureBlurPipeline(const BlurPipelineKey& key, const vulkan::AttachmentFormats& formats);
  PipelineInstance& ensureGlowPipeline(const GlowPipelineKey& key, const vulkan::AttachmentFormats& formats);
};

} // namespace nim
