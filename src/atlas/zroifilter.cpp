#include "zroifilter.h"

#include "zroi.h"
#include "zroidoc.h"
#include "zroiview.h"
#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include "zgraphicsview.h"
#include <QGraphicsSceneContextMenuEvent>
#include <QToolTip>
#include <boost/math/constants/constants.hpp>

namespace nim {

ROIGraphicsItem::ROIGraphicsItem(ZROI& roi, int slice, double z, QGraphicsItem* parent)
  : QGraphicsPathItem(parent)
  , m_roi(roi)
  , m_slice(slice)
{
  //todo: uncomment this when we have undo
  //setFlags(QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsMovable |
  //         QGraphicsItem::ItemIsSelectable);
  QPainterPath path = m_roi.slicePaintPath(m_slice);
  QPointF topLeft = path.boundingRect().topLeft();
  path.translate(-topLeft);
  setPath(path);
  m_basePos = topLeft;
  setPos(m_basePos + m_offset);
  //todo: uncomment this when we have undo
  //setCursor(Qt::OpenHandCursor);
  setZValue(z);
}

void ROIGraphicsItem::updateValue()
{
  QPainterPath path = m_roi.slicePaintPath(m_slice);
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

QVariant ROIGraphicsItem::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value)
{
  if (change == ItemPositionChange && scene()) {
    QPointF newPos = value.toPointF() - m_offset;
    QRectF boundRect = path().boundingRect();
    QRectF rect = scene()->sceneRect();
    // Keep the item inside the scene rect.
    newPos.setX(qMin(rect.right() - boundRect.width(), qMax(newPos.x(), rect.left())));
    newPos.setY(qMin(rect.bottom() - boundRect.height(), qMax(newPos.y(), rect.top())));
    m_roi.sliceSetTopLeft(m_slice, newPos.x(), newPos.y());
    return newPos + m_offset;
  }
  return QGraphicsPathItem::itemChange(change, value);
}

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
                                             double viewScale, double z, QGraphicsItem* parent)
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
  setZValue(z);
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

void ROICtrlPtGraphicsItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
  if (isSelected()) {
    double halfwidth = 6. / m_viewScale;
    QRectF rect(-halfwidth, -halfwidth, halfwidth * 2, halfwidth * 2);
    setRect(rect);
  } else {
    double halfwidth = 3. / m_viewScale;
    QRectF rect(-halfwidth, -halfwidth, halfwidth * 2, halfwidth * 2);
    setRect(rect);
  }
  QGraphicsRectItem::paint(painter, option, widget);
}

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
  }
  return QGraphicsRectItem::itemChange(change, value);
}

