#pragma once

#include "z3dview.h"
#include "zwidgetsgroup.h"
#include "z3dnetworkevaluator.h"
#include "z3dcompositor.h"
#include "zobjdoc.h"
#include "zexception.h"
#include <QObject>
#include <QMessageBox>

namespace nim {

class Z3DObjView : public QObject
{
Q_OBJECT
public:
  explicit Z3DObjView(Z3DView& view);

  std::vector<double> boundBox() const
  { return m_boundBox; }

  virtual const ZObjDoc& doc() const = 0;

  virtual bool hasObj(size_t id) const = 0;

  virtual void read(size_t id, const QJsonObject& json) = 0;

  virtual void write(size_t id, QJsonObject& json) const = 0;

  // get view setting widget group of obj id, default return nullptr
  virtual std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroupOf(size_t id);

  inline Z3DCameraParameter& camera()
  { return m_view.camera(); }

  inline Z3DTrackballInteractionHandler& interactionHandler()
  { return m_view.interactionHandler(); }

  inline Z3DCanvas& canvas()
  { return m_view.canvas(); }

  inline Z3DCompositor& compositor()
  { return m_view.compositor(); }

  inline Z3DNetworkEvaluator& networkEvaluator()
  { return m_view.networkEvaluator(); }

  inline Z3DGlobalParameters& globalParas()
  { return m_view.globalParas(); }

signals:

  void objViewReady(size_t id);

protected:
  virtual void updateBoundBox() = 0;

  virtual void onObjRemoved(size_t id) = 0;

  virtual void onAllObjsRemoved() = 0;

  virtual void onObjVisibleChanged(size_t id, bool v) = 0;

  virtual void onSelectionChanged(const QList<size_t>& selected, const QList<size_t>& deselected) = 0;

  virtual void onObjSelectedFromView(bool append) = 0;

  virtual void onObjDeselectedFromView() = 0;

  virtual void onObjVisibleChangedFromView(bool v) = 0;

  void resetBoundBox();

  void expandBoundBox(const std::vector<double>& boundBox);

protected:
  Z3DView& m_view;
  std::vector<double> m_boundBox;
};

} // namespace nim

