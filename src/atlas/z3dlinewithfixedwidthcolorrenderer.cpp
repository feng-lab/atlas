#include "z3dlinewithfixedwidthcolorrenderer.h"

#include <algorithm>
#include <utility>

namespace nim {

Z3DLineWithFixedWidthColorRenderer::Z3DLineWithFixedWidthColorRenderer(Z3DRendererBase& base)
  : Z3DLineRenderer(base)
{
  setUseSmoothLine(false);
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  setUseDisplayList(true);
#endif
  Z3DLineWithFixedWidthColorRenderer::setFixedLineWidth(m_fixedLineWidth);
}

void Z3DLineWithFixedWidthColorRenderer::setData(std::span<const glm::vec3> lines)
{
  Z3DLineRenderer::setData(lines);
  setLineColors();
}

void Z3DLineWithFixedWidthColorRenderer::setData(std::vector<glm::vec3> lines)
{
  Z3DLineRenderer::setData(std::move(lines));
  setLineColors();
}

float Z3DLineWithFixedWidthColorRenderer::lineWidth() const
{
  float width = m_fixedLineWidth;
  if (m_rendererBase.sceneState().geometryAAMode == GeometryAAMode::Supersample2x2) {
    width *= 2.f;
  }
  return width;
}

void Z3DLineWithFixedWidthColorRenderer::setLineColors()
{
  m_lineColorsPrivate.clear();
  if (m_linePositions.empty()) {
    return;
  }
  m_lineColorsPrivate.resize(m_linePositions.size(), m_lineColor);
  setDataColors(std::move(m_lineColorsPrivate));
}

void Z3DLineWithFixedWidthColorRenderer::setFixedLineWidth(float width)
{
  float clamped = std::max(1.f, width);
  if (clamped == m_fixedLineWidth) {
    return;
  }
  m_fixedLineWidth = clamped;
  m_lineWidth = clamped;
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
  invalidateOpenglPickingRenderer();
#endif
}

void Z3DLineWithFixedWidthColorRenderer::setLineColor(const glm::vec4& color)
{
  if (m_lineColor == color) {
    return;
  }
  m_lineColor = color;
  setLineColors();
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  invalidateOpenglRenderer();
#endif
}

} // namespace nim
