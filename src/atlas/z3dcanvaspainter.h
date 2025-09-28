#pragma once

#if defined(ATLAS_USE_OPENGLWIDGET)

#include "z3dtexturecopyrenderer.h"
#include "z3drendererbase.h"

namespace nim {

class Z3DCanvas;

class Z3DCompositor;

class Z3DRenderingEngine;

class Z3DRenderTarget;

class Z3DCanvasPainter
{
public:
  explicit Z3DCanvasPainter(Z3DCanvas& canvas);

  void setRenderingEngine(Z3DRenderingEngine* engine)
  {
    m_engine = engine;
  }

  void paint(bool stereo);

private:
  RendererParameterState m_parameterState{};
  RendererFrameState m_frameState{};
  RendererViewState m_viewState{};
  RendererSceneState m_sceneState{};
  Z3DRendererBase m_rendererBase;
  Z3DTextureCopyRenderer m_textureCopyRenderer;
  Z3DCanvas& m_canvas;

  Z3DRenderingEngine* m_engine = nullptr;

  void syncRendererState(const glm::uvec2& size);
  void renderEye(Z3DEye eye, Z3DRenderTarget& target);
};

} // namespace nim

#endif
