#pragma once

#include "z3dprimitiverenderer.h"

namespace nim {

class Z3DBackgroundRenderer : public Z3DPrimitiveRenderer
{
Q_OBJECT
public:
  explicit Z3DBackgroundRenderer(Z3DRendererBase& rendererBase);

  ZStringIntOptionParameter& modePara()
  { return m_mode; }

  ZVec4Parameter& firstColorPara()
  { return m_firstColor; }

  ZVec4Parameter& secondColorPara()
  { return m_secondColor; }

  ZStringIntOptionParameter& gradientOrientationPara()
  { return m_gradientOrientation; }

  void setRenderingRegion(double left = 0., double right = 1., double bottom = 0., double top = 1.);

protected:
  void adjustWidgets();

  void compile() override;

  QString generateHeader();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  void renderUsingOpengl() override;
  void renderPickingUsingOpengl() override;
#endif

  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye /*unused*/) override;

protected:
  Z3DShaderGroup m_backgroundShaderGrp;

  ZVec4Parameter m_firstColor;
  ZVec4Parameter m_secondColor;
  ZStringIntOptionParameter m_gradientOrientation;
  ZStringIntOptionParameter m_mode;

  ZVertexArrayObject m_VAO;
  ZVertexBufferObject m_VBO;

  glm::vec4 m_region;
};

} // namespace nim

