#ifndef ZTIMELINEAXISVIEW_H
#define ZTIMELINEAXISVIEW_H

#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include "ztimelinewidget.h"

namespace nim {

class CurrentTimeItem : public QGraphicsPathItem
{
public:
  CurrentTimeItem(ZTimelineWidget &timeline, QGraphicsItem * parent = nullptr);

protected:
  virtual QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
  virtual void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

  ZTimelineWidget &m_timeline;
};

class ZTimelineAxisView : public QGraphicsView
{
  Q_OBJECT
public:
  explicit ZTimelineAxisView(ZTimelineWidget &parent);

signals:

protected:
  void updateAxisScene();
  QString timeToString(double time) const;
  void moveCurrentTime();

private:
  ZTimelineWidget &m_timeline;

  QGraphicsScene *m_scene;
  CurrentTimeItem *m_currentTimeItem;
};

} // namespace nim
#endif // ZTIMELINEAXISVIEW_H
