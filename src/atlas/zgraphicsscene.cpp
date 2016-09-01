#include "zgraphicsscene.h"

#include "zview.h"
#include "zlog.h"
#include <QGraphicsSceneMouseEvent>

namespace nim {

ZGraphicsScene::ZGraphicsScene(ZView* view)
  : QGraphicsScene(view)
  , m_view(view)
{
  setSceneRect(QRectF(0, 0, 200, 200));
  setItemIndexMethod(NoIndex);
}

void ZGraphicsScene::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
  QGraphicsScene::mousePressEvent(event);
  if (!selectedItems().empty() &&
      event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier) {
    emit mousePressed(event->scenePos());
  }
}

void ZGraphicsScene::mouseReleaseEvent(QGraphicsSceneMouseEvent* event)
{
  QGraphicsScene::mouseReleaseEvent(event);
  if (!selectedItems().empty() && event->button() == Qt::LeftButton) {
    emit mouseReleased(event->scenePos());
  }
}

void ZGraphicsScene::mouseMoveEvent(QGraphicsSceneMouseEvent* event)
{
  double x = event->scenePos().x();
  double y = event->scenePos().y();
  m_view->setInfo(x, y);
  QGraphicsScene::mouseMoveEvent(event);
}

} // namespace nim