ZROIFilter::ZROIFilter(ZView& view)
  : ZObjFilter(view)
  , m_ROI(nullptr)
  , m_visible("Visible", true)
  , m_showControlPoints("Show Control Points", true)
  , m_outlineColor("Outline Color", glm::vec3(1, 1, 0), glm::vec3(0), glm::vec3(1))
  , m_regionColor("Region Color", glm::vec3(.2, .2, .2), glm::vec3(0), glm::vec3(1))
  , m_opacity("Opacity", .5, 0., 1.)
  , m_hasSelectedItems(false)
{
  m_outlineColor.setStyle("COLOR");
  m_regionColor.setStyle("COLOR");
  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZROIFilter::visibleChanged);
  connect(&m_showControlPoints, &ZBoolParameter::valueChanged, this, &ZROIFilter::showControlPointsChanged);
  connect(&m_outlineColor, &ZVec3Parameter::valueChanged, this, &ZROIFilter::outlineColorChanged);
  connect(&m_regionColor, &ZVec3Parameter::valueChanged, this, &ZROIFilter::regionColorChanged);
  connect(&m_opacity, &ZDoubleParameter::valueChanged, this, &ZROIFilter::regionColorChanged);
  addParameter(&m_visible);
  addParameter(&m_showControlPoints);
  addParameter(&m_outlineColor);
  addParameter(&m_regionColor);
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
    for (auto it = m_ROI->cbegin(); it != m_ROI->cend(); ++it) {
      int i = it->first;
      ROIGraphicsItem* roiItem = new ROIGraphicsItem(*m_ROI, i);
      roiItem->setPen(QPen(QColor(m_outlineColor.get().x * 255,
                                  m_outlineColor.get().y * 255,
                                  m_outlineColor.get().z * 255), 0));
      roiItem->setBrush(QColor(m_regionColor.get().x * 255,
                               m_regionColor.get().y * 255,
                               m_regionColor.get().z * 255,
                               m_opacity.get() * 255));
      roiItem->setOffset(m_offsetPara.get().x, m_offsetPara.get().y);
      roiItem->setVisible((realZ() == i || m_view.isMaxZProjView()) && m_visible.get());
      m_view.scene().addItem(roiItem);

      m_sliceToROIItem.emplace(i, std::unique_ptr<ROIGraphicsItem>(roiItem));
      m_sliceToCtrlPtItems.emplace(i, createCtrlPtItems(i));
    }
  }

  connect(m_ROI, &ZROI::boundBoxChanged, this, &ZROIFilter::boundBoxChanged);
  connect(m_ROI, &ZROI::roiChanged, this, &ZROIFilter::onRoiChanged);
  connect(m_ROI, &ZROI::roiMoved, this, &ZROIFilter::onRoiMoved);
  connect(m_ROI, &ZROI::roiDeleted, this, &ZROIFilter::onRoiDeleted);
}

void ZROIFilter::releaseItemsOwnership()
{
  for (auto it = m_sliceToCtrlPtItems.begin(); it != m_sliceToCtrlPtItems.end(); ++it) {
    for (auto it1 = it->second.begin(); it1 != it->second.end(); ++it1) {
      it1->release();
    }
  }
  for (auto it = m_sliceToROIItem.begin(); it != m_sliceToROIItem.end(); ++it) {
    it->second.release();
  }
}

void ZROIFilter::setNormalView(int z, int t)
{
  Q_UNUSED(t)
  if (!m_visible.get())
    return;
  int rz = realZ(z);
  for (auto it = m_sliceToROIItem.begin(); it != m_sliceToROIItem.end(); ++it) {
    it->second->setVisible(it->first == int(rz));
  }
  if (m_showControlPoints.get()) {
    for (auto it = m_sliceToCtrlPtItems.begin(); it != m_sliceToCtrlPtItems.end(); ++it) {
      for (auto it1 = it->second.begin(); it1 != it->second.end(); ++it1) {
        (*it1)->setVisible(it->first == int(rz));
      }
    }
  }
}

void ZROIFilter::setMaxZProjView(int t)
{
  Q_UNUSED(t)
  if (!m_visible.get())
    return;
  for (auto it = m_sliceToROIItem.begin(); it != m_sliceToROIItem.end(); ++it) {
    it->second->setVisible(true);
  }
  if (m_showControlPoints.get()) {
    for (auto it = m_sliceToCtrlPtItems.begin(); it != m_sliceToCtrlPtItems.end(); ++it) {
      for (auto it1 = it->second.begin(); it1 != it->second.end(); ++it1) {
        (*it1)->setVisible(true);
      }
    }
  }
}

std::vector<int> ZROIFilter::boundBox() const
{
  std::vector<int> res = m_ROI->boundBox();
  updateBoundBoxWithOffsetPara(res);
  return res;
}

