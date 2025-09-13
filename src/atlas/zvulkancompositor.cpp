#include "zvulkancompositor.h"

#include <fmt/format.h>
#include <cmath>

namespace nim {

ZVulkanCompositor::ZVulkanCompositor(ZVulkanDevice& device, uint32_t width, uint32_t height)
  : m_rendererBase(device, width, height)
  , m_bg(m_rendererBase)
  , m_lines(m_rendererBase)
{
  // Default camera and viewport
  auto& cam = m_rendererBase.globalCamera();
  cam.setCoordinateSystem(Z3DCoordinateSystem::Vulkan);
  cam.setAspectRatio(static_cast<float>(width) / static_cast<float>(height));

  // Wide line emulation by default
  m_lines.setNeedLighting(false);
  m_lines.setFollowCoordTransform(false);
}

void ZVulkanCompositor::resize(uint32_t width, uint32_t height)
{
  m_rendererBase.resize(width, height);
  m_bgDirty = true;
}

void ZVulkanCompositor::ensureBackgroundState()
{
  if (!m_bgDirty) return;
  m_bg.setMode(m_bgMode);
  m_bg.setGradientOrientation(m_bgOrient);
  m_bg.setColors(m_bgColor1, m_bgColor2);
  m_bg.setRegion(m_bgRegion);
  const float w = static_cast<float>(m_rendererBase.width());
  const float h = static_cast<float>(m_rendererBase.height());
  m_bg.setScreenDimRCP(1.0f / w, 1.0f / h);
  m_bgDirty = false;
}

void ZVulkanCompositor::buildAxisLines()
{
  m_axisLines.clear();
  m_axisLineColors.clear();

  // Axis endpoints in a small unit space
  glm::vec3 origin(0.0f);
  glm::vec3 XEnd(256.f, 0.f, 0.f);
  glm::vec3 YEnd(0.f, 256.f, 0.f);
  glm::vec3 ZEnd(0.f, 0.f, 256.f);

  // Rotate by camera rotation to match GL axis behavior
  glm::mat3 rot = m_rendererBase.globalCamera().rotateMatrix(MonoEye);
  XEnd = rot * XEnd;
  YEnd = rot * YEnd;
  ZEnd = rot * ZEnd;

  auto pushSeg = [&](const glm::vec3& p0, const glm::vec3& p1, const glm::vec4& c){
    m_axisLines.push_back(p0); m_axisLines.push_back(p1);
    m_axisLineColors.push_back(c); m_axisLineColors.push_back(c);
  };

  pushSeg(origin, XEnd * glm::vec3(0.88f), m_axisXColor);
  pushSeg(origin, YEnd * glm::vec3(0.88f), m_axisYColor);
  pushSeg(origin, ZEnd * glm::vec3(0.88f), m_axisZColor);

  m_lines.setData(&m_axisLines);
  m_lines.setDataColors(&m_axisLineColors);
  m_lines.setUseSmoothLine(true);
  m_lines.setLineWidth(2.0f);
}

std::vector<uint8_t> ZVulkanCompositor::renderAndReadback()
{
  ensureBackgroundState();

  auto cmd = m_rendererBase.beginFrame(vk::ClearColorValue(std::array<float,4>{0,0,0,1}),
                                       vk::ClearDepthStencilValue(1.0f, 0));

  // Background
  if (m_showBackground) {
    m_bg.render(cmd);
  }

  // Axis: draw in a small overlay viewport (bottom-left) similar to GL path
  if (m_showAxis) {
    buildAxisLines();

    // Compute axis viewport square
    const uint32_t W = m_rendererBase.width();
    const uint32_t H = m_rendererBase.height();
    const uint32_t S = static_cast<uint32_t>(std::round(std::min(W, H) * m_axisRegionRatio));

    // Set a temporary camera for axis
    Z3DCamera axisCam;
    axisCam.setFieldOfView(glm::radians(10.f));
    float radius = 300.f;
    float distance = radius / std::sin(axisCam.fieldOfView() * 0.5f);
    glm::vec3 center(0);
    glm::vec3 vn(0,0,1);
    glm::vec3 position = center + vn * distance;
    axisCam.setCamera(position, center, glm::vec3(0,1,0));
    axisCam.setNearDist(distance - radius - 1);
    axisCam.setFarDist(distance + radius);
    axisCam.setAspectRatio(1.0f);

    // Swap in camera for axis rendering
    m_rendererBase.setCamera(axisCam);

    // Dynamic viewport/scissor for axis region (bottom-left)
    vk::Viewport vp{.x = 0.0f, .y = 0.0f, .width = static_cast<float>(S), .height = static_cast<float>(S), .minDepth = 0.0f, .maxDepth = 1.0f};
    vk::Rect2D sc{{0,0}, {S,S}};
    cmd.setViewport(0, vp);
    cmd.setScissor(0, sc);

    // Render lines
    m_lines.render(cmd);

    // Restore full viewport for any subsequent draws
    vk::Viewport fullVp{.x = 0.0f, .y = 0.0f,
                        .width = static_cast<float>(W), .height = static_cast<float>(H),
                        .minDepth = 0.0f, .maxDepth = 1.0f};
    vk::Rect2D fullSc{{0,0}, {W,H}};
    cmd.setViewport(0, fullVp);
    cmd.setScissor(0, fullSc);
    m_rendererBase.unsetCamera();
  }

  m_rendererBase.endFrame(cmd);

  // Readback
  std::vector<uint8_t> pixels(static_cast<size_t>(m_rendererBase.width()) * static_cast<size_t>(m_rendererBase.height()) * 4);
  m_rendererBase.copyToMemory(pixels.data(), pixels.size());
  return pixels;
}

} // namespace nim
