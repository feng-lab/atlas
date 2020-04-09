#pragma once

#include <QGraphicsScene>

namespace nim {

class ZView;

class ZGraphicsScene : public QGraphicsScene
{
Q_OBJECT
public:
  explicit ZGraphicsScene(ZView* view);

signals:

  void mousePressed(QPointF);

  void mouseMoved(QPointF);

  void mouseReleased(QPointF);

protected:
  void contextMenuEvent(QGraphicsSceneContextMenuEvent *contextMenuEvent) override;

  void mousePressEvent(QGraphicsSceneMouseEvent* event) override;

  void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

  void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;

private:
  ZView* m_view;
};

} // namespace nim

