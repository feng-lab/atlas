#pragma once

#include "z3dcompositorbase.h"
#include "z3dglobalparameters.h"
#include "zvulkanrendererbase.h"
#include "zvulkanbackgroundrenderer.h"
#include "zvulkanlinerenderer.h"
#include "z3dcamera.h"
#include "zglmutils.h"
#include <memory>

namespace nim {

// Minimal Vulkan compositor: draws background + optional axis lines into the offscreen swapchain
class ZVulkanCompositor : public Z3DCompositorBase
{
public:
  ZVulkanCompositor(ZVulkanDevice& device,
                    Z3DGlobalParameters& globals,
                    uint32_t width,
                    uint32_t height,
                    QObject* parent = nullptr);

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

  // Z3DCompositorBase interface
  void setOutputSize(const glm::uvec2& size) override;
  glm::uvec2 outputSize() const override;
  void setRenderingRegion(double left, double right, double bottom, double top) override;
  void setProgressiveRenderingMode(bool v) override;
  void requestRender(bool stereo) override;

  std::shared_ptr<ZWidgetsGroup> backgroundWidgetsGroup() override;
  std::shared_ptr<ZWidgetsGroup> axisWidgetsGroup() override;

  void read(const json::object& json) override;
  void write(json::object& json) const override;

  Z3DLocalColorBuffer* monoReadyLocalBuffer() const override;
  Z3DLocalColorBuffer* leftReadyLocalBuffer() const override;
  Z3DLocalColorBuffer* rightReadyLocalBuffer() const override;

  void savePickingBufferToImage(const QString& filename) override;

private:
  void syncFromGlobalParameters();
  void ensureBackgroundState();
  void buildAxisLines();

private:
  Z3DGlobalParameters& m_globals;
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

  glm::uvec2 m_outputSize;
  struct
  {
    double left = 0.0;
    double right = 1.0;
    double bottom = 0.0;
    double top = 1.0;
  } m_renderRegion;
  bool m_progressive = false;
  std::shared_ptr<ZWidgetsGroup> m_backgroundWidgetsGroup;
  std::shared_ptr<ZWidgetsGroup> m_axisWidgetsGroup;
  Z3DLocalColorBuffer m_monoLocalBuffer{};
  Z3DLocalColorBuffer* m_monoReadyLocalBuffer = nullptr;
};

} // namespace nim
