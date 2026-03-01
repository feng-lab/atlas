#pragma once

#include "zswc.h"

#include <QGraphicsScene>
#include <QGraphicsItem>

#include <memory>
#include <vector>

namespace nim {

class ZView;

class ZROI;

class ZSwcPack;

class ZSwcFilter;

class ZGraphicsScene : public QGraphicsScene
{
  Q_OBJECT

public:
  enum class ROIAction
  {
    New,
    Add,
    Subtract
  };

  enum class SwcEditMode
  {
    Off,
    Extend,
    ConnectTo,
    MoveSelected,
    AddNode
  };

  explicit ZGraphicsScene(ZView* view);

  void registerROIForSubtraction(ZROI* roi, int slice, size_t shapeID);

  void removeROIForSubtraction();

  void performROISubtraction(const ZROI* roi, int slice, size_t shapeID);

  void enterSwcExtendMode(ZSwcFilter& filter);

  void enterSwcConnectToMode(ZSwcFilter& filter);

  void enterSwcMoveSelectedMode(ZSwcFilter& filter);

  void enterSwcAddNodeMode(ZSwcFilter& filter);

  void exitSwcEditMode();

  [[nodiscard]] SwcEditMode swcEditMode() const
  {
    return m_swcEditMode;
  }

  [[nodiscard]] ZSwcPack* swcEditPack() const
  {
    return m_swcEditPack;
  }

  [[nodiscard]] ZSwcFilter* swcEditFilter() const
  {
    return m_swcEditFilter;
  }

  [[nodiscard]] const ZSwc::SwcTreeNode& swcEditAnchorNode() const
  {
    return m_swcEditAnchorNode;
  }

  QPointF lastPressedPoint()
  {
    return m_lastPressedPt;
  }

  void escKeyPressed();

Q_SIGNALS:
  void mousePressed(QPointF, Qt::KeyboardModifiers);

  void mouseMoved(QPointF, Qt::KeyboardModifiers);

  void mouseReleased(QPointF);

protected:
  void contextMenuEvent(QGraphicsSceneContextMenuEvent* contextMenuEvent) override;

  void mousePressEvent(QGraphicsSceneMouseEvent* event) override;

  void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;

  void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

  void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;

  [[nodiscard]] static bool isScenePtOverlap(const QPointF& p1, const QPointF& p2)
  {
    return p1 == p2 || p1.toPoint() == p2.toPoint();
  }

private:
  void enterSwcExtendModeImpl(ZSwcPack& pack);

  void enterSwcConnectToModeImpl(ZSwcPack& pack);

  void enterSwcMoveSelectedModeImpl(ZSwcPack& pack);

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

  SwcEditMode m_swcEditMode = SwcEditMode::Off;
  ZSwcPack* m_swcEditPack = nullptr;
  ZSwcFilter* m_swcEditFilter = nullptr;
  ZSwc::SwcTreeNode m_swcEditAnchorNode{};

  bool m_swcMoveDragging = false;
  QPointF m_swcMoveStartScenePt;
  QPointF m_swcMoveStartSwcPt;
  QPointF m_swcMoveLastScenePt;

  double m_zValue = 60000;
};

} // namespace nim
