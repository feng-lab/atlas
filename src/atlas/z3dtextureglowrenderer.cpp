#include "z3dtextureglowrenderer.h"

#include "z3dtexture.h"
#include "z3drendertarget.h"
#include "z3dscratchresourcepool.h"
#include "z3drenderglobalstate.h"
#include "zlog.h"
#include <algorithm>

namespace nim {

Z3DTextureGlowRenderer::Z3DTextureGlowRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
{
  createResources(m_rendererBase.activeBackend());
}

void Z3DTextureGlowRenderer::compile()
{
  if (m_rendererBase.activeBackend() != RenderBackend::OpenGL) {
    return;
  }
  DCHECK(m_glowTextureShaderGrp != nullptr);
  m_glowTextureShaderGrp->rebuild(m_rendererBase.generateHeader() + generateHeader());
}

std::string Z3DTextureGlowRenderer::generateHeader()
{
  const char* define = "";
  switch (m_glowMode) {
    case GlowMode::Additive:
      define = "ADDITIVE_BLENDING";
      break;
    case GlowMode::Screen:
      define = "SCREEN_BLENDING";
      break;
    case GlowMode::Softlight:
      define = "SOFTLIGHT_BLENDING";
      break;
    case GlowMode::Glowmap:
      define = "GLOWMAP";
      break;
  }
  if (*define == '\0') {
    return {};
  }
  return fmt::format("#define {}\n", define);
}

void Z3DTextureGlowRenderer::render(Z3DEye eye)
{
  if (!m_colorTexture || !m_depthTexture) {
    return;
  }

  glm::uvec2 size = m_colorTexture->dimension().xy();

  auto& scratchPool = Z3DRenderGlobalState::instance().scratchPool();

  // Acquire temporary targets from the scratch pool for blur passes
  auto blurXLease = scratchPool.acquireTempRenderTarget2D(size);
  auto blurYLease = scratchPool.acquireTempRenderTarget2D(size);

  blurXLease.renderTarget->bind();
  blurXLease.renderTarget->clear();
  m_blurXTextureShader->bind();
  m_blurXTextureShader->setUniform("blur_radius", m_blurRadius);
  m_blurXTextureShader->setUniform("blur_scale", m_blurScale);
  m_blurXTextureShader->setUniform("blur_strength", m_blurStrength);
  m_blurXTextureShader->setUniform("screen_dim_RCP", 1.f / glm::vec2(size));
  m_blurXTextureShader->bindTexture("color_texture", m_colorTexture);
  m_blurXTextureShader->bindTexture("depth_texture", m_depthTexture);
  renderScreenQuad(*m_VAO, *m_blurXTextureShader);
  m_blurXTextureShader->release();
  blurXLease.renderTarget->release();

  blurYLease.renderTarget->bind();
  blurYLease.renderTarget->clear();
  m_blurYTextureShader->bind();
  m_blurYTextureShader->setUniform("blur_radius", m_blurRadius);
  m_blurYTextureShader->setUniform("blur_scale", m_blurScale);
  m_blurYTextureShader->setUniform("blur_strength", m_blurStrength);
  m_blurYTextureShader->setUniform("screen_dim_RCP", 1.f / glm::vec2(size));
  m_blurYTextureShader->bindTexture("color_texture", blurXLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
  m_blurYTextureShader->bindTexture("depth_texture", blurXLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
  renderScreenQuad(*m_VAO, *m_blurYTextureShader);
  m_blurYTextureShader->release();
  blurYLease.renderTarget->release();

  m_glowTextureShaderGrp->bind();
  Z3DShaderProgram& shader = m_glowTextureShaderGrp->get();
  m_rendererBase.setGlobalShaderParameters(shader, eye);
  shader.bindTexture("color_texture", m_colorTexture);
  shader.bindTexture("depth_texture", m_depthTexture);
  shader.bindTexture("glowmap_color_texture", blurYLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
  shader.bindTexture("glowmap_depth_texture", blurYLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
  renderScreenQuad(*m_VAO, shader);
  m_glowTextureShaderGrp->release();
}

TextureGlowPayload Z3DTextureGlowRenderer::buildTextureGlowPayload() const
{
  TextureGlowPayload payload;
  payload.mode = m_glowMode;
  payload.blurRadius = m_blurRadius;
  payload.blurScale = m_blurScale;
  payload.blurStrength = m_blurStrength;
  payload.colorAttachmentHandle = m_colorAttachmentHandle;
  payload.depthAttachmentHandle = m_depthAttachmentHandle;
  return payload;
}

RenderBatch Z3DTextureGlowRenderer::buildRenderBatch(Z3DEye eye) const
{
  RenderBatch batch;

  batch.eye = eye;

  const glm::uvec4 viewport = m_rendererBase.frameState().viewport;
  batch.pass.viewport.origin = glm::vec2(static_cast<float>(viewport.x), static_cast<float>(viewport.y));
  batch.pass.viewport.extent = glm::vec2(static_cast<float>(viewport.z), static_cast<float>(viewport.w));
  batch.pass.viewport.minDepth = 0.0f;
  batch.pass.viewport.maxDepth = 1.0f;

  const auto& surface = m_rendererBase.frameState().activeSurface;
  batch.pass.colorAttachments = surface.colorAttachments;
  batch.pass.depthAttachment = surface.depthAttachment;

  CHECK(m_colorAttachmentHandle.valid() && m_depthAttachmentHandle.valid())
    << "Texture glow renderer missing Vulkan input attachment handles.";
  batch.pass.externalImageUses.push_back(
    {m_colorAttachmentHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
  batch.pass.externalImageUses.push_back(
    {m_depthAttachmentHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Depth});

  batch.draw.topology = PrimitiveTopology::TriangleStrip;
  batch.draw.vertexCount = 4;
  batch.draw.indexCount = 0;

  batch.geometry = buildTextureGlowPayload();

  return batch;
}

void Z3DTextureGlowRenderer::enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking)
{
  if (backend != RenderBackend::Vulkan || picking) {
    return;
  }

  CHECK(m_colorAttachmentHandle.valid() && m_depthAttachmentHandle.valid())
    << "Texture glow renderer missing Vulkan attachment handles.";

  auto batch = buildRenderBatch(eye);
  m_rendererBase.appendBatch(std::move(batch));
}

void Z3DTextureGlowRenderer::renderVulkan(Z3DEye eye, AttachmentHandle colorHandle, AttachmentHandle depthHandle)
{
  setSourceAttachments(colorHandle, depthHandle);
  auto batch = buildRenderBatch(eye);
  m_rendererBase.appendBatch(std::move(batch));
}

void Z3DTextureGlowRenderer::setGlowMode(GlowMode mode)
{
  if (m_glowMode == mode) {
    return;
  }
  m_glowMode = mode;
  compile();
}

void Z3DTextureGlowRenderer::setBlurRadius(int radius)
{
  m_blurRadius = std::max(0, radius);
}

void Z3DTextureGlowRenderer::setBlurScale(float scale)
{
  m_blurScale = std::max(0.f, scale);
}

void Z3DTextureGlowRenderer::setBlurStrength(float strength)
{
  m_blurStrength = strength;
}

void Z3DTextureGlowRenderer::createResources(RenderBackend backend)
{
  if (backend != RenderBackend::OpenGL) {
    return;
  }
  m_blurXTextureShader = std::make_unique<Z3DShaderProgram>();
  m_blurYTextureShader = std::make_unique<Z3DShaderProgram>();
  m_blurXTextureShader->loadFromSourceFile("pass.vert",
                                           "blur.frag",
                                           m_rendererBase.generateHeader() + "#define ORIENTATION_X\n");
  m_blurYTextureShader->loadFromSourceFile("pass.vert",
                                           "blur.frag",
                                           m_rendererBase.generateHeader() + "#define ORIENTATION_Y\n");

  m_glowTextureShaderGrp = std::make_unique<Z3DShaderGroup>(m_rendererBase);
  QStringList allshaders;
  allshaders << "pass.vert"
             << "glow_func.frag";
  m_glowTextureShaderGrp->init(allshaders, m_rendererBase.generateHeader() + generateHeader());
  m_glowTextureShaderGrp->addAllSupportedPostShaders();
  CHECK_GL_ERROR

  m_VAO = std::make_unique<Z3DVertexArrayObject>(1);
}

void Z3DTextureGlowRenderer::destroyResources()
{
  m_blurXTextureShader.reset();
  m_blurYTextureShader.reset();
  m_glowTextureShaderGrp.reset();
  m_VAO.reset();
}

} // namespace nim
