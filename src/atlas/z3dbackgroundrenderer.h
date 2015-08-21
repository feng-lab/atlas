#ifndef Z3DBACKGROUNDRENDERER_H
#define Z3DBACKGROUNDRENDERER_H

#include "z3dprimitiverenderer.h"

namespace nim {

class Z3DBackgroundRenderer : public Z3DPrimitiveRenderer
{
  Q_OBJECT
public:
  explicit Z3DBackgroundRenderer(Z3DRendererBase &rendererBase);

  ZStringIntOptionParameter& modePara() { return m_mode; }
  ZVec4Parameter& firstColorPara() { return m_firstColor; }
  ZVec4Parameter& secondColorPara() { return m_secondColor; }
  ZStringIntOptionParameter& gradientOrientationPara() { return m_gradientOrientation; }

signals:

protected slots:
  void adjustWidgets();

protected:
  virtual void compile() override;
  QString generateHeader();

#ifndef _USE_CORE_PROFILE_
  virtual void renderUsingOpengl() override;
  virtual void renderPickingUsingOpengl() override;
#endif

  virtual void render(Z3DEye eye) override;
  virtual void renderPicking(Z3DEye) override;

  Z3DShaderGroup m_backgroundShaderGrp;

  ZVec4Parameter m_firstColor;
  ZVec4Parameter m_secondColor;
  ZStringIntOptionParameter m_gradientOrientation;
  ZStringIntOptionParameter m_mode;

  ZVertexArrayObject m_VAO;
  ZVertexBufferObject m_VBO;
};

} // namespace nim

#endif // Z3DBACKGROUNDRENDERER_H
