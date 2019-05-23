#pragma once

#include "zobjfilter.h"
#include "zparameter.h"
#include "znumericparameter.h"
#include "zroi.h"
#include "zgraphicsitemtype.h"
#include <QGraphicsRectItem>
#include <QGraphicsPathItem>
#include <QList>
#include <map>
#include <vector>

class ZWidgetsGroup;

namespace nim {

class ROIGraphicsItem : public QGraphicsPathItem
{
public:
  enum
  {
    Type = GraphicsItemType::ROIGraphicsItem
  };

  int type() const override
  { return Type; }

  ROIGraphicsItem(ZROI& roi, int slice, size_t id, QGraphicsItem* parent = nullptr);

  void updateValue();

  void setOffset(double x, double y);

protected:
  //void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
  //void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override;
  // QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

  void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;

private:
  ZROI& m_roi;
  int m_slice;
  size_t m_id;

  QPointF m_basePos;
  QPointF m_offset;
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

  ROICtrlPtGraphicsItem(ZROI& roi, const ZROIControlPoint& controlPoint,
                        double viewScale = 1., QGraphicsItem* parent = nullptr);

  void updateValue();

  void setFixedSize(bool v);

  void setViewScale(double s);

  ZROIControlPoint controlPoint() const
  { return m_controlPoint; }

  // void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget = nullptr) override;

  void setOffset(double x, double y);

protected:
  QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

  void updateRectSize();

private:
  ZROI& m_roi;
  ZROIControlPoint m_controlPoint;
  double m_viewScale;
  bool m_fixedSize = true;

  const ZROIShapeOperation& m_shapeOp;

  QPointF m_basePos;
  QPointF m_offset;
};

class ZROIFilter : public ZObjFilter
{
Q_OBJECT
public:
  explicit ZROIFilter(ZView& view);

  void setData(ZROI& roi);

  void releaseItemsOwnership();

  void setVisible(bool v)
  { m_visible.set(v); }

  void setSelected(bool v);

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  void setOutlineColor(const glm::vec3& col)
  { m_outlineColor.set(col); }

  void setRegionColor(const glm::vec3& col)
  { m_regionColor.set(col); }

  ZBBox<glm::ivec4> boundBox() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroupForAnnotationFilter();

  void deleteKeyPressed() override;

  void mousePressed(const QPointF& scenePos) override;

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

private:
  void visibleChanged();

  void showControlPointsChanged();

  void fixedControlPointsSizeChanged();

  void outlineColorChanged();

  void regionColorChanged();

  void opacityChanged();

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
  bool m_sliceValid;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  bool m_hasSelectedItems = false;

  bool m_lazyRendering = true;
};

} // namespace nim

