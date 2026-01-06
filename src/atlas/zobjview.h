#pragma once

#include "zgraphicsscene.h"
#include "zview.h"
#include "zwidgetsgroup.h"
#include "zobjdoc.h"
#include <QObject>
#include <QRectF>
#include <QString>

class QMenu;

namespace nim {

class ZObjView : public QObject
{
  Q_OBJECT

public:
  explicit ZObjView(ZView& view);

  [[nodiscard]] const ZBBox<glm::ivec4>& boundBox() const
  {
    return m_boundBox;
  }

  [[nodiscard]] virtual const ZObjDoc& doc() const = 0;

  [[nodiscard]] virtual bool hasObj(size_t id) const = 0;

  virtual void read(size_t id, const json::object& json) = 0;

  virtual void write(size_t id, json::object& json) const = 0;

  // to show on info bar, default return empty
  virtual QString infoOfPos(double x, double y);

  // Allows views to contribute to the 2D context menu at a scene position. `activeObjId` is the object currently
  // selected in the Objects Manager / View Setting dock (0 if none). Default is no-op.
  virtual void appendContextMenuActions(QMenu& menu,
                                       size_t activeObjId,
                                       const QPointF& scenePos,
                                       Qt::KeyboardModifiers modifiers)
  {
    (void)menu;
    (void)activeObjId;
    (void)scenePos;
    (void)modifiers;
  }

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
  virtual void mousePressed(const QPointF& scenePos, Qt::KeyboardModifiers modifiers) = 0;

  virtual void mouseMoved(const QPointF& scenePos, Qt::KeyboardModifiers modifiers) = 0;

  virtual void mouseReleased(const QPointF& scenePos) = 0;

  // called if selection changed from view
  virtual void selectionChanged(const QList<QGraphicsItem*>& items) = 0;

  // Called before a 2D animation export captures a frame. Views with async rendering (e.g. network tiles)
  // can use this hook to kick off any required work.
  virtual void prepare2DExportFrame() {}

  // Returns true when the view is ready for deterministic 2D export frame capture.
  // If false and errorMsg is set, export should abort and report the error.
  [[nodiscard]] virtual bool is2DExportFrameReady(QString* errorMsg) const
  {
    (void)errorMsg;
    return true;
  }

  //
  virtual void rotateClockwise(double x, double y) = 0;

  virtual void rotateCounterclockwise(double x, double y) = 0;

  //
  [[nodiscard]] virtual int minViewPrecedence() const = 0;

  [[nodiscard]] virtual int maxViewPrecedence() const = 0;

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

  void resetBoundBox()
  {
    m_boundBox.reset();
  }

  void expandBoundBox(const ZBBox<glm::ivec4>& boundBox)
  {
    m_boundBox.expand(boundBox);
  }

protected:
  ZView& m_view;
  ZBBox<glm::ivec4> m_boundBox;
};

} // namespace nim
