#include "z3dlinewithfixedwidthcolorrenderer.h"

namespace nim {

Z3DLineWithFixedWidthColorRenderer::Z3DLineWithFixedWidthColorRenderer(Z3DRendererBase& base)
  : Z3DLineRenderer(base)
  , m_lineWidth("Line Width", 2.0f, 1, 100)
  , m_lineColor("Line Color", glm::vec4(1.f, 1.f, 0.f, 1.f))
{
  setUseSmoothLine(false);
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  setUseDisplayList(true);
#endif
  m_lineColor.setStyle("COLOR");
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  connect(&m_lineWidth, &ZFloatParameter::valueChanged, this,
          &Z3DLineWithFixedWidthColorRenderer::invalidateOpenglRenderer);
  connect(&m_lineWidth, &ZFloatParameter::valueChanged, this,
          &Z3DLineWithFixedWidthColorRenderer::invalidateOpenglPickingRenderer);
  connect(&m_lineColor, &ZVec4Parameter::valueChanged, this,
          &Z3DLineWithFixedWidthColorRenderer::invalidateOpenglRenderer);
#endif
  connect(&m_lineColor, &ZVec4Parameter::valueChanged, this, &Z3DLineWithFixedWidthColorRenderer::setLineColors);
}

void Z3DLineWithFixedWidthColorRenderer::setData(std::vector<glm::vec3>* linesInput)
{
  Z3DLineRenderer::setData(linesInput);
  setLineColors();
}

float Z3DLineWithFixedWidthColorRenderer::lineWidth() const
{
  return m_lineWidth.get() * (m_rendererBase.geometriesMultisampleModePara().isSelected("2x2") ? 2.f : 1.f);
}

void Z3DLineWithFixedWidthColorRenderer::setLineColors()
{
  m_lineColorsPrivate.clear();
  if (!m_linesPt)
    return;
  for (size_t i = 0; i < m_linesPt->size(); ++i) {
    m_lineColorsPrivate.push_back(m_lineColor.get());
  }
  setDataColors(&m_lineColorsPrivate);
}

} // namespace nim
