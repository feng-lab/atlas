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

  const std::array<int, 8>& boundBox() const
  { return m_boundBox; }

  virtual const ZObjDoc& doc() const = 0;

  virtual bool hasObj(size_t id) const = 0;

  virtual void read(size_t id, const QJsonObject& json) = 0;

  virtual void write(size_t id, QJsonObject& json) const = 0;

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

  // called if mouse is pressed in a scene pos
  virtual void mousePressed(const QPointF& scenePos) = 0;

  virtual void mouseReleased(const QPointF& scenePos) = 0;

  // called if selection changed from view
  virtual void selectionChanged(const QList<QGraphicsItem*>& items) = 0;

  //
  virtual void rotateClockwise() = 0;

  virtual void rotateCounterclockwise() = 0;

  //
  virtual int minViewPrecedence() const = 0;

  virtual int maxViewPrecedence() const = 0;

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

  void resetBoundBox();

  void expandBoundBox(const std::array<int, 8>& boundBox);

protected:
  ZView& m_view;
  std::array<int, 8> m_boundBox;
};

} // namespace nim

