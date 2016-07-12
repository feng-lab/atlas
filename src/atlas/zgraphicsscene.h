#ifndef ZGRAPHICSSCENE_H
#define ZGRAPHICSSCENE_H

#include <QGraphicsScene>

namespace nim {

class ZView;

class ZGraphicsScene : public QGraphicsScene
{
  Q_OBJECT
public:
  explicit ZGraphicsScene(ZView *view);

signals:
  void mousePressed(QPointF);
  void mouseReleased(QPointF);

protected:
  void mousePressEvent(QGraphicsSceneMouseEvent *event);
  void mouseReleaseEvent(QGraphicsSceneMouseEvent *event);
  void mouseMoveEvent(QGraphicsSceneMouseEvent *event);

private:
  ZView *m_view;
};

} // namespace nim

#endif // ZSCENE_H
