#pragma once

#include <QGraphicsScene>

namespace nim {

class ZView;

class ZROI;

class ZGraphicsScene : public QGraphicsScene
{
Q_OBJECT
public:
  enum class ROIAction
  {
    New, Add, Subtract
  };

  explicit ZGraphicsScene(ZView* view);

  void registerROIForSubtraction(ZROI* roi, int slice, size_t shapeID);

  void removeROIForSubtraction();

  void performROISubtraction(const ZROI* roi, int slice, size_t shapeID);

  QPointF lastPressedPoint()
  { return m_lastPressedPt; }

  void escKeyPressed();

signals:

  void mousePressed(QPointF);

  void mouseMoved(QPointF);

  void mouseReleased(QPointF);

protected:
  void contextMenuEvent(QGraphicsSceneContextMenuEvent *contextMenuEvent) override;

  void mousePressEvent(QGraphicsSceneMouseEvent* event) override;

  void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;

  void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

  void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;

  [[nodiscard]] static inline bool isScenePtOverlap(const QPointF& p1, const QPointF& p2)
  { return p1 == p2 || p1.toPoint() == p2.toPoint(); }

private:
  ZView* m_view;

  ZROI* m_roi = nullptr;
  int m_slice = -1;
  size_t m_shapeID = 0;

  ROIAction m_roiAction = ROIAction::New;
  QPointF m_startScenePt;
  std::unique_ptr<QGraphicsPolygonItem> m_startPtItem;
  std::vector<std::unique_ptr<QGraphicsPolygonItem>> m_ctrlPtsItem;
  std::unique_ptr<QGraphicsEllipseItem> m_ellipseItem;
  std::unique_ptr<QGraphicsRectItem> m_rectItem;
  QPolygonF m_polygon;
  std::unique_ptr<QGraphicsPathItem> m_polygonItem;
  QPolygonF m_spline;
  std::unique_ptr<QGraphicsPathItem> m_splineItem;

  QPointF m_lastPressedPt;

  double m_zValue = 60000;
};

} // namespace nim

