#pragma once

#include "z3dcompositorbase.h"
#include <memory>

namespace nim {

class Z3DCompositor;
class Z3DGlobalParameters;

// Adapter that exposes the API-neutral façade while using the existing GL compositor under the hood
class Z3DCompositorGLBackend : public Z3DCompositorBase
{
  Q_OBJECT
public:
  explicit Z3DCompositorGLBackend(Z3DGlobalParameters& globals, QObject* parent = nullptr);
  ~Z3DCompositorGLBackend() override;

  // façade API
  void setOutputSize(const glm::uvec2& size) override;
  glm::uvec2 outputSize() const override;
  void setRenderingRegion(double left, double right, double bottom, double top) override;
  void setProgressiveRenderingMode(bool v) override;
  void requestRender(bool stereo) override;

  std::shared_ptr<ZWidgetsGroup> backgroundWidgetsGroup() override;
  std::shared_ptr<ZWidgetsGroup> axisWidgetsGroup() override;

  void read(const json::object& json) override;
  void write(json::object& json) const override;

  Z3DLocalColorBuffer* monoReadyLocalBuffer() const override;
  Z3DLocalColorBuffer* leftReadyLocalBuffer() const override;
  Z3DLocalColorBuffer* rightReadyLocalBuffer() const override;

  void savePickingBufferToImage(const QString& filename) override;

  // Transitional: access underlying GL compositor for network wiring while we migrate
  Z3DCompositor& glCompositor();

private:
  std::unique_ptr<Z3DCompositor> m_gl;
};

} // namespace nim
