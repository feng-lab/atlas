#pragma once

#include <QObject>
#include "zglmutils.h"

namespace nim {

struct Z3DLocalColorBuffer; // fwd decl to avoid pulling GL headers here

// Backend-neutral compositor façade used by the engine
class ZCompositorBase : public QObject
{
  Q_OBJECT
public:
  explicit ZCompositorBase(QObject* parent = nullptr) : QObject(parent) {}
  ~ZCompositorBase() override = default;

  // Sizing & region
  virtual void setOutputSize(const glm::uvec2& size) = 0;
  virtual glm::uvec2 outputSize() const = 0;
  virtual void setRenderingRegion(double left, double right, double bottom, double top) = 0;

  // Progressive mode
  virtual void setProgressiveRenderingMode(bool v) = 0;

  // Rendering entry (engine-triggered); stereo = left/right eyes
  virtual void requestRender(bool stereo) = 0;

  // Readback access used by screenshots/tiling
  virtual Z3DLocalColorBuffer* monoReadyLocalBuffer() const = 0;
  virtual Z3DLocalColorBuffer* leftReadyLocalBuffer() const = 0;
  virtual Z3DLocalColorBuffer* rightReadyLocalBuffer() const = 0;

Q_SIGNALS:
  void sceneParaUpdated();
  void renderingFinished();
  void renderingError(const QString& error) const;
};

} // namespace nim

