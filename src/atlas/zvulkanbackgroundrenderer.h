#pragma once

#include "zvulkanrenderer.h"
#include "zvulkanpipeline.h"
#include <memory>

namespace nim {

class ZVulkanShader;
class ZVulkanBuffer;

// Minimal background renderer to validate Vulkan pipeline and SPIR-V modules
class ZVulkanBackgroundRenderer : public ZVulkanRenderer
{
public:
  explicit ZVulkanBackgroundRenderer(ZVulkanRendererBase& rendererBase);
  ~ZVulkanBackgroundRenderer() override;

  void compile() override;
  void render(vk::raii::CommandBuffer& cmdBuffer) override;

  // Background parameters
  void setScreenDimRCP(float invW, float invH) { m_screenDimRCP = {invW, invH}; }
  void setColors(const glm::vec4& c1, const glm::vec4& c2) { m_color1 = c1; m_color2 = c2; }
  // region = {x0, xScale, y0, yScale}
  void setRegion(const glm::vec4& r) { m_region = r; }

  // Parity with Z3DBackgroundRenderer
  enum class Mode { Uniform, Gradient };
  enum class GradientOrientation { LeftToRight, RightToLeft, TopToBottom, BottomToTop };
  void setMode(Mode m) { m_mode = m; m_pipeline.reset(); /* force recreate to apply specs */ }
  void setGradientOrientation(GradientOrientation o) { m_orientation = o; m_pipeline.reset(); }

private:
  void ensureVertexBuffer();

  std::unique_ptr<ZVulkanShader> m_shader;
  std::unique_ptr<ZVulkanPipeline> m_pipeline;
  std::unique_ptr<ZVulkanBuffer> m_vertexBuffer;

  glm::vec2 m_screenDimRCP{1.0f, 1.0f};
  glm::vec4 m_color1{0.1f, 0.1f, 0.1f, 1.0f};
  glm::vec4 m_color2{0.3f, 0.3f, 0.3f, 1.0f};
  glm::vec4 m_region{0.0f, 1.0f, 0.0f, 1.0f};

  Mode m_mode{Mode::Gradient};
  GradientOrientation m_orientation{GradientOrientation::BottomToTop};
};

} // namespace nim
