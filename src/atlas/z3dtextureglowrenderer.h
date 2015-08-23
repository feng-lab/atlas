#ifndef Z3DTEXTUREGLOWRENDERER_H
#define Z3DTEXTUREGLOWRENDERER_H

#include "z3dprimitiverenderer.h"
#include "z3dshaderprogram.h"
#include "z3drendertarget.h"

namespace nim {

class Z3DTextureGlowRenderer : public Z3DPrimitiveRenderer
{
  Q_OBJECT
public:
  explicit Z3DTextureGlowRenderer(Z3DRendererBase &rendererBase);

  void setColorTexture(const Z3DTexture *colorTex) { m_colorTexture = colorTex; }
  void setDepthTexture(const Z3DTexture *depthTex) { m_depthTexture = depthTex; }

  ZStringStringOptionParameter& glowModePara() { return m_glowMode; }
  ZIntParameter& blurRadiusPara() { return m_blurRadius; }
  ZFloatParameter& blurScalePara() { return m_blurScale; }
  ZFloatParameter& blurStrengthPara() { return m_blurStrength; }

signals:

public slots:

protected:
  virtual void compile() override;
  QString generateHeader();

  virtual void render(Z3DEye eye) override;

  const Z3DTexture *m_colorTexture = nullptr;
  const Z3DTexture *m_depthTexture = nullptr;

  Z3DRenderTarget m_blurXTarget;
  Z3DRenderTarget m_blurYTarget;

  Z3DShaderProgram m_blurXTextureShader;
  Z3DShaderProgram m_blurYTextureShader;
  Z3DShaderGroup m_glowTextureShaderGrp;

  ZStringStringOptionParameter m_glowMode;
  ZIntParameter m_blurRadius;
  ZFloatParameter m_blurScale;
  ZFloatParameter m_blurStrength;

  ZVertexArrayObject m_VAO;
};

} // namespace nim

#endif // Z3DTEXTUREGLOWRENDERER_H