std::shared_ptr<ZWidgetsGroup> ZROIFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("ROI", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_showControlPoints, 1);
    m_widgetsGroup->addChild(m_outlineColor, 1);
    m_widgetsGroup->addChild(m_regionColor, 1);
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
  std::vector<int> slices;
  for (auto it = m_sliceToROIItem.begin(); it != m_sliceToROIItem.end(); ++it) {
    if (it->second->isSelected()) {
      slices.push_back(it->first);
    }
  }
  for (size_t i = 0; i < slices.size(); ++i) {
    m_ROI->deleteSliceROI(slices[i]);
  }

  std::vector<ZROIControlPoint> controlPoints;
  for (auto it = m_sliceToCtrlPtItems.begin(); it != m_sliceToCtrlPtItems.end(); ++it) {
    for (auto it1 = it->second.begin(); it1 != it->second.end(); ++it1) {
      if ((*it1)->isSelected()) {
        controlPoints.push_back((*it1)->controlPoint());
      }
    }
  }
  if (!controlPoints.empty())
    m_ROI->deleteROIControlPoints(controlPoints);
}

void ZROIFilter::mousePressed(const QPointF&)
{
  m_hasSelectedItems = false;
  if (m_view.isMaxZProjView()) {
    for (auto it = m_sliceToCtrlPtItems.begin(); it != m_sliceToCtrlPtItems.end(); ++it) {
      for (auto it1 = it->second.begin(); it1 != it->second.end(); ++it1) {
        if ((*it1)->isSelected()) {
          m_hasSelectedItems = true;
          break;
        }
      }
      if (m_hasSelectedItems)
        break;
    }
  } else if (m_sliceToCtrlPtItems.find(realZ()) != m_sliceToCtrlPtItems.end()) {
    for (auto it1 = m_sliceToCtrlPtItems.at(realZ()).begin();
         it1 != m_sliceToCtrlPtItems.at(realZ()).end(); ++it1) {
      if ((*it1)->isSelected()) {
        m_hasSelectedItems = true;
        break;
      }
    }
  }
  if (m_hasSelectedItems) {
    m_ROI->startMoveSelectedControlPointsCommand();
  }
}

void ZROIFilter::mouseReleased(const QPointF&)
{
  if (m_hasSelectedItems) {
    m_ROI->endMoveSelectedControlPointsCommand();
    m_hasSelectedItems = false;
  }
}

