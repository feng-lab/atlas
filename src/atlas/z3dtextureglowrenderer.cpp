#include "z3dtextureglowrenderer.h"

#include "z3dtexture.h"

namespace nim {

Z3DTextureGlowRenderer::Z3DTextureGlowRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_glowTextureShaderGrp(rendererBase)
  , m_glowMode("Glow Mode")
  , m_blurRadius("Glow Blur Radius", 10, 2, 10)
  , m_blurScale("Glow Blur Scale", 1.f, 1.f, 5.f)
  , m_blurStrength("Glow Blur Strength", .5f, 0.f, 1.f)
  , m_VAO(1)
{
  m_blurScale.setSingleStep(0.5);
  m_glowMode.addOptionsWithData(qMakePair<QString, QString>("Additive", "ADDITIVE_BLENDING"),
                                qMakePair<QString, QString>("Screen", "SCREEN_BLENDING"),
                                qMakePair<QString, QString>("Softlight", "SOFTLIGHT_BLENDING"),
                                qMakePair<QString, QString>("Glowmap", "GLOWMAP")
  );
  m_glowMode.select("Screen");
  connect(&m_glowMode, &ZStringStringOptionParameter::valueChanged, this, &Z3DTextureGlowRenderer::compile);

  m_blurXTextureShader.bindFragDataLocation(0, "FragData0");
  m_blurXTextureShader.loadFromSourceFile("pass.vert", "blur.frag",
                                          m_rendererBase.generateHeader() + "#define ORIENTATION_X\n");
  m_blurYTextureShader.bindFragDataLocation(0, "FragData0");
  m_blurYTextureShader.loadFromSourceFile("pass.vert", "blur.frag",
                                          m_rendererBase.generateHeader() + "#define ORIENTATION_Y\n");

  QStringList allshaders;
  allshaders << "pass.vert" << "glow_func.frag";
  m_glowTextureShaderGrp.init(allshaders, m_rendererBase.generateHeader() + generateHeader());
  m_glowTextureShaderGrp.addAllSupportedPostShaders();
  CHECK_GL_ERROR;
}

void Z3DTextureGlowRenderer::compile()
{
  m_glowTextureShaderGrp.rebuild(m_rendererBase.generateHeader() + generateHeader());
}

QString Z3DTextureGlowRenderer::generateHeader()
{
  return QString("#define %1\n").arg(m_glowMode.associatedData());
}

void Z3DTextureGlowRenderer::render(Z3DEye eye)
{
  if (!m_colorTexture || !m_depthTexture)
    return;

  glm::uvec2 size = m_colorTexture->dimension().xy();
  //glm::ivec2 size = glm::ivec2(glm::vec2(m_colorTexture->dimensions()) * 128.f / float(std::min(m_colorTexture->dimensions().x, m_colorTexture->dimensions().y)));
  m_blurXTarget.resize(size);
  m_blurYTarget.resize(size);

  m_blurXTarget.bind();
  m_blurXTarget.clear();
  m_blurXTextureShader.bind();
  m_blurXTextureShader.setUniform("blur_radius", m_blurRadius.get());
  m_blurXTextureShader.setUniform("blur_scale", m_blurScale.get());
  m_blurXTextureShader.setUniform("blur_strength", m_blurStrength.get());
  m_blurXTextureShader.setUniform("screen_dim_RCP", 1.f / glm::vec2(size));
  m_blurXTextureShader.bindTexture("color_texture", m_colorTexture);
  m_blurXTextureShader.bindTexture("depth_texture", m_depthTexture);
  renderScreenQuad(m_VAO, m_blurXTextureShader);
  m_blurXTextureShader.release();
  m_blurXTarget.release();

  m_blurYTarget.bind();
  m_blurYTarget.clear();
  m_blurYTextureShader.bind();
  m_blurYTextureShader.setUniform("blur_radius", m_blurRadius.get());
  m_blurYTextureShader.setUniform("blur_scale", m_blurScale.get());
  m_blurYTextureShader.setUniform("blur_strength", m_blurStrength.get());
  m_blurYTextureShader.setUniform("screen_dim_RCP", 1.f / glm::vec2(size));
  m_blurYTextureShader.bindTexture("color_texture", m_blurXTarget.attachment(GL_COLOR_ATTACHMENT0));
  m_blurYTextureShader.bindTexture("depth_texture", m_blurXTarget.attachment(GL_DEPTH_ATTACHMENT));
  renderScreenQuad(m_VAO, m_blurYTextureShader);
  m_blurYTextureShader.release();
  m_blurYTarget.release();

  m_glowTextureShaderGrp.bind();
  Z3DShaderProgram& shader = m_glowTextureShaderGrp.get();
  m_rendererBase.setGlobalShaderParameters(shader, eye);
  shader.bindTexture("color_texture", m_colorTexture);
  shader.bindTexture("depth_texture", m_depthTexture);
  shader.bindTexture("glowmap_color_texture", m_blurYTarget.attachment(GL_COLOR_ATTACHMENT0));
  shader.bindTexture("glowmap_depth_texture", m_blurYTarget.attachment(GL_DEPTH_ATTACHMENT));
  renderScreenQuad(m_VAO, shader);
  m_glowTextureShaderGrp.release();
}

} // namespace nim
