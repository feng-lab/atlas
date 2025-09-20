#pragma once

#include "z3dport.h"

#include <glm/vec2.hpp>
#include <memory>

namespace nim {

struct Z3DRenderSurfaceDescriptor
{
  glm::uvec2 size{0, 0};
};

class Z3DRenderSurfaceLease
{
public:
  virtual ~Z3DRenderSurfaceLease() = default;

  [[nodiscard]] virtual const Z3DRenderSurfaceDescriptor& descriptor() const = 0;
};

class Z3DRenderSurfaceOutputPort : public Z3DOutputPortBase
{
public:
  explicit Z3DRenderSurfaceOutputPort(const QString& name,
                                      Z3DFilter* filter,
                                      Z3DFilter::State invalidationState = Z3DFilter::State::AllResultInvalid)
    : Z3DOutputPortBase(name, filter)
    , m_invalidationState(invalidationState)
  {}

  ~Z3DRenderSurfaceOutputPort() override = default;

  void invalidate() override
  {
    m_surface.reset();
    Z3DOutputPortBase::invalidate();
  }

  [[nodiscard]] bool hasValidData() const override
  {
    return m_surface != nullptr;
  }

  void setSurface(std::unique_ptr<Z3DRenderSurfaceLease> lease)
  {
    m_surface = std::move(lease);
  }

  [[nodiscard]] const Z3DRenderSurfaceLease* surface() const
  {
    return m_surface.get();
  }

  [[nodiscard]] Z3DFilter::State invalidationState() const
  {
    return m_invalidationState;
  }

protected:
  Z3DFilter::State m_invalidationState;
  std::unique_ptr<Z3DRenderSurfaceLease> m_surface;
};

class Z3DRenderSurfaceInputPort : public Z3DInputPortBase
{
public:
  explicit Z3DRenderSurfaceInputPort(const QString& name,
                                     bool allowMultipleConnections,
                                     Z3DFilter* filter,
                                     Z3DFilter::State invalidationState = Z3DFilter::State::AllResultInvalid)
    : Z3DInputPortBase(name, allowMultipleConnections, filter, invalidationState)
  {}

  [[nodiscard]] bool isReady() const override
  {
    return numValidInputs() > 0;
  }

  [[nodiscard]] size_t numValidInputs() const;

  [[nodiscard]] const Z3DRenderSurfaceLease* surface(size_t idx = 0) const;

private:
  [[nodiscard]] const Z3DRenderSurfaceOutputPort* surfacePort(size_t idx) const;
};

inline size_t Z3DRenderSurfaceInputPort::numValidInputs() const
{
  size_t count = 0;
  for (const auto* port : m_connectedOutputPorts) {
    if (auto surfacePort = dynamic_cast<const Z3DRenderSurfaceOutputPort*>(port)) {
      if (surfacePort->hasValidData()) {
        ++count;
      }
    }
  }
  return count;
}

inline const Z3DRenderSurfaceLease* Z3DRenderSurfaceInputPort::surface(size_t idx) const
{
  if (auto port = surfacePort(idx)) {
    return port->surface();
  }
  return nullptr;
}

inline const Z3DRenderSurfaceOutputPort* Z3DRenderSurfaceInputPort::surfacePort(size_t idx) const
{
  size_t current = 0;
  const Z3DRenderSurfaceOutputPort* result = nullptr;
  for (const auto* port : m_connectedOutputPorts) {
    if (result) {
      break;
    }
    if (auto surfacePort = dynamic_cast<const Z3DRenderSurfaceOutputPort*>(port)) {
      if (surfacePort->hasValidData()) {
        if (current == idx) {
          result = surfacePort;
          break;
        }
        ++current;
      }
    }
  }
  return result;
}

} // namespace nim
