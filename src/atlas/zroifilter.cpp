#include "zroifilter.h"

#include "zroi.h"
#include "zroidoc.h"
#include "zroiview.h"
#include "zgraphicsview.h"
#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include <QGraphicsSceneContextMenuEvent>
#include <QToolTip>
#include <QPushButton>
#include <boost/math/constants/constants.hpp>

namespace nim {

ROIGraphicsItem::ROIGraphicsItem(ZROI& roi, int slice, size_t id, QGraphicsItem* parent)
  : QGraphicsPathItem(parent)
  , m_roi(roi)
  , m_slice(slice)
  , m_id(id)
{
  // setFlags(QGraphicsItem::ItemIsSelectable);
  //todo: uncomment this when we have undo
  //setFlags(QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsMovable |
  //         QGraphicsItem::ItemIsSelectable);
  QPainterPath path = m_roi.shapePainterPath(m_slice, m_id);
  QPointF topLeft = path.boundingRect().topLeft();
  path.translate(-topLeft);
  setPath(path);
  m_basePos = topLeft;
  setPos(m_basePos + m_offset);
  //todo: uncomment this when we have undo
  //setCursor(Qt::OpenHandCursor);
}

void ROIGraphicsItem::updateValue()
{
  QPainterPath path = m_roi.shapePainterPath(m_slice, m_id);
  QPointF topLeft = path.boundingRect().topLeft();
  path.translate(-topLeft);
  setPath(path);
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
  m_basePos = topLeft;
  setPos(m_basePos + m_offset);
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
}

void ROIGraphicsItem::setOffset(double x, double y)
{
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
  m_offset = QPointF(x, y);
  setPos(m_basePos + m_offset);
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
}

//void ROIGraphicsItem::mousePressEvent(QGraphicsSceneMouseEvent *event)
//{
//  setCursor(Qt::ClosedHandCursor);
//  QGraphicsPathItem::mousePressEvent(event);
//}

//void ROIGraphicsItem::mouseReleaseEvent(QGraphicsSceneMouseEvent *event)
//{
//  setCursor(Qt::OpenHandCursor);
//  QGraphicsPathItem::mouseReleaseEvent(event);
//}

//QVariant ROIGraphicsItem::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value)
//{
//  if (change == ItemPositionChange && scene()) {
//    QPointF newPos = value.toPointF() - m_offset;
//    QRectF boundRect = path().boundingRect();
//    QRectF rect = scene()->sceneRect();
//    // Keep the item inside the scene rect.
//    newPos.setX(qMin(rect.right() - boundRect.width(), qMax(newPos.x(), rect.left())));
//    newPos.setY(qMin(rect.bottom() - boundRect.height(), qMax(newPos.y(), rect.top())));
//    m_roi.sliceSetTopLeft(m_slice, newPos.x(), newPos.y());
//    return newPos + m_offset;
//  }
//  return QGraphicsPathItem::itemChange(change, value);
//}

void ROIGraphicsItem::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
{
  if (m_roi.sliceHasPolyOrSpline(m_slice)) {
    QMenu menu;
    QAction* addCtrlPointAction = menu.addAction("Add Ctrl Point Here");
    QAction* selectedAction = menu.exec(event->screenPos());
    if (selectedAction == addCtrlPointAction) {
      m_roi.sliceAddCtrlPoint(m_slice, event->scenePos() - m_offset);
    }
  }
}

ROICtrlPtGraphicsItem::ROICtrlPtGraphicsItem(ZROI& roi, const ZROIControlPoint& controlPoint,
                                             double viewScale, QGraphicsItem* parent)
  : QGraphicsRectItem(parent)
  , m_roi(roi)
  , m_controlPoint(controlPoint)
  , m_viewScale(viewScale)
  , m_shapeOp(m_roi.controlPointShapeOp(m_controlPoint))
{
  if (m_shapeOp.type == ROIType::Polygon || m_shapeOp.type == ROIType::Spline) {
    setFlags(QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsMovable |
             QGraphicsItem::ItemIsSelectable);
  } else {
    setFlags(QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsMovable |
             QGraphicsItem::ItemIsSelectable);
  }

  m_basePos = m_roi.controlPointCoord(m_controlPoint);
  setPos(m_basePos + m_offset);
  setToolTip(
    QString("Coord:(%1,%2), Offset:(%3,%4)").arg(m_basePos.x()).arg(m_basePos.y()).arg(m_offset.x()).arg(m_offset.y()));
  setPen(QPen(QColor(0, 0, 0), 0));
  setBrush(QBrush(QColor(255, 255, 255)));
  setCursor(Qt::PointingHandCursor);

  if (isSelected()) {
    double halfwidth = 1.5; // / m_viewScale;
    QRectF rect(-halfwidth, -halfwidth, halfwidth * 2, halfwidth * 2);
    setRect(rect);
  } else {
    double halfwidth = 1.; // / m_viewScale;
    QRectF rect(-halfwidth, -halfwidth, halfwidth * 2, halfwidth * 2);
    setRect(rect);
  }
}

void ROICtrlPtGraphicsItem::updateValue()
{
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
  m_basePos = m_roi.controlPointCoord(m_controlPoint);
  setPos(m_basePos + m_offset);
  setToolTip(
    QString("Coord:(%1,%2), Offset:(%3,%4)").arg(m_basePos.x()).arg(m_basePos.y()).arg(m_offset.x()).arg(m_offset.y()));
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
}

void ROICtrlPtGraphicsItem::setViewScale(double s)
{
  m_viewScale = s;
  if (isSelected()) {
    double halfwidth = 6. / m_viewScale;
    QRectF rect(-halfwidth, -halfwidth, halfwidth * 2, halfwidth * 2);
    setRect(rect);
  } else {
    double halfwidth = 3. / m_viewScale;
    QRectF rect(-halfwidth, -halfwidth, halfwidth * 2, halfwidth * 2);
    setRect(rect);
  }
}

//void ROICtrlPtGraphicsItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
//{
//  if (isSelected()) {
//    double halfwidth = 1.5; // / m_viewScale;
//    QRectF rect(-halfwidth, -halfwidth, halfwidth * 2, halfwidth * 2);
//    setRect(rect);
//  } else {
//    double halfwidth = 1.; // / m_viewScale;
//    QRectF rect(-halfwidth, -halfwidth, halfwidth * 2, halfwidth * 2);
//    setRect(rect);
//  }
//  QGraphicsRectItem::paint(painter, option, widget);
//}

void ROICtrlPtGraphicsItem::setOffset(double x, double y)
{
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
  m_offset = QPointF(x, y);
  setPos(m_basePos + m_offset);
  setToolTip(
    QString("Coord:(%1,%2), Offset:(%3,%4)").arg(m_basePos.x()).arg(m_basePos.y()).arg(m_offset.x()).arg(m_offset.y()));
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
}

QVariant ROICtrlPtGraphicsItem::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value)
{
  if (change == ItemPositionChange && scene()) {
    QPointF newPos = value.toPointF() - m_offset;
    QRectF sceneRect = scene()->sceneRect();
    if (true) {
      // Keep the item inside the scene rect.
      if (m_controlPoint.pos == ZROIControlPoint::Pos::Center) {
        newPos.setX(qMin(sceneRect.right() - m_shapeOp.rect().width() / 2,
                         qMax(newPos.x(), sceneRect.left() + m_shapeOp.rect().width() / 2)));
        newPos.setY(qMin(sceneRect.bottom() - m_shapeOp.rect().height() / 2,
                         qMax(newPos.y(), sceneRect.top() + m_shapeOp.rect().height() / 2)));
      } else {
        newPos.setX(qMin(sceneRect.right(), qMax(newPos.x(), sceneRect.left())));
        newPos.setY(qMin(sceneRect.bottom(), qMax(newPos.y(), sceneRect.top())));
      }
    }
    return m_roi.setControlPointCoord(m_controlPoint, newPos) + m_offset;
  } else if (change == ItemSelectedHasChanged && scene()) {
    if (value.toBool()) {
      double halfwidth = 1.5; // / m_viewScale;
      QRectF rect(-halfwidth, -halfwidth, halfwidth * 2, halfwidth * 2);
      setRect(rect);
    } else {
      double halfwidth = 1.; // / m_viewScale;
      QRectF rect(-halfwidth, -halfwidth, halfwidth * 2, halfwidth * 2);
      setRect(rect);
    }
    return value;
  }
  return QGraphicsRectItem::itemChange(change, value);
}

