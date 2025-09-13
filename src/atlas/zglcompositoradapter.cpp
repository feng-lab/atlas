#include "zglcompositoradapter.h"

#include "z3dcompositor.h"
#include "z3dglobalparameters.h"

namespace nim {

ZGLCompositorAdapter::ZGLCompositorAdapter(Z3DGlobalParameters& globals, QObject* parent)
  : ZCompositorBase(parent)
  , m_gl(std::make_unique<Z3DCompositor>(globals))
{
  // Bridge signals from GL compositor to façade
  QObject::connect(m_gl.get(), &Z3DCompositor::sceneParaUpdated, this, &ZCompositorBase::sceneParaUpdated);
  QObject::connect(m_gl.get(), &Z3DCompositor::renderingFinished, this, &ZCompositorBase::renderingFinished);
  QObject::connect(m_gl.get(), &Z3DCompositor::renderingError, this, &ZCompositorBase::renderingError);
}

ZGLCompositorAdapter::~ZGLCompositorAdapter() = default;

void ZGLCompositorAdapter::setOutputSize(const glm::uvec2& size)
{
  m_gl->setOutputSize(size);
}

glm::uvec2 ZGLCompositorAdapter::outputSize() const
{
  return m_gl->outputSize();
}

void ZGLCompositorAdapter::setRenderingRegion(double left, double right, double bottom, double top)
{
  m_gl->setRenderingRegion(left, right, bottom, top);
}

void ZGLCompositorAdapter::setProgressiveRenderingMode(bool v)
{
  m_gl->setProgressiveRenderingMode(v);
}

void ZGLCompositorAdapter::requestRender(bool stereo)
{
  // The engine drives rendering via Z3DNetworkEvaluator today; keep this a no-op for now.
  (void)stereo;
}

Z3DLocalColorBuffer* ZGLCompositorAdapter::monoReadyLocalBuffer() const
{
  return m_gl->monoReadyLocalBuffer();
}

Z3DLocalColorBuffer* ZGLCompositorAdapter::leftReadyLocalBuffer() const
{
  return m_gl->leftReadyLocalBuffer();
}

Z3DLocalColorBuffer* ZGLCompositorAdapter::rightReadyLocalBuffer() const
{
  return m_gl->rightReadyLocalBuffer();
}

Z3DCompositor& ZGLCompositorAdapter::glCompositor()
{
  return *m_gl;
}

} // namespace nim

