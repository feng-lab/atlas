#include "z3dskeletonfilter.h"

#include "zlog.h"

namespace nim {

Z3DSkeletonFilter::Z3DSkeletonFilter(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DGeometryFilter(globalParas, parent)
  , m_lineRenderer(m_rendererBase)
  , m_coneRenderer(m_rendererBase)
  , m_sphereRenderer(m_rendererBase)
  , m_renderingPrimitive("Rendering Mode")
  , m_color("Color", glm::vec4(1.f, 0.f, 0.f, 1.f), glm::vec4(0.f), glm::vec4(1.f))
{
  m_renderingPrimitive.addOptions("Normal", "Line");
  m_renderingPrimitive.select("Normal");
  connect(&m_renderingPrimitive, &ZStringIntOptionParameter::valueChanged, this, &Z3DSkeletonFilter::updateBoundBox);
  addParameter(m_renderingPrimitive);

  m_color.setStyle("COLOR");
  connect(&m_color, &ZVec4Parameter::valueChanged, this, &Z3DSkeletonFilter::prepareColor);
  addParameter(m_color);

  m_sphereRenderer.setUseDynamicMaterial(true);
}

void Z3DSkeletonFilter::setData(ZSkeleton& skeleton)
{
  m_skeleton = &skeleton;
  m_dataIsInvalid = true;
  invalidateResult();
}

bool Z3DSkeletonFilter::isReady(Z3DEye) const
{
  return m_skeleton != nullptr && !m_skeleton->vertices().empty() && !m_skeleton->edges().empty();
}

std::shared_ptr<ZWidgetsGroup> Z3DSkeletonFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_renderingPrimitive, 1);
    m_widgetsGroup->addChild(m_color, 1);
    m_widgetsGroup->addChild(m_rendererParameters.sizeScale, 1);
    m_widgetsGroup->addChild(m_rendererParameters.opacity, 1);
    m_widgetsGroup->addChild(m_rendererParameters.coordTransform, 1);
  }
  return m_widgetsGroup;
}

double Z3DSkeletonFilter::process(Z3DEye)
{
  syncRendererState();
  if (m_dataIsInvalid) {
    prepareData();
  }
  return 1.0;
}

void Z3DSkeletonFilter::updateNotTransformedBoundBoxImpl()
{
  m_notTransformedBoundBox.reset();
  if (!m_skeleton) {
    return;
  }
  m_notTransformedBoundBox.expand(m_skeleton->boundBox());
}

void Z3DSkeletonFilter::prepareColor()
{
  const glm::vec4 c = m_color.get();

  m_lineColors.assign(m_lines.size(), c);
  m_coneColors.assign(m_baseAndBaseRadius.size(), c);
  m_pointColors.assign(m_pointAndRadius.size(), c);

  m_lineRenderer.setDataColors(m_lineColors);
  m_coneRenderer.setDataColors(&m_coneColors);
  m_sphereRenderer.setDataColors(&m_pointColors);
}

void Z3DSkeletonFilter::prepareData()
{
  m_dataIsInvalid = false;

  m_lines.clear();
  m_lineColors.clear();
  m_baseAndBaseRadius.clear();
  m_axisAndTopRadius.clear();
  m_coneColors.clear();
  m_pointAndRadius.clear();
  m_pointColors.clear();

  if (!m_skeleton) {
    return;
  }

  const auto& verts = m_skeleton->vertices();
  const auto& edges = m_skeleton->edges();
  const bool hasRadii = m_skeleton->hasRadii() && m_skeleton->radii().size() == verts.size();
  const auto& radii = m_skeleton->radii();

  m_lines.reserve(edges.size() * 2);
  m_baseAndBaseRadius.reserve(edges.size());
  m_axisAndTopRadius.reserve(edges.size());
  m_coneColors.reserve(edges.size());

  for (const glm::uvec2& e : edges) {
    if (e.x >= verts.size() || e.y >= verts.size()) {
      continue;
    }
    const glm::vec3& a = verts[e.x];
    const glm::vec3& b = verts[e.y];

    m_lines.push_back(a);
    m_lines.push_back(b);

    const float ra = hasRadii ? radii[e.x] : 1.0f;
    const float rb = hasRadii ? radii[e.y] : 1.0f;
    m_baseAndBaseRadius.emplace_back(a.x, a.y, a.z, ra);
    m_axisAndTopRadius.emplace_back((b.x - a.x), (b.y - a.y), (b.z - a.z), rb);
    m_coneColors.push_back(m_color.get());
  }

  // Per-vertex colors for the line renderer (2 vertices per edge).
  m_lineColors.assign(m_lines.size(), m_color.get());

  // Per-vertex spheres for "Normal" rendering.
  m_pointAndRadius.reserve(verts.size());
  m_pointColors.reserve(verts.size());
  for (size_t i = 0; i < verts.size(); ++i) {
    const glm::vec3& p = verts[i];
    const float r = hasRadii ? radii[i] : 1.0f;
    m_pointAndRadius.emplace_back(p.x, p.y, p.z, r);
    m_pointColors.push_back(m_color.get());
  }

  m_lineRenderer.setData(m_lines);
  m_lineRenderer.setDataColors(m_lineColors);

  m_coneRenderer.setData(&m_baseAndBaseRadius, &m_axisAndTopRadius);
  m_coneRenderer.setDataColors(&m_coneColors);

  m_sphereRenderer.setData(&m_pointAndRadius);
  m_sphereRenderer.setDataColors(&m_pointColors);

  updateBoundBox();
}

void Z3DSkeletonFilter::renderOpaque(Z3DEye eye)
{
  if (!m_skeleton) {
    return;
  }

  if (!m_renderingPrimitive.isSelected("Line") && !m_baseAndBaseRadius.empty()) {
    if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
      m_rendererBase.renderVulkan(eye, m_coneRenderer, m_sphereRenderer);
    } else {
      m_rendererBase.render(eye, m_coneRenderer, m_sphereRenderer);
    }
  }
  renderBoundBox(eye);
}

void Z3DSkeletonFilter::renderTransparent(Z3DEye eye)
{
  if (!m_skeleton) {
    return;
  }

  if (m_renderingPrimitive.isSelected("Line")) {
    if (!m_lines.empty()) {
      if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
        m_rendererBase.renderVulkan(eye, m_lineRenderer);
      } else {
        m_rendererBase.render(eye, m_lineRenderer);
      }
    }
    renderBoundBox(eye);
    return;
  }

  // Non-line modes may still be transparent if opacity < 1.
  if (m_rendererParameters.opacity.get() < 1.f) {
    if (!m_baseAndBaseRadius.empty()) {
      if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
        m_rendererBase.renderVulkan(eye, m_coneRenderer, m_sphereRenderer);
      } else {
        m_rendererBase.render(eye, m_coneRenderer, m_sphereRenderer);
      }
    }
  }
  renderBoundBox(eye);
}

} // namespace nim