ZROIFilter::ZROIFilter(ZView& view)
  : ZObjFilter(view)
  , m_visible("Visible", true)
  , m_showControlPoints("Show Control Points", true)
  , m_outlineColor("Outline Color", glm::vec3(1, 1, 0), glm::vec3(0), glm::vec3(1))
  , m_regionColor("Region Color", glm::vec3(.2, .2, .2), glm::vec3(0), glm::vec3(1))
  , m_opacity("Opacity", .5, 0., 1.)
{
  m_outlineColor.setStyle("COLOR");
  m_regionColor.setStyle("COLOR");
  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZROIFilter::visibleChanged);
  connect(&m_showControlPoints, &ZBoolParameter::valueChanged, this, &ZROIFilter::showControlPointsChanged);
  connect(&m_outlineColor, &ZVec3Parameter::valueChanged, this, &ZROIFilter::outlineColorChanged);
  connect(&m_regionColor, &ZVec3Parameter::valueChanged, this, &ZROIFilter::regionColorChanged);
  connect(&m_opacity, &ZDoubleParameter::valueChanged, this, &ZROIFilter::opacityChanged);
  addParameter(&m_visible);
  addParameter(&m_showControlPoints);
  addParameter(&m_outlineColor);
  addParameter(&m_regionColor);
  m_viewPrecedencePara.blockSignals(true);
  m_viewPrecedencePara.set(3000);
  m_viewPrecedencePara.blockSignals(false);
  addParameter(&m_viewPrecedencePara);
  addParameter(&m_transform);
  addParameter(&m_offsetPara);
  addParameter(&m_opacity);

  connect(&view.graphicsView(), &ZGraphicsView::scaleChanged, this, &ZROIFilter::viewScaleChanged);
}

