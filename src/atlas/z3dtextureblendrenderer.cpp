#include "z3dtextureblendrenderer.h"

#include "z3dtexture.h"

namespace nim {

Z3DTextureBlendRenderer::Z3DTextureBlendRenderer(Z3DRendererBase& rendererBase, TextureBlendMode mode)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_blendMode(mode)
  , m_VAO(1)
{
  m_blendTextureShader.loadFromSourceFile("pass.vert",
                                          "compositor.frag",
                                          m_rendererBase.generateHeader() + generateHeader());
}

void Z3DTextureBlendRenderer::compile()
{
  m_blendTextureShader.setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
}

QString Z3DTextureBlendRenderer::generateHeader()
{
  QString define;
  switch (m_blendMode) {
    case TextureBlendMode::DepthTest:
      define = QStringLiteral("DEPTH_TEST");
      break;
    case TextureBlendMode::FirstOnTop:
      define = QStringLiteral("FIRST_ON_TOP");
      break;
    case TextureBlendMode::SecondOnTop:
      define = QStringLiteral("SECOND_ON_TOP");
      break;
    case TextureBlendMode::DepthTestBlending:
      define = QStringLiteral("DEPTH_TEST_BLENDING");
      break;
    case TextureBlendMode::FirstOnTopBlending:
      define = QStringLiteral("FIRST_ON_TOP_BLENDING");
      break;
    case TextureBlendMode::SecondOnTopBlending:
      define = QStringLiteral("SECOND_ON_TOP_BLENDING");
      break;
    case TextureBlendMode::MIPImageDepthTestBlending:
      define = QStringLiteral("MIP_IMAGE_DEPTH_TEST_BLENDING");
      break;
  }
  return QString("#define %1\n").arg(define);
}

void Z3DTextureBlendRenderer::render(Z3DEye eye)
{
  if (!m_colorTexture1 || !m_depthTexture1 || !m_colorTexture2 || !m_depthTexture2) {
    return;
  }

  m_blendTextureShader.bind();
  m_rendererBase.setGlobalShaderParameters(m_blendTextureShader, eye);

  m_blendTextureShader.bindTexture("color_texture_0", m_colorTexture1);
  m_blendTextureShader.bindTexture("depth_texture_0", m_depthTexture1);

  m_blendTextureShader.bindTexture("color_texture_1", m_colorTexture2);
  m_blendTextureShader.bindTexture("depth_texture_1", m_depthTexture2);

  glDepthFunc(GL_ALWAYS);
  renderScreenQuad(m_VAO, m_blendTextureShader);
  glDepthFunc(GL_LESS);
  m_blendTextureShader.release();
}

void Z3DTextureBlendRenderer::setBlendMode(TextureBlendMode mode)
{
  if (m_blendMode == mode) {
    return;
  }
  m_blendMode = mode;
  compile();
}

} // namespace nim
