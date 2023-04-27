#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dshaderprogram.h"

namespace nim {

class Z3DTextureBlendRenderer : public Z3DPrimitiveRenderer
{
public:
  // supported modes:
  // "DepthTest", "FirstOnTop", "SecondOnTop"
  // "DepthTestBlending", "FirstOnTopBlending", "SecondOnTopBlending"
  // "MIPImageDepthTestBlending"
  explicit Z3DTextureBlendRenderer(Z3DRendererBase& rendererBase, const QString& mode = "DepthTestBlending");

  void setColorTexture1(const Z3DTexture* colorTex)
  {
    m_colorTexture1 = colorTex;
  }

  void setDepthTexture1(const Z3DTexture* depthTex)
  {
    m_depthTexture1 = depthTex;
  }

  void setColorTexture2(const Z3DTexture* colorTex)
  {
    m_colorTexture2 = colorTex;
  }

  void setDepthTexture2(const Z3DTexture* depthTex)
  {
    m_depthTexture2 = depthTex;
  }

  ZStringStringOptionParameter& blendModePara()
  {
    return m_blendMode;
  }

protected:
  void compile() override;

  QString generateHeader();

  void render(Z3DEye eye) override;

protected:
  const Z3DTexture* m_colorTexture1 = nullptr;
  const Z3DTexture* m_depthTexture1 = nullptr;
  const Z3DTexture* m_colorTexture2 = nullptr;
  const Z3DTexture* m_depthTexture2 = nullptr;

  Z3DShaderProgram m_blendTextureShader;

  ZStringStringOptionParameter m_blendMode;
  Z3DVertexArrayObject m_VAO;
};

} // namespace nim
