#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dshaderprogram.h"
#include <memory>
#include <string>

namespace nim {

enum class TextureBlendMode
{
  DepthTest,
  FirstOnTop,
  SecondOnTop,
  DepthTestBlending,
  FirstOnTopBlending,
  SecondOnTopBlending,
  MIPImageDepthTestBlending
};

class Z3DTextureBlendRenderer : public Z3DPrimitiveRenderer
{
public:
  // supported modes:
  // "DepthTest", "FirstOnTop", "SecondOnTop"
  // "DepthTestBlending", "FirstOnTopBlending", "SecondOnTopBlending"
  // "MIPImageDepthTestBlending"
  explicit Z3DTextureBlendRenderer(Z3DRendererBase& rendererBase,
                                   TextureBlendMode mode = TextureBlendMode::DepthTestBlending);

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

  void setBlendMode(TextureBlendMode mode);

protected:
  void compile() override;

  [[nodiscard]] std::string generateHeader();

  void render(Z3DEye eye) override;

protected:
  const Z3DTexture* m_colorTexture1 = nullptr;
  const Z3DTexture* m_depthTexture1 = nullptr;
  const Z3DTexture* m_colorTexture2 = nullptr;
  const Z3DTexture* m_depthTexture2 = nullptr;

  void createResources(RenderBackend backend) override;

  void destroyResources() override;

  std::unique_ptr<Z3DShaderProgram> m_blendTextureShader;

  TextureBlendMode m_blendMode;
  std::unique_ptr<Z3DVertexArrayObject> m_VAO;
};

} // namespace nim
