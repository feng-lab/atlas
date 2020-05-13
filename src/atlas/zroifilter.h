#pragma once

#include "zobjfilter.h"
#include "zparameter.h"
#include "znumericparameter.h"
#include "zroi.h"
#include "zgraphicsitemtype.h"
#include <QGraphicsRectItem>
#include <QGraphicsPathItem>
#include <QList>
#include <QPointF>
#include <map>
#include <vector>

namespace nim {

class ZWidgetsGroup;

struct RegionNode;

class SliceROIGraphicsItem : public QGraphicsPathItem
{
public:
  enum
  {
    Type = GraphicsItemType::SliceROIGraphicsItem
  };

  int type() const override
  { return Type; }

  SliceROIGraphicsItem(ZROI& roi, int slice, QGraphicsItem* parent = nullptr);

  void updateValue();

protected:

  void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;

private:
  ZROI& m_roi;
  int m_slice;

  QPointF m_basePos;
};

class ROIGraphicsItem : public QGraphicsPathItem
{
public:
  enum
  {
    Type = GraphicsItemType::ROIGraphicsItem
  };

  int type() const override
  { return Type; }

  ROIGraphicsItem(ZROI& roi, int slice, size_t id, ZView& view, const RegionNode* regionNode = nullptr,
                  QGraphicsItem* parent = nullptr);

  void setHighlightOnHover(bool v)
  { setAcceptHoverEvents(v); }

  void updateValue();

  QPainterPath shape() const override;

protected:
  //void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;

  // void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;
//
  QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

  void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;

  void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;

  void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

private:
  ZROI& m_roi;
  int m_slice;
  size_t m_id;
  ZView& m_view;
  const RegionNode* m_regionNode = nullptr;

  QPointF m_basePos;
};

class ROICtrlPtGraphicsItem : public QGraphicsRectItem
{
public:
  enum
  {
    Type = GraphicsItemType::ROICtrlPtGraphicsItem
  };

  int type() const override
  { return Type; }

  ROICtrlPtGraphicsItem(ZROI& roi, const ZROIControlPoint& controlPoint, const QTransform& tfm, ZView& view,
                        double viewScale = 1., const RegionNode* regionNode = nullptr,
                        QGraphicsItem* parent = nullptr);

  void updateValue();

  void setFixedSize(bool v);

  void setViewScale(double s);

  void setTransform_(const QTransform& tfm)
  { m_transform = tfm; updateValue(); }

  ZROIControlPoint controlPoint() const
  { return m_controlPoint; }

  // void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget = nullptr) override;

protected:
  QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

  void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;

//  void mousePressEvent(QGraphicsSceneMouseEvent* event) override;

  void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

  void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;

  void updateRectSize();

private:
  ZROI& m_roi;
  ZROIControlPoint m_controlPoint;
  double m_viewScale;
  bool m_fixedSize = true;

  QPointF m_basePos;
  bool m_doubleClicked = false;
  QTransform m_transform;
  ZView& m_view;
  const RegionNode* m_regionNode = nullptr;
};

class ZROIFilter : public ZObjFilter
{
Q_OBJECT
public:
  explicit ZROIFilter(ZView& view, const RegionNode* regionNode = nullptr);

  static int getViewPrecedence()
  {
    static int vp = 30000;
    return vp++;
  }

  void setData(ZROI& roi);

  void releaseItemsOwnership();

  void setVisible(bool v)
  { m_visible.set(v); }

  bool isVisible() const
  { return m_visible.get(); }

  void setSelected(bool v);

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  void setOutlineColor(const glm::vec3& col)
  { m_outlineColor.set(col); }

  void setRegionColor(const glm::vec3& col)
  { m_regionColor.set(col); }

  glm::vec3 outlineColor() const
  { return m_outlineColor.get(); }

  glm::vec3 regionColor() const
  { return m_regionColor.get(); }

  double opacity() const
  { return m_opacity.get(); }

  QString regionName() const;

  ZBBox<glm::ivec4> boundBox() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroupForAnnotationFilter();

  void deleteKeyPressed() override;

  void copyKeyPressed() override;

  void pasteKeyPressed(int slice, QPointF point, bool hFlip, bool vFlip) override;

  void pasteKeyPressed(int slice, QPointF point, const ZBBox<glm::ivec4>& srcBoundBox, bool hFlip, bool vFlip);

  void mousePressed(const QPointF& scenePos) override;

  void mouseMoved(const QPointF& scenePos) override;

  void mouseReleased(const QPointF& scenePos) override;

  void rotateClockwise() override;

  void rotateCounterclockwise() override;

  ZIntParameter& viewPrecedencePara()
  { return m_viewPrecedencePara; }

  Z2DTransformParameter& transformPara()
  { return m_transform; }

  ZDVec2Parameter& offsetPara()
  { return m_offsetPara; }

protected:
  void viewPrecedenceChanged() override;

  void transformChanged() override;

  void offsetChanged() override;

  void createShapeItem(int slice, size_t shapeID);

  void createCtrlPtItems(int slice, size_t shapeID);

  void selectCtrlPtItems(int slice, size_t shapeID, bool append);

  void deselectCtrlPtItems(int slice, size_t shapeID);

private:
  void visibleChanged();

  void showControlPointsChanged();

  void fixedControlPointsSizeChanged();

  void outlineColorChanged();

  void regionColorChanged();

  void opacityChanged();

  void highlightRegionOnMouseHoverChanged();

  void onRoiChanged(int slice, const std::vector<size_t>& newShapes,
                    const std::vector<size_t>& deletedShapes,
                    const std::vector<size_t>& changedShapes);

  void onRoiMoved(int slice, const std::vector<size_t>& changedShapes);

  void onRoiDeleted(int slice);

  void viewScaleChanged(double s);

private:
  ZROI* m_ROI = nullptr;
  std::map<int, std::map<size_t, std::unique_ptr<ROIGraphicsItem>>> m_sliceToROIItem;
  std::map<int, std::map<size_t, std::vector<std::unique_ptr<ROICtrlPtGraphicsItem>>>> m_sliceToCtrlPtItems;

  ZBoolParameter m_visible;
  ZBoolParameter m_showControlPoints;
  ZBoolParameter m_fixedControlPointsSize;
  ZVec3Parameter m_outlineColor;
  ZVec3Parameter m_regionColor;
  ZDoubleParameter m_opacity;
  ZBoolParameter m_highlightRegionOnMouseHover;
  bool m_sliceValid;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  bool m_hasSelectedItems = false;
  QPointF m_startPoint;

  const RegionNode* m_regionNode = nullptr;
};

} // namespace nim

