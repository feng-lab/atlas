#include "zvulkancompositor.h"

#include "z3dglobalparameters.h"
#include "zlog.h"
#include "zwidgetsgroup.h"
#include "zvulkanswapchain.h"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <QString>

namespace nim {

ZVulkanCompositor::ZVulkanCompositor(ZVulkanDevice& device,
                                     Z3DGlobalParameters& globals,
                                     uint32_t width,
                                     uint32_t height,
                                     QObject* parent)
  : Z3DCompositorBase(parent)
  , m_globals(globals)
  , m_rendererBase(device, width, height)
  , m_bg(m_rendererBase)
  , m_lines(m_rendererBase)
  , m_outputSize(width, height)
{
  m_rendererBase.setGlobalParameters(&m_globals);
  syncFromGlobalParameters();

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

void ZVulkanCompositor::setOutputSize(const glm::uvec2& size)
{
  if (m_outputSize == size) {
    return;
  }
  m_outputSize = size;
  resize(size.x, size.y);
}

glm::uvec2 ZVulkanCompositor::outputSize() const
{
  return m_outputSize;
}

void ZVulkanCompositor::setRenderingRegion(double left, double right, double bottom, double top)
{
  m_renderRegion = {left, right, bottom, top};
  setBackgroundRegion(glm::vec4(static_cast<float>(left),
                                 static_cast<float>(right),
                                 static_cast<float>(bottom),
                                 static_cast<float>(top)));
}

void ZVulkanCompositor::setProgressiveRenderingMode(bool v)
{
  m_progressive = v;
}

void ZVulkanCompositor::requestRender(bool stereo)
{
  if (stereo) {
    LOG(WARNING) << "ZVulkanCompositor currently renders mono only";
  }

  syncFromGlobalParameters();

  auto pixels = renderAndReadback();
  m_monoLocalBuffer.width = m_rendererBase.width();
  m_monoLocalBuffer.height = m_rendererBase.height();
  m_monoLocalBuffer.data.assign(pixels.begin(), pixels.end());
  m_monoReadyLocalBuffer = &m_monoLocalBuffer;

  Q_EMIT Z3DCompositorBase::renderingFinished();
}

std::shared_ptr<ZWidgetsGroup> ZVulkanCompositor::backgroundWidgetsGroup()
{
  if (!m_backgroundWidgetsGroup) {
    m_backgroundWidgetsGroup = std::make_shared<ZWidgetsGroup>(QStringLiteral("Vulkan Background"), 0);
  }
  return m_backgroundWidgetsGroup;
}

std::shared_ptr<ZWidgetsGroup> ZVulkanCompositor::axisWidgetsGroup()
{
  if (!m_axisWidgetsGroup) {
    m_axisWidgetsGroup = std::make_shared<ZWidgetsGroup>(QStringLiteral("Vulkan Axis"), 0);
  }
  return m_axisWidgetsGroup;
}

void ZVulkanCompositor::read(const json::object& jo)
{
  auto toDouble = [](const json::value& v) -> double {
    if (v.is_double()) return v.as_double();
    if (v.is_int64()) return static_cast<double>(v.as_int64());
    if (v.is_uint64()) return static_cast<double>(v.as_uint64());
    return 0.0;
  };
  auto toUInt = [](const json::value& v) -> uint32_t {
    if (v.is_uint64()) return static_cast<uint32_t>(v.as_uint64());
    if (v.is_int64()) return static_cast<uint32_t>(std::max<int64_t>(0, v.as_int64()));
    if (v.is_double()) return static_cast<uint32_t>(std::max(0.0, v.as_double()));
    return 0u;
  };

  if (const auto* prog = jo.if_contains("progressive")) {
    if (prog->is_bool()) {
      m_progressive = prog->as_bool();
    }
  }

  if (const auto* region = jo.if_contains("renderRegion")) {
    if (region->is_array() && region->as_array().size() == 4) {
      const auto& arr = region->as_array();
      m_renderRegion.left = toDouble(arr[0]);
      m_renderRegion.right = toDouble(arr[1]);
      m_renderRegion.bottom = toDouble(arr[2]);
      m_renderRegion.top = toDouble(arr[3]);
      setRenderingRegion(m_renderRegion.left, m_renderRegion.right, m_renderRegion.bottom, m_renderRegion.top);
    }
  }

  if (const auto* size = jo.if_contains("outputSize")) {
    if (size->is_array() && size->as_array().size() == 2) {
      const auto& arr = size->as_array();
      setOutputSize(glm::uvec2{toUInt(arr[0]), toUInt(arr[1])});
    }
  }
}

void ZVulkanCompositor::write(json::object& jo) const
{
  jo["backend"] = "vulkan";
  jo["progressive"] = m_progressive;

  json::array sizeArr;
  sizeArr.emplace_back(static_cast<uint64_t>(m_outputSize.x));
  sizeArr.emplace_back(static_cast<uint64_t>(m_outputSize.y));
  jo["outputSize"] = std::move(sizeArr);

  json::array regionArr;
  regionArr.emplace_back(m_renderRegion.left);
  regionArr.emplace_back(m_renderRegion.right);
  regionArr.emplace_back(m_renderRegion.bottom);
  regionArr.emplace_back(m_renderRegion.top);
  jo["renderRegion"] = std::move(regionArr);
}

Z3DLocalColorBuffer* ZVulkanCompositor::monoReadyLocalBuffer() const
{
  return m_monoReadyLocalBuffer;
}

Z3DLocalColorBuffer* ZVulkanCompositor::leftReadyLocalBuffer() const
{
  return nullptr;
}

Z3DLocalColorBuffer* ZVulkanCompositor::rightReadyLocalBuffer() const
{
  return nullptr;
}

void ZVulkanCompositor::savePickingBufferToImage(const QString& filename)
{
  LOG(WARNING) << "Picking buffer save not implemented for Vulkan compositor: " << filename.toStdString();
}

void ZVulkanCompositor::syncFromGlobalParameters()
{
  m_rendererBase.syncFromGlobalParameters();
}

} // namespace nim
