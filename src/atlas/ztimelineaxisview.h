#pragma once

#include "ztimelinewidget.h"
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPathItem>

namespace nim {

class CurrentTimeItem : public QGraphicsPathItem
{
public:
  explicit CurrentTimeItem(ZTimelineWidget& timeline, QGraphicsItem* parent = nullptr);

protected:
  QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

  ZTimelineWidget& m_timeline;
};

class ZTimelineAxisView : public QGraphicsView
{
  Q_OBJECT

public:
  explicit ZTimelineAxisView(ZTimelineWidget& parent);

protected:
  void updateAxisScene();

  static QString timeToString(double time) ;

  void moveCurrentTime();

private:
  ZTimelineWidget& m_timeline;

  QGraphicsScene* m_scene;
  CurrentTimeItem* m_currentTimeItem;
};

} // namespace nim
