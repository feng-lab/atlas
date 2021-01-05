#pragma once

#include "zgraphicsscene.h"
#include "zview.h"
#include "zwidgetsgroup.h"
#include "zobjdoc.h"
#include <QObject>
#include <QRectF>

namespace nim {

class ZObjView : public QObject
{
Q_OBJECT
public:
  explicit ZObjView(ZView& view);

  [[nodiscard]] const ZBBox<glm::ivec4>& boundBox() const
  { return m_boundBox; }

  [[nodiscard]] virtual const ZObjDoc& doc() const = 0;

  [[nodiscard]] virtual bool hasObj(size_t id) const = 0;

  virtual void read(size_t id, const json::object& json) = 0;

  virtual void write(size_t id, json::object& json) const = 0;

  // to show on info bar, default return empty
  virtual QString infoOfPos(double x, double y);

  // called to show a certain slice of certain time
  virtual void setNormalView(int slice, int time) = 0;

  // called to show a max z projection view of time
  virtual void setMaxZProjView(int time) = 0;

  // get view setting widget group of obj id, default return nullptr
  virtual std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroupOf(size_t id);

  // called if Delete key is pressed
  virtual void deleteKeyPressed() = 0;

  virtual void copyKeyPressed() = 0;

  virtual void pasteKeyPressed(int slice, QPointF point, bool hFlip, bool vFlip) = 0;

  // called if mouse is pressed in a scene pos
  virtual void mousePressed(const QPointF& scenePos) = 0;

  virtual void mouseMoved(const QPointF& scenePos) = 0;

  virtual void mouseReleased(const QPointF& scenePos) = 0;

  // called if selection changed from view
  virtual void selectionChanged(const QList<QGraphicsItem*>& items) = 0;

  //
  virtual void rotateClockwise(double x, double y) = 0;

  virtual void rotateCounterclockwise(double x, double y) = 0;

  //
  [[nodiscard]] virtual int minViewPrecedence() const = 0;

  [[nodiscard]] virtual int maxViewPrecedence() const = 0;

signals:

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
  { m_boundBox.reset(); }

  inline void expandBoundBox(const ZBBox<glm::ivec4>& boundBox)
  { m_boundBox.expand(boundBox); }

protected:
  ZView& m_view;
  ZBBox<glm::ivec4> m_boundBox;
};

} // namespace nim