void ZROIFilter::setData(ZROI& roi)
{
  m_ROI = &roi;
  m_sliceToROIItem.clear();
  m_sliceToCtrlPtItems.clear();
  if (!m_ROI->isEmpty()) {
    for (const auto& sliceROI : *m_ROI) {
      int slice = sliceROI.first;
      auto& shapeItems = m_sliceToROIItem[slice];
      auto& shapeCtrlPtItems = m_sliceToCtrlPtItems[slice];
      for (auto shapeID : m_ROI->sliceShapeIDs(slice)) {
        auto roiItem = new ROIGraphicsItem(*m_ROI, slice, shapeID);
        roiItem->setZValue(m_viewPrecedencePara.get());
        roiItem->setPen(QPen(QColor(m_outlineColor.get().x * 255,
                                    m_outlineColor.get().y * 255,
                                    m_outlineColor.get().z * 255),
                             0));
        roiItem->setBrush(QColor(m_regionColor.get().x * 255,
                                 m_regionColor.get().y * 255,
                                 m_regionColor.get().z * 255,
                                 m_opacity.get() * 255));
        //roiItem->setOpacity(m_opacity.get());
        roiItem->setTransform(getQTransform());
        roiItem->setVisible((realZ() == slice || m_view.isMaxZProjView()) && m_visible.get());
        m_view.scene().addItem(roiItem);

        shapeItems[shapeID] = std::unique_ptr<ROIGraphicsItem>(roiItem);

        shapeCtrlPtItems[shapeID] = createCtrlPtItems(slice, shapeID);
      }
    }
  }

  connect(m_ROI, &ZROI::boundBoxChanged, this, &ZROIFilter::boundBoxChanged);
  connect(m_ROI, &ZROI::roiChanged, this, &ZROIFilter::onRoiChanged);
  connect(m_ROI, &ZROI::roiMoved, this, &ZROIFilter::onRoiMoved);
  connect(m_ROI, &ZROI::roiDeleted, this, &ZROIFilter::onRoiDeleted);
}

