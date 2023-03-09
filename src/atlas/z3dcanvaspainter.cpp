#include "z3dcanvaspainter.h"

#include "z3dgl.h"
#include "z3dcanvas.h"
#include "z3drenderingengine.h"

namespace nim {

Z3DCanvasPainter::Z3DCanvasPainter(Z3DCanvas& canvas)
  : m_globalParas()
  , m_rendererBase(m_globalParas)
  , m_textureCopyRenderer(m_rendererBase, Z3DTextureCopyRenderer::OutputColorOption::DivideByAlpha)
  , m_canvas(canvas)
{
}

void Z3DCanvasPainter::paint(bool stereo)
{
  if (!m_engine || !m_engine->monoReadyTarget()) {
    return;
  }
  // render to screen
  glViewport(0, 0, m_canvas.physicalSize().x, m_canvas.physicalSize().y);
  m_rendererBase.setViewport(m_canvas.physicalSize());
  if (stereo) {
    const std::lock_guard<std::mutex> lock(m_engine->targetSwitchMutex());
    glDrawBuffer(GL_BACK_LEFT);
    m_textureCopyRenderer.setColorTexture(m_engine->leftReadyTarget()->colorTexture());
    m_textureCopyRenderer.setDepthTexture(m_engine->leftReadyTarget()->depthTexture());
    m_rendererBase.render(Z3DEye::Left, m_textureCopyRenderer);

    glDrawBuffer(GL_BACK_RIGHT);
    m_textureCopyRenderer.setColorTexture(m_engine->rightReadyTarget()->colorTexture());
    m_textureCopyRenderer.setDepthTexture(m_engine->rightReadyTarget()->depthTexture());
    m_rendererBase.render(Z3DEye::Right, m_textureCopyRenderer);

    m_engine->clearNewRenderingFlag();
  } else {
    const std::lock_guard<std::mutex> lock(m_engine->targetSwitchMutex());
    auto startTarget = m_engine->monoReadyTarget();
    m_textureCopyRenderer.setColorTexture(m_engine->monoReadyTarget()->colorTexture());
    m_textureCopyRenderer.setDepthTexture(m_engine->monoReadyTarget()->depthTexture());
    auto endTarget = m_engine->monoReadyTarget();
    LOG(INFO) << startTarget << " " << endTarget << " " << m_canvas.physicalSize();
    m_rendererBase.render(Z3DEye::Mono, m_textureCopyRenderer);

    m_engine->clearNewRenderingFlag();
  }
}

} // namespace nim
