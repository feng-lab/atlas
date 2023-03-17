#pragma once

#include "z3drenderingengine.h"
#include "zwidgetsgroup.h"
#include "z3dcanvas.h"
#include "z3dnetworkevaluator.h"
#include "z3dcompositor.h"
#include "zobjdoc.h"
#include "zexception.h"
#include <QObject>

namespace nim {

class Z3DObjView : public QObject
{
  Q_OBJECT

public:
  explicit Z3DObjView(Z3DRenderingEngine& engine);

  [[nodiscard]] const ZBBox<glm::dvec3>& boundBox() const
  {
    return m_boundBox;
  }

  [[nodiscard]] virtual const ZObjDoc& doc() const = 0;

  [[nodiscard]] virtual bool hasObj(size_t id) const = 0;

  // return default ZBBox if object id is not the current type
  [[nodiscard]] virtual ZBBox<glm::dvec3> boundBoxOfObj(size_t id) const = 0;

  [[nodiscard]] virtual ZBBox<glm::dvec3> boundBoxOfObjAfterClipping(size_t id) const = 0;

  virtual void read(size_t id, const json::object& json) = 0;

  virtual void write(size_t id, json::object& json) const = 0;

  // get view setting widget group of obj id, default return nullptr
  virtual std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroupOf(size_t id);

Q_SIGNALS:

  void objViewReady(size_t id);

protected:
  virtual void updateBoundBox() = 0;

  virtual void onObjAboutToBeRemoved(size_t id) = 0;

  virtual void onObjVisibleChanged(size_t id, bool v) = 0;

  virtual void onSelectionChanged(const std::vector<size_t>& selected, const std::vector<size_t>& deselected) = 0;

  virtual void onObjSelectedFromView(bool append) = 0;

  virtual void onObjDeselectedFromView() = 0;

  virtual void onObjVisibleChangedFromView(bool v) = 0;

  inline void resetBoundBox()
  {
    m_boundBox.reset();
  }

  inline void expandBoundBox(const ZBBox<glm::dvec3>& boundBox)
  {
    m_boundBox.expand(boundBox);
  }

protected:
  Z3DRenderingEngine& m_engine;
  ZBBox<glm::dvec3> m_boundBox;
};

} // namespace nim
