#include "zgraphicsscene.h"

#include "zview.h"
#include "zlog.h"
#include <QGraphicsSceneMouseEvent>
#include <QMenu>

namespace nim {

ZGraphicsScene::ZGraphicsScene(ZView* view)
  : QGraphicsScene(view)
  , m_view(view)
{
  setSceneRect(QRectF(0, 0, 200, 200));
  setItemIndexMethod(NoIndex);
}

void ZGraphicsScene::contextMenuEvent(QGraphicsSceneContextMenuEvent* contextMenuEvent)
{
  QGraphicsScene::contextMenuEvent(contextMenuEvent);
  if (!contextMenuEvent->isAccepted()) {
    QMenu menu;
    QAction* pasteAction = menu.addAction("Paste Here");
    QAction* pasteHFlipAction = menu.addAction("Paste Horizonally Flipped");
    QAction* pasteVFlipAction = menu.addAction("Paste Vertically Flipped");
    QAction* selectedAction = menu.exec(contextMenuEvent->screenPos());
    if (selectedAction == pasteAction) {
      m_view->pasteHere(m_view->currentSlice(), contextMenuEvent->scenePos());
    } else if (selectedAction == pasteHFlipAction) {
      m_view->pasteHere(m_view->currentSlice(), contextMenuEvent->scenePos(), true);
    } else if (selectedAction == pasteVFlipAction) {
      m_view->pasteHere(m_view->currentSlice(), contextMenuEvent->scenePos(), false, true);
    }
  }
}

void ZGraphicsScene::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
  if (event->button() != Qt::LeftButton) {
    event->accept();
    return;
  }
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
  if (!selectedItems().empty()) {
    emit mouseMoved(event->scenePos());
  }
}

} // namespace nim
