#include "z3dlinewithfixedwidthcolorrenderer.h"

#include <algorithm>

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

void Z3DLineWithFixedWidthColorRenderer::setData(std::vector<glm::vec3>* linesInput)
{
  Z3DLineRenderer::setData(linesInput);
  setLineColors();
}

float Z3DLineWithFixedWidthColorRenderer::lineWidth() const
{
  float width = m_fixedLineWidth;
  if (m_rendererBase.geometriesMultisampleModePara().isSelected("2x2")) {
    width *= 2.f;
  }
  return width;
}

void Z3DLineWithFixedWidthColorRenderer::setLineColors()
{
  m_lineColorsPrivate.clear();
  if (!m_linesPt) {
    return;
  }
  for (size_t i = 0; i < m_linesPt->size(); ++i) {
    m_lineColorsPrivate.push_back(m_lineColor);
  }
  setDataColors(&m_lineColorsPrivate);
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
