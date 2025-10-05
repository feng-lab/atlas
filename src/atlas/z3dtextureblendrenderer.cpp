#include "z3dtextureblendrenderer.h"

#include "z3dtexture.h"
#include "zlog.h"

namespace nim {

Z3DTextureBlendRenderer::Z3DTextureBlendRenderer(Z3DRendererBase& rendererBase, TextureBlendMode mode)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_blendMode(mode)
{
  createResources(m_rendererBase.activeBackend());
}

void Z3DTextureBlendRenderer::compile()
{
  if (m_rendererBase.activeBackend() != RenderBackend::OpenGL) {
    return;
  }
  DCHECK(m_blendTextureShader != nullptr);
  m_blendTextureShader->setHeaderAndRebuild(m_rendererBase.generateHeader() + generateHeader());
}

std::string Z3DTextureBlendRenderer::generateHeader()
{
  const char* define = "";
  switch (m_blendMode) {
    case TextureBlendMode::DepthTest:
      define = "DEPTH_TEST";
      break;
    case TextureBlendMode::FirstOnTop:
      define = "FIRST_ON_TOP";
      break;
    case TextureBlendMode::SecondOnTop:
      define = "SECOND_ON_TOP";
      break;
    case TextureBlendMode::DepthTestBlending:
      define = "DEPTH_TEST_BLENDING";
      break;
    case TextureBlendMode::FirstOnTopBlending:
      define = "FIRST_ON_TOP_BLENDING";
      break;
    case TextureBlendMode::SecondOnTopBlending:
      define = "SECOND_ON_TOP_BLENDING";
      break;
    case TextureBlendMode::MIPImageDepthTestBlending:
      define = "MIP_IMAGE_DEPTH_TEST_BLENDING";
      break;
  }
  if (*define == '\0') {
    return {};
  }
  return fmt::format("#define {}\n", define);
}

void Z3DTextureBlendRenderer::render(Z3DEye eye)
{
  if (!m_colorTexture1 || !m_depthTexture1 || !m_colorTexture2 || !m_depthTexture2) {
    return;
  }

  m_blendTextureShader->bind();
  m_rendererBase.setGlobalShaderParameters(*m_blendTextureShader, eye);

  m_blendTextureShader->bindTexture("color_texture_0", m_colorTexture1);
  m_blendTextureShader->bindTexture("depth_texture_0", m_depthTexture1);

  m_blendTextureShader->bindTexture("color_texture_1", m_colorTexture2);
  m_blendTextureShader->bindTexture("depth_texture_1", m_depthTexture2);

  glDepthFunc(GL_ALWAYS);
  renderScreenQuad(*m_VAO, *m_blendTextureShader);
  glDepthFunc(GL_LESS);
  m_blendTextureShader->release();
}

TextureBlendPayload Z3DTextureBlendRenderer::buildTextureBlendPayload() const
{
  TextureBlendPayload payload;
  payload.renderer = const_cast<Z3DTextureBlendRenderer*>(this);
  payload.colorTexture0 = m_colorTexture1;
  payload.depthTexture0 = m_depthTexture1;
  payload.colorTexture1 = m_colorTexture2;
  payload.depthTexture1 = m_depthTexture2;
  payload.mode = m_blendMode;
  payload.colorAttachmentHandle0 = m_colorAttachmentHandle0;
  payload.depthAttachmentHandle0 = m_depthAttachmentHandle0;
  payload.colorAttachmentHandle1 = m_colorAttachmentHandle1;
  payload.depthAttachmentHandle1 = m_depthAttachmentHandle1;
  return payload;
}

RenderBatch Z3DTextureBlendRenderer::buildRenderBatch(Z3DEye eye) const
{
  RenderBatch batch;

  batch.eye = eye;

  const glm::uvec4 viewport = m_rendererBase.frameState().viewport;
  batch.pass.extent = glm::uvec2(viewport.z, viewport.w);
  batch.pass.viewport.origin = glm::vec2(static_cast<float>(viewport.x), static_cast<float>(viewport.y));
  batch.pass.viewport.extent = glm::vec2(static_cast<float>(viewport.z), static_cast<float>(viewport.w));
  batch.pass.viewport.minDepth = 0.0f;
  batch.pass.viewport.maxDepth = 1.0f;

  const auto& surface = m_rendererBase.frameState().activeSurface;
  batch.pass.colorAttachments = surface.colorAttachments;
  batch.pass.depthAttachment = surface.depthAttachment;

  batch.draw.topology = PrimitiveTopology::TriangleStrip;
  batch.draw.vertexCount = 4;
  batch.draw.indexCount = 0;

  batch.geometry = buildTextureBlendPayload();

  return batch;
}

void Z3DTextureBlendRenderer::enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking)
{
  if (backend != RenderBackend::Vulkan || picking) {
    return;
  }

  if (!m_colorAttachmentHandle0.valid() || !m_depthAttachmentHandle0.valid() ||
      !m_colorAttachmentHandle1.valid() || !m_depthAttachmentHandle1.valid()) {
    LOG_FIRST_N(WARNING, 5) << "Texture blend renderer missing Vulkan attachment handles.";
    return;
  }

  auto batch = buildRenderBatch(eye);
  m_rendererBase.appendBatch(std::move(batch));
}

void Z3DTextureBlendRenderer::renderVulkan(Z3DEye eye,
                                           AttachmentHandle colorHandle0,
                                           AttachmentHandle depthHandle0,
                                           AttachmentHandle colorHandle1,
                                           AttachmentHandle depthHandle1)
{
  setSourceAttachments0(colorHandle0, depthHandle0);
  setSourceAttachments1(colorHandle1, depthHandle1);
  auto batch = buildRenderBatch(eye);
  m_rendererBase.appendBatch(std::move(batch));
}

void Z3DTextureBlendRenderer::setBlendMode(TextureBlendMode mode)
{
  if (m_blendMode == mode) {
    return;
  }
  m_blendMode = mode;
  compile();
}

void Z3DTextureBlendRenderer::createResources(RenderBackend backend)
{
  if (backend != RenderBackend::OpenGL) {
    return;
  }
  m_blendTextureShader = std::make_unique<Z3DShaderProgram>();
  m_blendTextureShader->loadFromSourceFile("pass.vert",
                                           "compositor.frag",
                                           m_rendererBase.generateHeader() + generateHeader());

  m_VAO = std::make_unique<Z3DVertexArrayObject>(1);
}

void Z3DTextureBlendRenderer::destroyResources()
{
  m_blendTextureShader.reset();
  m_VAO.reset();
}

} // namespace nim