void ZROIFilter::releaseItemsOwnership()
{
  for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
    for (auto&[id, ctrlItems] : sliceItem) {
      for (auto& item : ctrlItems) {
        item.release();
      }
    }
  }
  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      shapeItem.release();
    }
  }
}

void ZROIFilter::setSelected(bool v)
{
  for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
    for (auto&[id, ctrlItems] : sliceItem) {
      for (auto& item : ctrlItems) {
        if (item->isSelected() != v) {
          item->setSelected(v);
        }
      }
    }
  }
  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      if (shapeItem->isSelected() != v) {
        shapeItem->setSelected(v);
      }
    }
  }
}

void ZROIFilter::setNormalView(int z, int /*t*/)
{
  if (!m_visible.get())
    return;
  int rz = realZ(z);
  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      shapeItem->setVisible(slice == int(rz));
    }
  }
  if (m_showControlPoints.get()) {
    for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
      for (auto&[id, ctrlItems] : sliceItem) {
        for (auto& item : ctrlItems) {
          item->setVisible(slice == int(rz));
        }
      }
    }
  }
}

void ZROIFilter::setMaxZProjView(int /*t*/)
{
  if (!m_visible.get())
    return;
  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      shapeItem->setVisible(true);
    }
  }
  if (m_showControlPoints.get()) {
    for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
      for (auto&[id, ctrlItems] : sliceItem) {
        for (auto& item : ctrlItems) {
          item->setVisible(true);
        }
      }
    }
  }
}

ZBBox<glm::ivec4> ZROIFilter::boundBox() const
{
  ZBBox<glm::ivec4> res = m_ROI->boundBox();
  updateBoundBoxWithOffsetPara(res);
  return res;
}

std::shared_ptr<ZWidgetsGroup> ZROIFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("ROI", 1);
    m_widgetsGroup->addChild(m_visible, 1);

    QPushButton* pb = new QPushButton("Bring to Front");
    connect(pb, &QPushButton::clicked, this, &ZROIFilter::bringToFront);
    m_widgetsGroup->addChild(*pb, 1);

    pb = new QPushButton("Send to Back");
    connect(pb, &QPushButton::clicked, this, &ZROIFilter::sendToBack);
    m_widgetsGroup->addChild(*pb, 1);

    m_widgetsGroup->addChild(m_viewPrecedencePara, 1);
    m_widgetsGroup->addChild(m_showControlPoints, 1);
    m_widgetsGroup->addChild(m_outlineColor, 1);
    m_widgetsGroup->addChild(m_regionColor, 1);
    m_widgetsGroup->addChild(m_transform, 1);
    m_widgetsGroup->addChild(m_offsetPara, 1);
    m_widgetsGroup->addChild(m_opacity, 1);
  }
  return m_widgetsGroup;
}

std::shared_ptr<ZWidgetsGroup> ZROIFilter::viewSettingWidgetsGroupForAnnotationFilter()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_showControlPoints, 1);
    m_widgetsGroup->addChild(m_outlineColor, 1);
    m_widgetsGroup->addChild(m_regionColor, 1);
    m_widgetsGroup->addChild(m_opacity, 1);
  }
  return m_widgetsGroup;
}

void ZROIFilter::deleteKeyPressed()
{
//  std::vector<int> slices;
//  for (const auto& sliceItem : m_sliceToROIItem) {
//    if (sliceItem.second->isSelected()) {
//      slices.push_back(sliceItem.first);
//    }
//  }
//  for (size_t i = 0; i < slices.size(); ++i) {
//    m_ROI->deleteSliceROI(slices[i]);
//  }

  std::vector<ZROIControlPoint> controlPoints;
  for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
    for (auto&[id, ctrlItems] : sliceItem) {
      for (auto& item : ctrlItems) {
        if (item->isSelected()) {
          controlPoints.push_back(item->controlPoint());
        }
      }
    }
  }
  if (!controlPoints.empty())
    m_ROI->deleteROIControlPoints(controlPoints);
}

