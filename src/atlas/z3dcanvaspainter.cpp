#include "z3dcanvaspainter.h"

#include "z3dgl.h"
#include "z3dcanvas.h"
#include "z3dcompositor.h"
#include "z3dgpuinfo.h"
#include "z3dtexture.h"
#include "zexception.h"
#include "zimgformat.h"
#include "zlog.h"
#include <QApplication>
#include <memory>

namespace nim {

Z3DCanvasPainter::Z3DCanvasPainter(Z3DGlobalParameters& globalParas, Z3DCanvas& canvas, QObject* parent)
  : Z3DBoundedFilter(globalParas, parent)
  , m_textureCopyRenderer(m_rendererBase, Z3DTextureCopyRenderer::OutputColorOption::DivideByAlpha)
  , m_canvas(canvas)
  , m_inport("Image", false, this, State::MonoViewResultInvalid)
  , m_leftEyeInport("LeftEyeImage", false, this, State::LeftEyeResultInvalid)
  , m_rightEyeInport("RightEyeImage", false, this, State::RightEyeResultInvalid)
{
  addPort(m_inport);
  addPort(m_leftEyeInport);
  addPort(m_rightEyeInport);

  setOutputSize(m_canvas.physicalSize());
  connect(&m_canvas, &Z3DCanvas::canvasSizeChanged, this, &Z3DCanvasPainter::onCanvasResized);
}

void Z3DCanvasPainter::process(Z3DEye eye)
{
  Z3DRenderInputPort& currentInport = (eye == Z3DEye::Mono)   ? m_inport
                                      : (eye == Z3DEye::Left) ? m_leftEyeInport
                                                              : m_rightEyeInport;

  // render to screen
  glViewport(0, 0, m_canvas.physicalSize().x, m_canvas.physicalSize().y);
  if (eye == Z3DEye::Left) {
    glDrawBuffer(GL_BACK_LEFT);
  } else if (eye == Z3DEye::Right) {
    glDrawBuffer(GL_BACK_RIGHT);
  }

  if (currentInport.isReady()) {
    m_rendererBase.setViewport(m_canvas.physicalSize());
    m_textureCopyRenderer.setColorTexture(currentInport.colorTexture());
    m_textureCopyRenderer.setDepthTexture(currentInport.depthTexture());
    m_rendererBase.render(eye, m_textureCopyRenderer);
  } else {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  }
}

bool Z3DCanvasPainter::isReady(Z3DEye /*eye*/) const
{
  return true;
}

bool Z3DCanvasPainter::isValid(Z3DEye /*eye*/) const
{
  return false;
}

void Z3DCanvasPainter::updateSize()
{
  setOutputSize(m_canvas.physicalSize());
}

void Z3DCanvasPainter::onCanvasResized(size_t w, size_t h)
{
  setOutputSize(glm::uvec2(w, h));
}

void Z3DCanvasPainter::invalidate(State inv)
{
  if (m_locked) {
    return;
  }
  m_locked = true;
  m_state |= inv;
  m_canvas.updateAll();
  m_locked = false;
}

void Z3DCanvasPainter::setOutputSize(const glm::uvec2& size)
{
  if (size != m_inport.expectedSize() || size != m_leftEyeInport.expectedSize() ||
      size != m_rightEyeInport.expectedSize()) {
    m_inport.setExpectedSize(size);
    m_leftEyeInport.setExpectedSize(size);
    m_rightEyeInport.setExpectedSize(size);
    globalCameraPara().viewportChanged(size);
    Q_EMIT requestUpstreamSizeChange(this);
  }
}

} // namespace nim
