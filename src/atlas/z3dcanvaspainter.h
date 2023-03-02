#pragma once

#include "z3dboundedfilter.h"
#include "z3drenderport.h"
#include "z3dtexturecopyrenderer.h"
#include "zexception.h"
#include "zimg.h"
#include "znumericparameter.h"

namespace nim {

class Z3DCanvas;

class Z3DCompositor;

class Z3DCanvasPainter : public Z3DBoundedFilter
{
  Q_OBJECT

public:
  explicit Z3DCanvasPainter(Z3DGlobalParameters& globalParas, Z3DCanvas& canvas, QObject* parent = nullptr);

  void invalidate(State inv) override;

  Z3DCanvas& canvas()
  {
    return m_canvas;
  }

  void resetToMatchCanvasSize();

protected:
  void onCanvasResized(size_t w, size_t h);

  void process(Z3DEye eye) override;

  [[nodiscard]] bool isReady(Z3DEye /*eye*/) const override;

  [[nodiscard]] bool isValid(Z3DEye eye) const override;

  void updateSize() override;

  void renderInportToImage(Z3DEye eye);

private:
  void setOutputSize(const glm::uvec2& size);

private:
  Z3DTextureCopyRenderer m_textureCopyRenderer;

  Z3DCanvas& m_canvas;
  Z3DRenderInputPort m_inport;
  Z3DRenderInputPort m_leftEyeInport;
  Z3DRenderInputPort m_rightEyeInport;

  mutable bool m_locked = false;
};

} // namespace nim