void ZROIFilter::mousePressed(const QPointF& /*scenePos*/)
{
  m_hasSelectedItems = false;
  if (m_view.isMaxZProjView()) {
    for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
      for (auto&[id, ctrlItems] : sliceItem) {
        for (auto& item : ctrlItems) {
          if (item->isSelected()) {
            m_hasSelectedItems = true;
            break;
          }
        }
        if (m_hasSelectedItems)
          break;
      }
      if (m_hasSelectedItems)
        break;
    }
  } else if (m_sliceToCtrlPtItems.find(realZ()) != m_sliceToCtrlPtItems.end()) {
    for (auto&[id, ctrlItems] : m_sliceToCtrlPtItems.at(realZ())) {
      for (auto& item : ctrlItems) {
        if (item->isSelected()) {
          m_hasSelectedItems = true;
          break;
        }
      }
      if (m_hasSelectedItems)
        break;
    }
  }
  if (m_hasSelectedItems) {
    m_ROI->startMoveSelectedControlPointsCommand();
  }
}

void ZROIFilter::mouseReleased(const QPointF& /*scenePos*/)
{
  if (m_hasSelectedItems) {
    m_ROI->endMoveSelectedControlPointsCommand();
    m_hasSelectedItems = false;
  }
}

void ZROIFilter::rotateClockwise()
{
  std::vector<ZROIControlPoint> controlPoints;
  for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
    for (auto&[id, ctrlItems] : sliceItem) {
      for (auto& item : ctrlItems) {
        if (item->isSelected()) {
          controlPoints.push_back(item->controlPoint());
        }
      }
    }
  }
  using namespace boost::math::double_constants;
  if (!controlPoints.empty())
    m_ROI->rotateROIControlPoints(controlPoints, 5. * degree);
}

void ZROIFilter::rotateCounterclockwise()
{
  std::vector<ZROIControlPoint> controlPoints;
  for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
    for (auto&[id, ctrlItems] : sliceItem) {
      for (auto& item : ctrlItems) {
        if (item->isSelected()) {
          controlPoints.push_back(item->controlPoint());
        }
      }
    }
  }
  using namespace boost::math::double_constants;
  if (!controlPoints.empty())
    m_ROI->rotateROIControlPoints(controlPoints, -5. * degree);
}

void ZROIFilter::viewPrecedenceChanged()
{
  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      shapeItem->setZValue(m_viewPrecedencePara.get());
    }
  }
  if (m_showControlPoints.get()) {
    for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
      for (auto&[id, ctrlItems] : sliceItem) {
        for (auto& item : ctrlItems) {
          item->setZValue(m_viewPrecedencePara.get());
        }
      }
    }
  }
  ZObjFilter::viewPrecedenceChanged();
}

void ZROIFilter::transformChanged()
{
  QTransform trans = getQTransform();

  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      shapeItem->setTransform(trans);
    }
  }
  if (m_showControlPoints.get()) {
    for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
      for (auto&[id, ctrlItems] : sliceItem) {
        for (auto& item : ctrlItems) {
          item->setTransform(trans);
        }
      }
    }
  }
  ZObjFilter::transformChanged();
}

void ZROIFilter::offsetChanged()
{
}

std::vector<std::unique_ptr<ROICtrlPtGraphicsItem>> ZROIFilter::createCtrlPtItems(int slice)
{
  std::vector<std::unique_ptr<ROICtrlPtGraphicsItem>> items;
  std::vector<ZROIControlPoint> controlPoints = m_ROI->sliceControlPoints(slice);
  for (const auto& controlPoint : controlPoints) {
    ROICtrlPtGraphicsItem* rectItem = new ROICtrlPtGraphicsItem(*m_ROI, controlPoint,
                                                                m_view.graphicsView().currentScale());
    rectItem->setZValue(m_viewPrecedencePara.get());
    rectItem->setVisible((realZ() == slice || m_view.isMaxZProjView()) && m_visible.get() && m_showControlPoints.get());
    rectItem->setTransform(getQTransform());
    m_view.scene().addItem(rectItem);
    items.emplace_back(rectItem);
  }

  return items;
}

