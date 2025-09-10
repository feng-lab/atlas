#pragma once

#if defined(ATLAS_USE_OPENGLWIDGET)

#include "z3dtexturecopyrenderer.h"

namespace nim {

class Z3DCanvas;

class Z3DCompositor;

class Z3DRenderingEngine;

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
  Z3DGlobalParameters m_globalParas;
  Z3DRendererBase m_rendererBase;
  Z3DTextureCopyRenderer m_textureCopyRenderer;
  Z3DCanvas& m_canvas;

  Z3DRenderingEngine* m_engine = nullptr;
};

} // namespace nim

#endif