void ZROIFilter::rotateClockwise()
{
  std::vector<ZROIControlPoint> controlPoints;
  for (auto it = m_sliceToCtrlPtItems.begin(); it != m_sliceToCtrlPtItems.end(); ++it) {
    for (auto it1 = it->second.begin(); it1 != it->second.end(); ++it1) {
      if ((*it1)->isSelected()) {
        controlPoints.push_back((*it1)->controlPoint());
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
  for (auto it = m_sliceToCtrlPtItems.begin(); it != m_sliceToCtrlPtItems.end(); ++it) {
    for (auto it1 = it->second.begin(); it1 != it->second.end(); ++it1) {
      if ((*it1)->isSelected()) {
        controlPoints.push_back((*it1)->controlPoint());
      }
    }
  }
  using namespace boost::math::double_constants;
  if (!controlPoints.empty())
    m_ROI->rotateROIControlPoints(controlPoints, -5. * degree);
}

void ZROIFilter::offsetChanged()
{
  for (auto it = m_sliceToROIItem.begin(); it != m_sliceToROIItem.end(); ++it) {
    it->second->setOffset(m_offsetPara.get().x, m_offsetPara.get().y);
  }
  if (m_showControlPoints.get()) {
    for (auto it = m_sliceToCtrlPtItems.begin(); it != m_sliceToCtrlPtItems.end(); ++it) {
      for (auto it1 = it->second.begin(); it1 != it->second.end(); ++it1) {
        (*it1)->setOffset(m_offsetPara.get().x, m_offsetPara.get().y);
      }
    }
  }
}

std::vector<std::unique_ptr<ROICtrlPtGraphicsItem>> ZROIFilter::createCtrlPtItems(int slice)
{
  std::vector<std::unique_ptr<ROICtrlPtGraphicsItem>> items;
  std::vector<ZROIControlPoint> controlPoints = m_ROI->sliceControlPoints(slice);
  for (auto controlPoint : controlPoints) {
    ROICtrlPtGraphicsItem* rectItem = new ROICtrlPtGraphicsItem(*m_ROI, controlPoint,
                                                                m_view.graphicsView().currentScale());
    rectItem->setVisible((realZ() == slice || m_view.isMaxZProjView()) && m_visible.get() && m_showControlPoints.get());
    rectItem->setOffset(m_offsetPara.get().x, m_offsetPara.get().y);
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
    for (auto it = m_sliceToROIItem.begin(); it != m_sliceToROIItem.end(); ++it) {
      it->second->setVisible(false);
    }
    for (auto it = m_sliceToCtrlPtItems.begin(); it != m_sliceToCtrlPtItems.end(); ++it) {
      for (auto it1 = it->second.begin(); it1 != it->second.end(); ++it1) {
        (*it1)->setVisible(false);
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
    for (auto it = m_sliceToCtrlPtItems.begin(); it != m_sliceToCtrlPtItems.end(); ++it) {
      for (auto it1 = it->second.begin(); it1 != it->second.end(); ++it1) {
        (*it1)->setVisible(false);
      }
    }
  }
}

void ZROIFilter::outlineColorChanged()
{
  for (auto it = m_sliceToROIItem.begin(); it != m_sliceToROIItem.end(); ++it) {
    it->second->setPen(QPen(QColor(m_outlineColor.get().x * 255,
                                   m_outlineColor.get().y * 255,
                                   m_outlineColor.get().z * 255), 0));
  }
}

void ZROIFilter::regionColorChanged()
{
  for (auto it = m_sliceToROIItem.begin(); it != m_sliceToROIItem.end(); ++it) {
    it->second->setBrush(QColor(m_regionColor.get().x * 255,
                                m_regionColor.get().y * 255,
                                m_regionColor.get().z * 255,
                                m_opacity.get() * 255));
  }
}

void ZROIFilter::onRoiChanged(int slice)
{
  if (m_sliceToROIItem.find(slice) == m_sliceToROIItem.end()) {
    ROIGraphicsItem* roiItem = new ROIGraphicsItem(*m_ROI, slice);
    roiItem->setPen(QPen(QColor(m_outlineColor.get().x * 255,
                                m_outlineColor.get().y * 255,
                                m_outlineColor.get().z * 255), 0));
    roiItem->setBrush(QColor(m_regionColor.get().x * 255,
                             m_regionColor.get().y * 255,
                             m_regionColor.get().z * 255,
                             m_opacity.get() * 255));
    roiItem->setOffset(m_offsetPara.get().x, m_offsetPara.get().y);
    roiItem->setVisible((realZ() == slice || m_view.isMaxZProjView()) && m_visible.get());
    m_view.scene().addItem(roiItem);

    m_sliceToROIItem.emplace(slice, std::unique_ptr<ROIGraphicsItem>(roiItem));
  } else {
    m_sliceToROIItem.at(slice)->updateValue();
  }

  m_sliceToCtrlPtItems.erase(slice);
  m_sliceToCtrlPtItems.emplace(slice, createCtrlPtItems(slice));
}

void ZROIFilter::onRoiMoved(int slice)
{
  m_sliceToROIItem.at(slice)->updateValue();
  for (auto it1 = m_sliceToCtrlPtItems.at(slice).begin(); it1 != m_sliceToCtrlPtItems.at(slice).end(); ++it1) {
    (*it1)->updateValue();
  }
}

void ZROIFilter::onRoiDeleted(int slice)
{
  m_sliceToROIItem.erase(slice);
  m_sliceToCtrlPtItems.erase(slice);
}

void ZROIFilter::viewScaleChanged(double s)
{
  for (auto it = m_sliceToCtrlPtItems.begin(); it != m_sliceToCtrlPtItems.end(); ++it) {
    for (auto it1 = it->second.begin(); it1 != it->second.end(); ++it1) {
      (*it1)->setViewScale(s);
    }
  }
}

} // namespace nim