std::vector<std::unique_ptr<ROICtrlPtGraphicsItem>> ZROIFilter::createCtrlPtItems(int slice, size_t shapeID)
{
  std::vector<std::unique_ptr<ROICtrlPtGraphicsItem>> items;
  std::vector<ZROIControlPoint> controlPoints = m_ROI->sliceControlPoints(slice, shapeID);
  for (const auto& controlPoint : controlPoints) {
    ROICtrlPtGraphicsItem* rectItem = new ROICtrlPtGraphicsItem(*m_ROI, controlPoint,
                                                                m_view.graphicsView().currentScale());
    rectItem->setZValue(m_viewPrecedencePara.get());
    rectItem->setVisible((realZ() == slice || m_view.isMaxZProjView()) && m_visible.get() && m_showControlPoints.get());
    rectItem->setTransform(getQTransform());
    m_view.scene().addItem(rectItem);
    items.emplace_back(rectItem);
  }

  return items;
}

void ZROIFilter::visibleChanged()
{
  if (m_visible.get()) {
    if (m_view.isMaxZProjView()) {
      setMaxZProjView(m_view.currentTime());
    } else {
      setNormalView(m_view.currentSlice(), m_view.currentTime());
    }
  } else {
    for (auto&[slice, sliceItem] : m_sliceToROIItem) {
      for (auto&[id, shapeItem] : sliceItem) {
        shapeItem->setVisible(false);
      }
    }
    for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
      for (auto&[id, ctrlItems] : sliceItem) {
        for (auto& item : ctrlItems) {
          item->setVisible(false);
        }
      }
    }
  }
}

void ZROIFilter::showControlPointsChanged()
{
  if (m_showControlPoints.get()) {
    if (m_view.isMaxZProjView()) {
      setMaxZProjView(m_view.currentTime());
    } else {
      setNormalView(m_view.currentSlice(), m_view.currentTime());
    }
  } else {
    for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
      for (auto&[id, ctrlItems] : sliceItem) {
        for (auto& item : ctrlItems) {
          item->setVisible(false);
        }
      }
    }
  }
}

void ZROIFilter::outlineColorChanged()
{
  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      shapeItem->setPen(QPen(QColor(m_outlineColor.get().x * 255,
                                    m_outlineColor.get().y * 255,
                                    m_outlineColor.get().z * 255),
                             0));
    }
  }
}

void ZROIFilter::regionColorChanged()
{
  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      shapeItem->setBrush(QColor(m_regionColor.get().x * 255,
                                 m_regionColor.get().y * 255,
                                 m_regionColor.get().z * 255,
                                 m_opacity.get() * 255));
    }
  }
}

void ZROIFilter::opacityChanged()
{
  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      shapeItem->setBrush(QColor(m_regionColor.get().x * 255,
                                 m_regionColor.get().y * 255,
                                 m_regionColor.get().z * 255,
                                 m_opacity.get() * 255));
    }
  }
}

