#pragma once

#include "z3dboundedfilter.h"
#include "z3dcompositorbase.h"
#include "zjson.h"

#include <memory>

namespace nim {

class Z3DCompositor;
class Z3DCompositorGLBackend;
class Z3DRenderingEngine;
class ZWidgetsGroup;
class Z3DRenderTarget;
struct Z3DLocalColorBuffer;

// Transitional shell that will own the engine-facing compositor filter behaviour while delegating
// rendering responsibilities to a backend implementing Z3DCompositorBase.
class Z3DCompositorFilter : public Z3DBoundedFilter
{
  Q_OBJECT
public:
  explicit Z3DCompositorFilter(Z3DGlobalParameters& globals, QObject* parent = nullptr);
  ~Z3DCompositorFilter() override;

  Z3DCompositorBase& backend();
  std::unique_ptr<Z3DCompositorBase> takeBackend();
  void setBackend(std::unique_ptr<Z3DCompositorBase> backend);

  void setOutputSize(const glm::uvec2& size);
  [[nodiscard]] glm::uvec2 outputSize() const;

  void setRenderingRegion(double left = 0., double right = 1., double bottom = 0., double top = 1.);

  [[nodiscard]] std::shared_ptr<ZWidgetsGroup> backgroundWidgetsGroup();
  [[nodiscard]] std::shared_ptr<ZWidgetsGroup> axisWidgetsGroup();

  void read(const json::object& json);
  void write(json::object& json) const;

  [[nodiscard]] Z3DLocalColorBuffer* monoReadyLocalBuffer() const;
  [[nodiscard]] Z3DLocalColorBuffer* leftReadyLocalBuffer() const;
  [[nodiscard]] Z3DLocalColorBuffer* rightReadyLocalBuffer() const;

  void savePickingBufferToImage(const QString& filename);

  // Temporary access to the legacy compositor for paths that still require the GL implementation.
  [[nodiscard]] Z3DCompositor* glLegacy() const { return m_glLegacy; }

  // TODO: expose ports and parameter wiring once the shell assumes ownership from the legacy GL compositor.

private:
  Z3DGlobalParameters& m_globals;
  std::unique_ptr<Z3DCompositorBase> m_backend;
  Z3DCompositor* m_glLegacy = nullptr; // temporary bridge so we can reuse existing GL behaviour
};

} // namespace nim
