#include "z3dcompositorfilter.h"

#include "z3dcompositorglbackend.h"
#include "z3dcompositor.h"
#include "zwidgetsgroup.h"

#include <QString>

namespace nim {

Z3DCompositorFilter::Z3DCompositorFilter(Z3DGlobalParameters& globals, QObject* parent)
  : Z3DBoundedFilter(globals, parent)
  , m_globals(globals)
{
  auto glBackend = std::make_unique<Z3DCompositorGLBackend>(globals, this);
  m_glLegacy = &glBackend->glCompositor();
  m_backend = std::move(glBackend);
}

Z3DCompositorFilter::~Z3DCompositorFilter() = default;

Z3DCompositorBase& Z3DCompositorFilter::backend()
{
  return *m_backend;
}

std::unique_ptr<Z3DCompositorBase> Z3DCompositorFilter::takeBackend()
{
  m_glLegacy = nullptr;
  return std::move(m_backend);
}

void Z3DCompositorFilter::setBackend(std::unique_ptr<Z3DCompositorBase> backend)
{
  m_backend = std::move(backend);
  // If we plugged in a GL backend we keep a pointer to the underlying compositor so existing
  // paths can continue to operate while the shell is filled out.
  if (auto* glBackend = dynamic_cast<Z3DCompositorGLBackend*>(m_backend.get())) {
    m_glLegacy = &glBackend->glCompositor();
  } else {
    m_glLegacy = nullptr;
  }
}

void Z3DCompositorFilter::setOutputSize(const glm::uvec2& size)
{
  if (m_backend) {
    m_backend->setOutputSize(size);
  }
}

glm::uvec2 Z3DCompositorFilter::outputSize() const
{
  return m_backend ? m_backend->outputSize() : glm::uvec2(0);
}

void Z3DCompositorFilter::setRenderingRegion(double left, double right, double bottom, double top)
{
  if (m_backend) {
    m_backend->setRenderingRegion(left, right, bottom, top);
  }
}

std::shared_ptr<ZWidgetsGroup> Z3DCompositorFilter::backgroundWidgetsGroup()
{
  if (m_backend) {
    return m_backend->backgroundWidgetsGroup();
  }
  return {};
}

std::shared_ptr<ZWidgetsGroup> Z3DCompositorFilter::axisWidgetsGroup()
{
  if (m_backend) {
    return m_backend->axisWidgetsGroup();
  }
  return {};
}

void Z3DCompositorFilter::read(const json::object& json)
{
  if (m_backend) {
    m_backend->read(json);
  }
}

void Z3DCompositorFilter::write(json::object& json) const
{
  if (m_backend) {
    m_backend->write(json);
  }
}

Z3DLocalColorBuffer* Z3DCompositorFilter::monoReadyLocalBuffer() const
{
  return m_backend ? m_backend->monoReadyLocalBuffer() : nullptr;
}

Z3DLocalColorBuffer* Z3DCompositorFilter::leftReadyLocalBuffer() const
{
  return m_backend ? m_backend->leftReadyLocalBuffer() : nullptr;
}

Z3DLocalColorBuffer* Z3DCompositorFilter::rightReadyLocalBuffer() const
{
  return m_backend ? m_backend->rightReadyLocalBuffer() : nullptr;
}

void Z3DCompositorFilter::savePickingBufferToImage(const QString& filename)
{
  if (m_backend) {
    m_backend->savePickingBufferToImage(filename);
  }
}

} // namespace nim
