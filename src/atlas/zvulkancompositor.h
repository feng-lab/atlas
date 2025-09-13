#pragma once

#include "zvulkanrendererbase.h"
#include "zvulkanbackgroundrenderer.h"
#include "zvulkanlinerenderer.h"
#include "z3dcamera.h"
#include "zglmutils.h"

namespace nim {

// Minimal Vulkan compositor: draws background + optional axis lines into the offscreen swapchain
class ZVulkanCompositor
{
public:
  ZVulkanCompositor(ZVulkanDevice& device, uint32_t width, uint32_t height);

  // Resize output
  void resize(uint32_t width, uint32_t height);

  // Background controls
  void setShowBackground(bool v) { m_showBackground = v; }
  void setBackgroundMode(ZVulkanBackgroundRenderer::Mode mode) { m_bgMode = mode; m_bgDirty = true; }
  void setBackgroundOrientation(ZVulkanBackgroundRenderer::GradientOrientation o) { m_bgOrient = o; m_bgDirty = true; }
  void setBackgroundColors(const glm::vec4& c1, const glm::vec4& c2) { m_bgColor1 = c1; m_bgColor2 = c2; }
  void setBackgroundRegion(const glm::vec4& region) { m_bgRegion = region; }

  // Axis controls
  void setShowAxis(bool v) { m_showAxis = v; }
  void setAxisColors(const glm::vec4& x, const glm::vec4& y, const glm::vec4& z)
  {
    m_axisXColor = x; m_axisYColor = y; m_axisZColor = z;
  }
  void setAxisRegionRatio(float r) { m_axisRegionRatio = r; }

  // Camera
  Z3DCamera& camera() { return m_rendererBase.globalCamera(); }

  // Render one frame and copy to CPU memory. Returns pixels in RGBA8.
  std::vector<uint8_t> renderAndReadback();

  // Expose renderer base for advanced control/tests
  ZVulkanRendererBase& rendererBase() { return m_rendererBase; }

private:
  void ensureBackgroundState();
  void buildAxisLines();

private:
  ZVulkanRendererBase m_rendererBase;
  ZVulkanBackgroundRenderer m_bg;
  ZVulkanLineRenderer m_lines;

  // Background state
  bool m_showBackground = true;
  bool m_bgDirty = true;
  ZVulkanBackgroundRenderer::Mode m_bgMode = ZVulkanBackgroundRenderer::Mode::Gradient;
  ZVulkanBackgroundRenderer::GradientOrientation m_bgOrient =
    ZVulkanBackgroundRenderer::GradientOrientation::TopToBottom;
  glm::vec4 m_bgColor1{0.05f, 0.07f, 0.09f, 1.0f};
  glm::vec4 m_bgColor2{0.25f, 0.27f, 0.29f, 1.0f};
  glm::vec4 m_bgRegion{0.0f, 1.0f, 0.0f, 1.0f};

  // Axis state
  bool m_showAxis = true;
  float m_axisRegionRatio = 0.25f;
  glm::vec4 m_axisXColor{1,0,0,1};
  glm::vec4 m_axisYColor{0,1,0,1};
  glm::vec4 m_axisZColor{0,0,1,1};

  // Axis geometry (line list p0,p1,...)
  std::vector<glm::vec3> m_axisLines;
  std::vector<glm::vec4> m_axisLineColors;
};

} // namespace nim