void ZROIFilter::onRoiChanged(int slice, const std::vector<size_t>& newShapes,
                              const std::vector<size_t>& deletedShapes,
                              const std::vector<size_t>& changedShapes)
{
  if (newShapes.empty() && deletedShapes.empty() && changedShapes.empty()) {
    m_sliceToCtrlPtItems.erase(slice);
    m_sliceToROIItem.erase(slice);
    if (m_ROI->hasSlice(slice)) {
      auto& shapeItems = m_sliceToROIItem[slice];
      auto& shapeCtrlPtItems = m_sliceToCtrlPtItems[slice];
      for (auto shapeID : m_ROI->sliceShapeIDs(slice)) {
        auto roiItem = new ROIGraphicsItem(*m_ROI, slice, shapeID);
        roiItem->setZValue(m_viewPrecedencePara.get());
        roiItem->setPen(QPen(QColor(m_outlineColor.get().x * 255,
                                    m_outlineColor.get().y * 255,
                                    m_outlineColor.get().z * 255),
                             0));
        roiItem->setBrush(QColor(m_regionColor.get().x * 255,
                                 m_regionColor.get().y * 255,
                                 m_regionColor.get().z * 255,
                                 m_opacity.get() * 255));
        //roiItem->setOpacity(m_opacity.get());
        roiItem->setTransform(getQTransform());
        roiItem->setVisible((realZ() == slice || m_view.isMaxZProjView()) && m_visible.get());
        m_view.scene().addItem(roiItem);

        shapeItems[shapeID] = std::unique_ptr<ROIGraphicsItem>(roiItem);
        shapeCtrlPtItems[shapeID] = createCtrlPtItems(slice, shapeID);
      }
    }
  } else {
    if (!deletedShapes.empty()) {
      for (auto shapeID : deletedShapes) {
        m_sliceToROIItem.at(slice).erase(shapeID);
        m_sliceToCtrlPtItems.at(slice).erase(shapeID);
      }
    }
    if (!changedShapes.empty()) {
      for (auto shapeID : changedShapes) {
        m_sliceToROIItem[slice][shapeID]->updateValue();
        m_sliceToCtrlPtItems.at(slice).erase(shapeID);
        m_sliceToCtrlPtItems.at(slice)[shapeID] = createCtrlPtItems(slice, shapeID);
      }
    }
    if (!newShapes.empty()) {
      auto& shapeItems = m_sliceToROIItem[slice];
      auto& ctrlItems = m_sliceToCtrlPtItems[slice];
      for (auto shapeID : newShapes) {
        auto roiItem = new ROIGraphicsItem(*m_ROI, slice, shapeID);
        roiItem->setZValue(m_viewPrecedencePara.get());
        roiItem->setPen(QPen(QColor(m_outlineColor.get().x * 255,
                                    m_outlineColor.get().y * 255,
                                    m_outlineColor.get().z * 255),
                             0));
        roiItem->setBrush(QColor(m_regionColor.get().x * 255,
                                 m_regionColor.get().y * 255,
                                 m_regionColor.get().z * 255,
                                 m_opacity.get() * 255));
        //roiItem->setOpacity(m_opacity.get());
        roiItem->setTransform(getQTransform());
        roiItem->setVisible((realZ() == slice || m_view.isMaxZProjView()) && m_visible.get());
        m_view.scene().addItem(roiItem);

        shapeItems[shapeID] = std::unique_ptr<ROIGraphicsItem>(roiItem);

        ctrlItems[shapeID] = createCtrlPtItems(slice, shapeID);
      }
    }
  }
}

void ZROIFilter::onRoiMoved(int slice, const std::vector<size_t>& changedShapes)
{
  if (changedShapes.empty()) {
    for (auto&[id, shapeItem] : m_sliceToROIItem[slice]) {
      shapeItem->updateValue();
    }
    for (auto&[id, ctrlItems] : m_sliceToCtrlPtItems.at(slice)) {
      for (auto& item : ctrlItems) {
        item->updateValue();
      }
    }
  } else {
    for (auto shapeID : changedShapes) {
      m_sliceToROIItem[slice][shapeID]->updateValue();
      for (const auto& item : m_sliceToCtrlPtItems.at(slice)[shapeID]) {
        item->updateValue();
      }
    }
  }
}

void ZROIFilter::onRoiDeleted(int slice)
{
  m_sliceToROIItem.erase(slice);
  m_sliceToCtrlPtItems.erase(slice);
}

void ZROIFilter::viewScaleChanged(double s)
{
//  for (const auto& sliceItems : m_sliceToCtrlPtItems) {
//    for (const auto& item : sliceItems.second) {
//      item->setViewScale(s);
//    }
//  }
}

} // namespace nim
