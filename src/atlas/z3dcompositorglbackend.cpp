#include "z3dcompositorglbackend.h"

#include "z3dcompositor.h"
#include "z3dglobalparameters.h"

namespace nim {

Z3DCompositorGLBackend::Z3DCompositorGLBackend(Z3DGlobalParameters& globals, QObject* parent)
  : Z3DCompositorBase(parent)
  , m_gl(std::make_unique<Z3DCompositor>(globals))
{
  // Bridge signals from GL compositor to façade
  QObject::connect(m_gl.get(), &Z3DCompositor::sceneParaUpdated, this, &Z3DCompositorBase::sceneParaUpdated);
  QObject::connect(m_gl.get(), &Z3DCompositor::renderingFinished, this, &Z3DCompositorBase::renderingFinished);
  QObject::connect(m_gl.get(), &Z3DCompositor::renderingError, this, &Z3DCompositorBase::renderingError);
}

Z3DCompositorGLBackend::~Z3DCompositorGLBackend() = default;

void Z3DCompositorGLBackend::setOutputSize(const glm::uvec2& size)
{
  m_gl->setOutputSize(size);
}

glm::uvec2 Z3DCompositorGLBackend::outputSize() const
{
  return m_gl->outputSize();
}

void Z3DCompositorGLBackend::setRenderingRegion(double left, double right, double bottom, double top)
{
  m_gl->setRenderingRegion(left, right, bottom, top);
}

void Z3DCompositorGLBackend::setProgressiveRenderingMode(bool v)
{
  m_gl->setProgressiveRenderingMode(v);
}

void Z3DCompositorGLBackend::requestRender(bool stereo)
{
  // The engine drives rendering via Z3DNetworkEvaluator today; keep this a no-op for now.
  (void)stereo;
}

std::shared_ptr<ZWidgetsGroup> Z3DCompositorGLBackend::backgroundWidgetsGroup()
{
  return m_gl->backgroundWidgetsGroup();
}

std::shared_ptr<ZWidgetsGroup> Z3DCompositorGLBackend::axisWidgetsGroup()
{
  return m_gl->axisWidgetsGroup();
}

void Z3DCompositorGLBackend::read(const json::object& json)
{
  m_gl->read(json);
}

void Z3DCompositorGLBackend::write(json::object& json) const
{
  m_gl->write(json);
}

Z3DLocalColorBuffer* Z3DCompositorGLBackend::monoReadyLocalBuffer() const
{
  return m_gl->monoReadyLocalBuffer();
}

Z3DLocalColorBuffer* Z3DCompositorGLBackend::leftReadyLocalBuffer() const
{
  return m_gl->leftReadyLocalBuffer();
}

Z3DLocalColorBuffer* Z3DCompositorGLBackend::rightReadyLocalBuffer() const
{
  return m_gl->rightReadyLocalBuffer();
}

void Z3DCompositorGLBackend::savePickingBufferToImage(const QString& filename)
{
  m_gl->savePickingBufferToImage(filename);
}

Z3DCompositor& Z3DCompositorGLBackend::glCompositor()
{
  return *m_gl;
}

} // namespace nim
