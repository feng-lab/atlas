#include "z3dcanvaspainter.h"

#if defined(ATLAS_USE_OPENGLWIDGET)

#include "z3dgl.h"
#include "z3dcanvas.h"
#include "z3drenderingengine.h"

namespace nim {

Z3DCanvasPainter::Z3DCanvasPainter(Z3DCanvas& canvas)
  : m_globalParas()
  , m_rendererBase(m_globalParas)
  , m_textureCopyRenderer(m_rendererBase, Z3DTextureCopyRenderer::OutputColorOption::DivideByAlpha)
  , m_canvas(canvas)
{}

void Z3DCanvasPainter::paint(bool stereo)
{
  if (!m_engine) {
    return;
  }
  if (!stereo && !m_engine->monoReadyTarget()) {
    return;
  }
  if (stereo && (!m_engine->leftReadyTarget() || !m_engine->rightReadyTarget())) {
    return;
  }

  const std::scoped_lock lock(m_engine->targetSwitchMutex());
  // render to screen
  glViewport(0, 0, m_canvas.physicalSize().x, m_canvas.physicalSize().y);
  m_rendererBase.setViewport(m_canvas.physicalSize());
  if (stereo) {
    glDrawBuffer(GL_BACK_LEFT);
    m_textureCopyRenderer.setColorTexture(m_engine->leftReadyTarget()->colorTexture());
    m_textureCopyRenderer.setDepthTexture(m_engine->leftReadyTarget()->depthTexture());
    m_rendererBase.render(LeftEye, m_textureCopyRenderer);

    glDrawBuffer(GL_BACK_RIGHT);
    m_textureCopyRenderer.setColorTexture(m_engine->rightReadyTarget()->colorTexture());
    m_textureCopyRenderer.setDepthTexture(m_engine->rightReadyTarget()->depthTexture());
    m_rendererBase.render(RightEye, m_textureCopyRenderer);

    m_engine->clearNewRenderingFlag();
  } else {
    auto startTarget = m_engine->monoReadyTarget();
    m_textureCopyRenderer.setColorTexture(m_engine->monoReadyTarget()->colorTexture());
    m_textureCopyRenderer.setDepthTexture(m_engine->monoReadyTarget()->depthTexture());
    auto endTarget = m_engine->monoReadyTarget();
    m_rendererBase.render(MonoEye, m_textureCopyRenderer);

    m_engine->clearNewRenderingFlag();
    VLOG(1) << startTarget << " " << endTarget << " " << m_canvas.physicalSize();
  }
}

} // namespace nim

#endif