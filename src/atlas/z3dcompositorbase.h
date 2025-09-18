#pragma once

#include <QObject>
#include <memory>

#include "zglmutils.h"
#include "zjson.h"

class QString;

namespace nim {

class ZWidgetsGroup;
struct Z3DLocalColorBuffer;

class Z3DCompositorBase : public QObject
{
  Q_OBJECT
public:
  explicit Z3DCompositorBase(QObject* parent = nullptr) : QObject(parent) {}
  ~Z3DCompositorBase() override = default;

  virtual void setOutputSize(const glm::uvec2& size) = 0;
  virtual glm::uvec2 outputSize() const = 0;
  virtual void setRenderingRegion(double left, double right, double bottom, double top) = 0;
  virtual void setProgressiveRenderingMode(bool v) = 0;
  virtual void requestRender(bool stereo) = 0;

  virtual std::shared_ptr<ZWidgetsGroup> backgroundWidgetsGroup() = 0;
  virtual std::shared_ptr<ZWidgetsGroup> axisWidgetsGroup() = 0;

  virtual void read(const json::object& json) = 0;
  virtual void write(json::object& json) const = 0;

  virtual Z3DLocalColorBuffer* monoReadyLocalBuffer() const = 0;
  virtual Z3DLocalColorBuffer* leftReadyLocalBuffer() const = 0;
  virtual Z3DLocalColorBuffer* rightReadyLocalBuffer() const = 0;

  virtual void savePickingBufferToImage(const QString& filename) = 0;

Q_SIGNALS:
  void sceneParaUpdated();
  void renderingFinished();
  void renderingError(const QString& error) const;
};

} // namespace nim
