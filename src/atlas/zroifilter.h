#ifndef ZROIFILTER_H
#define ZROIFILTER_H

#include "zobjfilter.h"
#include <QList>
#include <vector>
#include "zparameter.h"
#include <QGraphicsRectItem>
#include <QGraphicsPathItem>
#include <map>
#include "znumericparameter.h"
#include <vector>
#include "zroi.h"

class ZWidgetsGroup;

namespace nim {

class ROIGraphicsItem : public QGraphicsPathItem
{
public:
  enum
  {
    Type = UserType + 5
  };

  int type() const override
  { return Type; }

  ROIGraphicsItem(ZROI& roi, int slice, double z = 100, QGraphicsItem* parent = nullptr);

  void updateValue();

  void setOffset(double x, double y);

protected:
  //virtual void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
  //virtual void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override;
  virtual QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

  virtual void contextMenuEvent(QGraphicsSceneContextMenuEvent* event) override;

private:
  ZROI& m_roi;
  int m_slice;

  QPointF m_basePos;
  QPointF m_offset;
};

class ROICtrlPtGraphicsItem : public QGraphicsRectItem
{
public:
  enum
  {
    Type = UserType + 6
  };

  int type() const override
  { return Type; }

  ROICtrlPtGraphicsItem(ZROI& roi, const ZROIControlPoint& controlPoint,
                        double viewScale = 1., double z = 100, QGraphicsItem* parent = nullptr);

  void updateValue();

  void setViewScale(double s);

  ZROIControlPoint controlPoint() const
  { return m_controlPoint; }

  virtual void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget = nullptr) override;

  void setOffset(double x, double y);

protected:
  virtual QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

private:
  ZROI& m_roi;
  ZROIControlPoint m_controlPoint;
  double m_viewScale;

  const ZROIShapeOperation& m_shapeOp;

  QPointF m_basePos;
  QPointF m_offset;
};

class ZROIFilter : public ZObjFilter
{
Q_OBJECT
public:
  ZROIFilter(ZView& view);

  void setData(ZROI& roi);

  void releaseItemsOwnership();

  void setVisible(bool v)
  { m_visible.set(v); }

  void setSelected(bool v)
  { Q_UNUSED(v) }

  void setNormalView(int z, int t) override;

  void setMaxZProjView(int t) override;

  void setOutlineColor(glm::vec3 col)
  { m_outlineColor.set(col); }

  void setRegionColor(glm::vec3 col)
  { m_regionColor.set(col); }

  std::vector<int> boundBox() const;

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroup();

  std::shared_ptr<ZWidgetsGroup> viewSettingWidgetsGroupForAnnotationFilter();

  virtual void deleteKeyPressed() override;

  virtual void mousePressed(const QPointF& scenePos) override;

  virtual void mouseReleased(const QPointF& scenePos) override;

  virtual void rotateClockwise() override;

  virtual void rotateCounterclockwise() override;

  ZDVec4Parameter& offsetPara()
  { return m_offsetPara; }

protected:
  virtual void offsetChanged() override;

  std::vector<std::unique_ptr<ROICtrlPtGraphicsItem>> createCtrlPtItems(int slice);

private:
  void visibleChanged();

  void showControlPointsChanged();

  void outlineColorChanged();

  void regionColorChanged();

  void onRoiChanged(int slice);

  void onRoiMoved(int slice);

  void onRoiDeleted(int slice);

  void viewScaleChanged(double s);

private:
  ZROI* m_ROI;
  std::map<int, std::unique_ptr<ROIGraphicsItem>> m_sliceToROIItem;
  std::map<int, std::vector<std::unique_ptr<ROICtrlPtGraphicsItem>>> m_sliceToCtrlPtItems;

  ZBoolParameter m_visible;
  ZBoolParameter m_showControlPoints;
  ZVec3Parameter m_outlineColor;
  ZVec3Parameter m_regionColor;
  ZDoubleParameter m_opacity;
  bool m_sliceValid;

  std::shared_ptr<ZWidgetsGroup> m_widgetsGroup;
  bool m_hasSelectedItems;
};

} // namespace nim

#endif // ZROIFILTER_H
