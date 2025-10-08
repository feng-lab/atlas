#include "z3dbackgroundrenderer.h"

#include "z3dgl.h"
#include "z3dgpuinfo.h"
#include "z3dshaderprogram.h"
#include "zlog.h"

#include <utility>

namespace nim {

Z3DBackgroundRenderer::Z3DBackgroundRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
{
  createResources(m_rendererBase.activeBackend());
}

void Z3DBackgroundRenderer::setMode(BackgroundMode mode)
{
  if (m_modeValue == mode) {
    return;
  }
  m_modeValue = mode;
  compile();
}

void Z3DBackgroundRenderer::setGradientOrientation(BackgroundGradientOrientation orientation)
{
  if (m_orientationValue == orientation) {
    return;
  }
  m_orientationValue = orientation;
  compile();
}

void Z3DBackgroundRenderer::setRenderingRegion(double left, double right, double bottom, double top)
{
  m_region = glm::vec4(left, right - left, bottom, top - bottom);
}

void Z3DBackgroundRenderer::compile()
{
  if (m_rendererBase.activeBackend() != RenderBackend::OpenGL) {
    return;
  }
  DCHECK(m_backgroundShaderGrp != nullptr);
  m_backgroundShaderGrp->rebuild(m_rendererBase.generateHeader() + generateHeader());
}

std::string Z3DBackgroundRenderer::generateHeader()
{
  if (m_modeValue == BackgroundMode::Uniform) {
    return "#define UNIFORM\n";
  }

  std::string header;
  switch (m_orientationValue) {
    case BackgroundGradientOrientation::LeftToRight:
      header = "#define GRADIENT_LEFT_TO_RIGHT\n";
      break;
    case BackgroundGradientOrientation::RightToLeft:
      header = "#define GRADIENT_RIGHT_TO_LEFT\n";
      break;
    case BackgroundGradientOrientation::TopToBottom:
      header = "#define GRADIENT_TOP_TO_BOTTOM\n";
      break;
    case BackgroundGradientOrientation::BottomToTop:
      header = "#define GRADIENT_BOTTOM_TO_TOP\n";
      break;
  }
  return header;
}

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
void Z3DBackgroundRenderer::renderUsingOpengl()
{
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();

  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  if (m_modeValue == BackgroundMode::Gradient) {
    switch (m_orientationValue) {
      case BackgroundGradientOrientation::LeftToRight:
        glRotatef(270.f, 0.0f, 0.0f, 1.0f);
        break;
      case BackgroundGradientOrientation::RightToLeft:
        glRotatef(90.f, 0.0f, 0.0f, 1.0f);
        break;
      case BackgroundGradientOrientation::TopToBottom:
        glRotatef(180.f, 0.0f, 0.0f, 1.0f);
        break;
      case BackgroundGradientOrientation::BottomToTop:
        glRotatef(0.f, 0.0f, 0.0f, 1.0f);
        break;
    }
  }

  glBegin(GL_QUADS);
  glColor4fv(&m_firstColorValue[0]);
  glVertex3f(-1.0, -1.0, 1.0 - 1e-5);
  glVertex3f(1.0, -1.0, 1.0 - 1e-5);
  if (m_modeValue == BackgroundMode::Gradient) {
    glColor4fv(&m_secondColorValue[0]);
  }
  glVertex3f(1.0, 1.0, 1.0 - 1e-5);
  glVertex3f(-1.0, 1.0, 1.0 - 1e-5);
  glEnd();

  glMatrixMode(GL_PROJECTION);
  glPopMatrix();

  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
}

void Z3DBackgroundRenderer::renderPickingUsingOpengl()
{
  // do nothing
}
#endif

void Z3DBackgroundRenderer::render(Z3DEye eye)
{
  m_backgroundShaderGrp->bind();
  Z3DShaderProgram& shader = m_backgroundShaderGrp->get();
  m_rendererBase.setGlobalShaderParameters(shader, eye);

  shader.setColor1Uniform(m_firstColorValue);
  if (m_modeValue != BackgroundMode::Uniform) {
    shader.setColor2Uniform(m_secondColorValue);
    shader.setRegionUniform(m_region);
  }

  if (m_useVAO) {
    m_VAO->bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_VAO->release();
  } else {
    const GLfloat vertices[] = {-1.f,
                                1.f,
                                1.0f - 1e-5f, // top left corner
                                -1.f,
                                -1.f,
                                1.0f - 1e-5f, // bottom left corner
                                1.f,
                                1.f,
                                1.0f - 1e-5f, // top right corner
                                1.f,
                                -1.f,
                                1.0f - 1e-5f}; // bottom right rocner
    auto attr_vertex = shader.vertexAttributeLocation();

    glEnableVertexAttribArray(attr_vertex);
    m_VBO->bind(GL_ARRAY_BUFFER);
    glBufferData(GL_ARRAY_BUFFER, 3 * 4 * sizeof(GLfloat), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    m_VBO->release(GL_ARRAY_BUFFER);

    glDisableVertexAttribArray(attr_vertex);
  }

  m_backgroundShaderGrp->release();
}

BackgroundPayload Z3DBackgroundRenderer::buildBackgroundPayload() const
{
  BackgroundPayload payload;
  payload.renderer = const_cast<Z3DBackgroundRenderer*>(this);
  payload.color1 = m_firstColorValue;
  payload.color2 = m_secondColorValue;
  payload.region = m_region;
  payload.mode = m_modeValue;
  payload.orientation = m_orientationValue;
  payload.params = &m_rendererBase.parameterState();
  return payload;
}

RenderBatch Z3DBackgroundRenderer::buildRenderBatch(Z3DEye eye) const
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

  batch.draw.topology = PrimitiveTopology::TriangleStrip;
  batch.draw.vertexCount = 4;
  batch.draw.indexCount = 0;

  batch.geometry = buildBackgroundPayload();
  return batch;
}

void Z3DBackgroundRenderer::enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking)
{
  if (backend != RenderBackend::Vulkan || picking) {
    return;
  }

  auto batch = buildRenderBatch(eye);
  m_rendererBase.appendBatch(std::move(batch));
}

