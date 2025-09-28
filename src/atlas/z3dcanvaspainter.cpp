#include "z3dcanvaspainter.h"

#if defined(ATLAS_USE_OPENGLWIDGET)

#include "z3dgl.h"
#include "z3dcanvas.h"
#include "z3drenderingengine.h"
#include "z3drendertarget.h"

namespace nim {

Z3DCanvasPainter::Z3DCanvasPainter(Z3DCanvas& canvas)
  : m_parameterState()
  , m_frameState()
  , m_viewState()
  , m_sceneState()
  , m_rendererBase(m_parameterState, m_frameState, m_viewState, m_sceneState)
  , m_textureCopyRenderer(m_rendererBase, Z3DTextureCopyRenderer::OutputColorOption::DivideByAlpha)
  , m_canvas(canvas)
{
  m_rendererBase.setBackend(createGLRendererBackend());
}

void Z3DCanvasPainter::paint(bool stereo)
{
  if (!m_engine) {
    return;
  }
  auto& compositor = m_engine->compositor();
  if (!stereo && !compositor.monoReadyTarget()) {
    return;
  }
  if (stereo && (!compositor.leftReadyTarget() || !compositor.rightReadyTarget())) {
    return;
  }

  const std::scoped_lock lock(m_engine->targetSwitchMutex());
  // render to screen
  const glm::uvec2 viewportSize = m_canvas.physicalSize();
  glViewport(0, 0, viewportSize.x, viewportSize.y);
  if (stereo) {
    glDrawBuffer(GL_BACK_LEFT);
    renderEye(LeftEye, *compositor.leftReadyTarget());

    glDrawBuffer(GL_BACK_RIGHT);
    renderEye(RightEye, *compositor.rightReadyTarget());

    m_engine->clearNewRenderingFlag();
  } else {
    renderEye(MonoEye, *compositor.monoReadyTarget());

    m_engine->clearNewRenderingFlag();
  }
}

void Z3DCanvasPainter::renderEye(Z3DEye eye, Z3DRenderTarget& target)
{
  const glm::uvec2 viewportSize = m_canvas.physicalSize();
  syncRendererState(viewportSize);

  m_textureCopyRenderer.setColorTexture(target.colorTexture());
  m_textureCopyRenderer.setDepthTexture(target.depthTexture());
  m_rendererBase.render(eye, m_textureCopyRenderer);
}

void Z3DCanvasPainter::syncRendererState(const glm::uvec2& size)
{
  m_frameState.updateViewportData(glm::uvec4(0, 0, size.x, size.y));
  m_frameState.progressiveEnabled = false;
  m_frameState.progressiveActive = false;

  m_viewState.nearClip = 0.f;
  m_viewState.farClip = 1.f;
  for (auto& eyeState : m_viewState.eyes) {
    eyeState.viewMatrix = glm::mat4(1.f);
    eyeState.projectionMatrix = glm::mat4(1.f);
    eyeState.projectionViewMatrix = glm::mat4(1.f);
    eyeState.inverseViewMatrix = glm::mat4(1.f);
    eyeState.inverseProjectionMatrix = glm::mat4(1.f);
    eyeState.normalMatrix = glm::mat3(1.f);
    eyeState.eyePosition = glm::vec3(0.f);
    eyeState.frustumNearPlaneSize = glm::vec2(0.f);
    eyeState.fieldOfView = 0.f;
    eyeState.isPerspective = false;
  }

  m_parameterState.coordTransform = glm::mat4(1.f);

  m_sceneState.sceneAmbient = glm::vec4(0.f);
  m_sceneState.weightedBlendedDepthScale = 1.f;
  m_sceneState.devicePixelRatio = static_cast<float>(m_canvas.devicePixelRatioF());
  m_sceneState.transparency = TransparencyMode::BlendDelayed;
  m_sceneState.multisample = GeometryMSAAMode::None;
  m_sceneState.lighting.lightCount = 0;
  m_sceneState.fog.mode = FogMode::None;
}

} // namespace nim

#endif
