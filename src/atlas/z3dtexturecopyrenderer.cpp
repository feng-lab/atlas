#include "z3dtexturecopyrenderer.h"

#include "z3dshaderprogram.h"
#include "z3dtexture.h"
#include "zlog.h"

namespace nim {

Z3DTextureCopyRenderer::Z3DTextureCopyRenderer(Z3DRendererBase& rendererBase, OutputColorOption mode)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_mode(mode)
{
  createResources(m_rendererBase.activeBackend());
}

void Z3DTextureCopyRenderer::compile()
{
  if (m_rendererBase.activeBackend() != RenderBackend::OpenGL) {
    return;
  }
  DCHECK(m_copyTextureShaderGrp != nullptr);
  m_copyTextureShaderGrp->rebuild(m_rendererBase.generateHeader() + generateHeader());
}

std::string Z3DTextureCopyRenderer::generateHeader() const
{
  switch (m_mode) {
    case OutputColorOption::MultiplyAlpha:
      return "#define Multiply_Alpha\n";
    case OutputColorOption::DivideByAlpha:
      return "#define Divide_By_Alpha\n";
    case OutputColorOption::NoChange:
    default:
      return {};
  }
}

void Z3DTextureCopyRenderer::render(Z3DEye eye)
{
  if (!m_colorTexture || !m_depthTexture) {
    return;
  }

  m_copyTextureShaderGrp->bind();
  Z3DShaderProgram& shader = m_copyTextureShaderGrp->get();
  m_rendererBase.setGlobalShaderParameters(shader, eye);
  shader.setUniform("discard_transparent", m_discardTransparent);

  // pass texture parameters to the shader
  // Match Vulkan's bindless linear-clamp sampling for fullscreen copy/resolve
  // passes. Scratch render targets are created with NEAREST filtering for
  // generic intermediate use, but the AA resolve path expects filtered
  // downsampling when sampling a larger source into a smaller output.
  shader.bindTexture("color_texture", m_colorTexture, GLint(GL_LINEAR), GLint(GL_LINEAR));
  shader.bindTexture("depth_texture", m_depthTexture, GLint(GL_LINEAR), GLint(GL_LINEAR));

  renderScreenQuad(*m_VAO, shader);
  m_copyTextureShaderGrp->release();
}

TextureCopyPayload Z3DTextureCopyRenderer::buildTextureCopyPayload() const
{
  TextureCopyPayload payload;
  payload.discardTransparent = m_discardTransparent;
  payload.flipY = m_flipY;
  payload.colorAttachmentHandle = m_colorAttachmentHandle;
  payload.depthAttachmentHandle = m_depthAttachmentHandle;

  switch (m_mode) {
    case OutputColorOption::DivideByAlpha:
      payload.mode = TextureCopyPayload::OutputMode::DivideByAlpha;
      break;
    case OutputColorOption::MultiplyAlpha:
      payload.mode = TextureCopyPayload::OutputMode::MultiplyAlpha;
      break;
    case OutputColorOption::NoChange:
    default:
      payload.mode = TextureCopyPayload::OutputMode::NoChange;
      break;
  }

  return payload;
}

RenderBatch Z3DTextureCopyRenderer::buildRenderBatch(Z3DEye eye) const
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
    << "Texture copy renderer missing Vulkan input attachment handles.";
  batch.pass.externalImageUses.push_back(
    {m_colorAttachmentHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Color});
  batch.pass.externalImageUses.push_back(
    {m_depthAttachmentHandle, ExternalImageUseKind::SampledRead, ExternalImageAspectHint::Depth});

  batch.draw.topology = PrimitiveTopology::TriangleStrip;
  batch.draw.vertexCount = 4;
  batch.draw.indexCount = 0;

  batch.geometry = buildTextureCopyPayload();

  return batch;
}

void Z3DTextureCopyRenderer::enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking)
{
  if (backend != RenderBackend::Vulkan || picking) {
    return;
  }

  CHECK(m_colorAttachmentHandle.valid() && m_depthAttachmentHandle.valid())
    << "Texture copy renderer missing Vulkan attachment handles.";

  auto batch = buildRenderBatch(eye);
  m_rendererBase.appendBatch(std::move(batch));
}

void Z3DTextureCopyRenderer::renderVulkan(Z3DEye eye, AttachmentHandle colorHandle, AttachmentHandle depthHandle)
{
  setSourceAttachments(colorHandle, depthHandle);
  auto batch = buildRenderBatch(eye);
  m_rendererBase.appendBatch(std::move(batch));
}

void Z3DTextureCopyRenderer::createResources(RenderBackend backend)
{
  if (backend != RenderBackend::OpenGL) {
    return;
  }
  m_copyTextureShaderGrp = std::make_unique<Z3DShaderGroup>(m_rendererBase);
  QStringList allshaders;
  allshaders << "pass.vert"
             << "copyimage_func.frag";
  QStringList normalShaders;
  normalShaders << "pass.vert"
                << "copyimage.frag";
  m_copyTextureShaderGrp->init(allshaders, m_rendererBase.generateHeader() + generateHeader(), "", normalShaders);
  m_copyTextureShaderGrp->addAllSupportedPostShaders();

  m_VAO = std::make_unique<Z3DVertexArrayObject>(1);
}

void Z3DTextureCopyRenderer::destroyResources()
{
  m_copyTextureShaderGrp.reset();
  m_VAO.reset();
}

} // namespace nim