void Z3DBackgroundRenderer::createResources(RenderBackend backend)
{
  if (backend != RenderBackend::OpenGL) {
    return;
  }
  m_backgroundShaderGrp = std::make_unique<Z3DShaderGroup>(m_rendererBase);
  QStringList allshaders;
  allshaders << "pass.vert"
             << "background_func.frag";
  QStringList normalShaders;
  normalShaders << "pass.vert"
                << "background.frag";
  m_backgroundShaderGrp->init(allshaders, m_rendererBase.generateHeader() + generateHeader(), "", normalShaders);
  m_backgroundShaderGrp->addAllSupportedPostShaders();

  m_VBO = std::make_unique<Z3DVertexBufferObject>(1);

  if (m_useVAO) {
    m_VAO = std::make_unique<Z3DVertexArrayObject>(1);
    m_VAO->bind();
    const GLfloat vertices[] = {-1.f,
                                1.f,
                                1.0f - 1e-5f, // top left corner
                                -1.f,
                                -1.f,
                                1.0f - 1e-5f, // bottom left corner
                                1.f,
                                1.f,
                                1.0f - 1e-5f, // top right corner
                                1.f,
                                -1.f,
                                1.0f - 1e-5f}; // bottom right rocner
    auto attr_vertex = m_backgroundShaderGrp->get().vertexAttributeLocation();

    glEnableVertexAttribArray(attr_vertex);
    m_VBO->bind(GL_ARRAY_BUFFER);
    glBufferData(GL_ARRAY_BUFFER, 3 * 4 * sizeof(GLfloat), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    m_VBO->release(GL_ARRAY_BUFFER);

    m_VAO->release();
  } else {
    m_VAO.reset();
  }
}

void Z3DBackgroundRenderer::destroyResources()
{
  m_backgroundShaderGrp.reset();
  m_VAO.reset();
  m_VBO.reset();
}

void Z3DBackgroundRenderer::renderPicking(Z3DEye)
{
  // do nothing
}

} // namespace nim
