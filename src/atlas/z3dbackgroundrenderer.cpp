#include "z3dbackgroundrenderer.h"

#include "z3dgl.h"
#include "z3dgpuinfo.h"
#include "z3dshaderprogram.h"
#include "zlog.h"
#include <QObject>

namespace nim {

Z3DBackgroundRenderer::Z3DBackgroundRenderer(Z3DRendererBase& rendererBase)
  : Z3DPrimitiveRenderer(rendererBase)
  , m_backgroundShaderGrp(rendererBase)
  , m_VAO(1)
{
  QStringList allshaders;
  allshaders << "pass.vert"
             << "background_func.frag";
  QStringList normalShaders;
  normalShaders << "pass.vert"
                << "background.frag";
  m_backgroundShaderGrp.init(allshaders, m_rendererBase.generateHeader() + generateHeader(), "", normalShaders);
  m_backgroundShaderGrp.addAllSupportedPostShaders();

  if (m_useVAO) {
    m_VAO.bind();
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
    auto attr_vertex = m_backgroundShaderGrp.get().vertexAttributeLocation();

    glEnableVertexAttribArray(attr_vertex);
    m_VBO.bind(GL_ARRAY_BUFFER);
    glBufferData(GL_ARRAY_BUFFER, 3 * 4 * sizeof(GLfloat), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    m_VBO.release(GL_ARRAY_BUFFER);

    m_VAO.release();
  }
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
  m_backgroundShaderGrp.rebuild(m_rendererBase.generateHeader() + generateHeader());
}

QString Z3DBackgroundRenderer::generateHeader()
{
  QString headerSource;
  if (m_modeValue == BackgroundMode::Uniform) {
    headerSource += "#define UNIFORM\n";
  } else {
    switch (m_orientationValue) {
      case BackgroundGradientOrientation::LeftToRight:
        headerSource += "#define GRADIENT_LEFT_TO_RIGHT\n";
        break;
      case BackgroundGradientOrientation::RightToLeft:
        headerSource += "#define GRADIENT_RIGHT_TO_LEFT\n";
        break;
      case BackgroundGradientOrientation::TopToBottom:
        headerSource += "#define GRADIENT_TOP_TO_BOTTOM\n";
        break;
      case BackgroundGradientOrientation::BottomToTop:
        headerSource += "#define GRADIENT_BOTTOM_TO_TOP\n";
        break;
    }
  }
  return headerSource;
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
  m_backgroundShaderGrp.bind();
  Z3DShaderProgram& shader = m_backgroundShaderGrp.get();
  m_rendererBase.setGlobalShaderParameters(shader, eye);

  shader.setColor1Uniform(m_firstColorValue);
  if (m_modeValue != BackgroundMode::Uniform) {
    shader.setColor2Uniform(m_secondColorValue);
    shader.setRegionUniform(m_region);
  }

  if (m_useVAO) {
    m_VAO.bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_VAO.release();
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
    m_VBO.bind(GL_ARRAY_BUFFER);
    glBufferData(GL_ARRAY_BUFFER, 3 * 4 * sizeof(GLfloat), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(attr_vertex, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    m_VBO.release(GL_ARRAY_BUFFER);

    glDisableVertexAttribArray(attr_vertex);
  }

  m_backgroundShaderGrp.release();
}

void Z3DBackgroundRenderer::renderPicking(Z3DEye)
{
  // do nothing
}

} // namespace nim
