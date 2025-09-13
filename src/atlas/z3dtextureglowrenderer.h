#pragma once

#include "z3dprimitiverenderer.h"
#include "z3drendertarget.h"
#include "z3dshaderprogram.h"

namespace nim {

class Z3DTextureGlowRenderer : public Z3DPrimitiveRenderer
{
public:
  explicit Z3DTextureGlowRenderer(Z3DRendererBase& rendererBase);

  void setColorTexture(const Z3DTexture* colorTex)
  {
    m_colorTexture = colorTex;
  }

  void setDepthTexture(const Z3DTexture* depthTex)
  {
    m_depthTexture = depthTex;
  }

  ZStringStringOptionParameter& glowModePara()
  {
    return m_glowMode;
  }

  ZIntParameter& blurRadiusPara()
  {
    return m_blurRadius;
  }

  ZFloatParameter& blurScalePara()
  {
    return m_blurScale;
  }

  ZFloatParameter& blurStrengthPara()
  {
    return m_blurStrength;
  }

protected:
  void compile() override;

  QString generateHeader();

  void render(Z3DEye eye) override;

protected:
  const Z3DTexture* m_colorTexture = nullptr;
  const Z3DTexture* m_depthTexture = nullptr;

  Z3DShaderProgram m_blurXTextureShader;
  Z3DShaderProgram m_blurYTextureShader;
  Z3DShaderGroup m_glowTextureShaderGrp;

  ZStringStringOptionParameter m_glowMode;
  ZIntParameter m_blurRadius;
  ZFloatParameter m_blurScale;
  ZFloatParameter m_blurStrength;

  Z3DVertexArrayObject m_VAO;
};

} // namespace nim
