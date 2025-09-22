#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dshaderprogram.h"
#include <string>

namespace nim {

enum class GlowMode
{
  Additive,
  Screen,
  Softlight,
  Glowmap
};

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

  void setGlowMode(GlowMode mode);

  void setBlurRadius(int radius);

  void setBlurScale(float scale);

  void setBlurStrength(float strength);

protected:
  void compile() override;

  [[nodiscard]] std::string generateHeader();

  void render(Z3DEye eye) override;

protected:
  const Z3DTexture* m_colorTexture = nullptr;
  const Z3DTexture* m_depthTexture = nullptr;

  Z3DShaderProgram m_blurXTextureShader;
  Z3DShaderProgram m_blurYTextureShader;
  Z3DShaderGroup m_glowTextureShaderGrp;

  GlowMode m_glowMode = GlowMode::Screen;
  int m_blurRadius = 10;
  float m_blurScale = 1.f;
  float m_blurStrength = 0.5f;

  Z3DVertexArrayObject m_VAO;
};

} // namespace nim
