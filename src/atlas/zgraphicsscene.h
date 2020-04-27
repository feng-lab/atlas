#pragma once

#include <QGraphicsScene>

namespace nim {

class ZView;

class ZROI;

class ZGraphicsScene : public QGraphicsScene
{
Q_OBJECT
public:
  explicit ZGraphicsScene(ZView* view);

  void registerROIForSubtraction(ZROI* roi, int slice, size_t shapeID);

  void removeROIForSubtraction();

  void performROISubtraction(const ZROI* roi, int slice, size_t shapeID);

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

  ZROI* m_roi = nullptr;
  int m_slice = -1;
  size_t m_shapeID = 0;
};

} // namespace nim

