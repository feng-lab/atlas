#include "zroifilter.h"

#include "zroi.h"
#include "zroidoc.h"
#include "zroiview.h"
#include "zgraphicsview.h"
#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include "zchooseregiondialog.h"
#include "zregionannotation.h"
#include <QGraphicsSceneContextMenuEvent>
#include <QToolTip>
#include <QPushButton>
#include <QGraphicsItemGroup>
#include <boost/math/constants/constants.hpp>
#include <utility>

namespace nim {

SliceROIGraphicsItem::SliceROIGraphicsItem(ZROI& roi, int slice, QGraphicsItem* parent)
  : QGraphicsPathItem(parent)
  , m_roi(roi)
  , m_slice(slice)
{
  // setFlags(QGraphicsItem::ItemIsSelectable);
  //todo: uncomment this when we have undo
//  setFlags(QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsMovable |
//           QGraphicsItem::ItemIsSelectable);
  QPainterPath path = m_roi.slicePaintPath(m_slice);
  // QPointF topLeft = path.boundingRect().topLeft();
  // path.translate(-topLeft);
  setPath(path);
  // m_basePos = topLeft;
  m_basePos = QPointF(0, 0);
  setPos(m_basePos);
  //todo: uncomment this when we have undo
  //setCursor(Qt::OpenHandCursor);
}

void SliceROIGraphicsItem::updateValue()
{
  QPainterPath path = m_roi.slicePaintPath(m_slice);
  //QPointF topLeft = path.boundingRect().topLeft();
  //path.translate(-topLeft);
  setPath(path);
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
  // m_basePos = topLeft;
  m_basePos = QPointF(0, 0);
  setPos(m_basePos);
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
}

void SliceROIGraphicsItem::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
{
  if (m_roi.sliceHasPolyOrSpline(m_slice)) {
    QMenu menu;
    QAction* addCtrlPointAction = menu.addAction("Add Ctrl Point Here");
    QAction* selectedAction = menu.exec(event->screenPos());
    if (selectedAction == addCtrlPointAction) {
      m_roi.sliceAddCtrlPoint(m_slice, event->scenePos());
    }
  }
}

ROIGraphicsItem::ROIGraphicsItem(ZROI& roi, int slice, size_t id, ZView& view, const RegionNode* regionNode,
                                 QGraphicsItem* parent)
  : QGraphicsPathItem(parent)
  , m_roi(roi)
  , m_slice(slice)
  , m_id(id)
  , m_view(view)
  , m_regionNode(regionNode)
{
//   setFlags(QGraphicsItem::ItemIsSelectable);
  //todo: uncomment this when we have undo
//  setFlags(QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsMovable |
//           QGraphicsItem::ItemIsSelectable);
  QPainterPath path = m_roi.shapePainterPath(m_slice, m_id);
  //QPointF topLeft = path.boundingRect().topLeft();
  //path.translate(-topLeft);
  setPath(path);
  //m_basePos = topLeft;
  m_basePos = QPointF(0, 0);
  setPos(m_basePos);
  if (m_regionNode) {
    QString tooltip = QString("Region: %1 (%2)").arg(m_regionNode->abbreviation).arg(m_regionNode->name);
    setToolTip(tooltip);
  }
  //todo: uncomment this when we have undo
  //setCursor(Qt::OpenHandCursor);
  setAcceptHoverEvents(true);
}

void ROIGraphicsItem::updateValue()
{
  QPainterPath path = m_roi.shapePainterPath(m_slice, m_id);
  //QPointF topLeft = path.boundingRect().topLeft();
  //path.translate(-topLeft);
  setPath(path);
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
  //m_basePos = topLeft;
  m_basePos = QPointF(0, 0);
  setPos(m_basePos);
  if (m_regionNode) {
    QString tooltip = QString("Region: %1 (%2)").arg(m_regionNode->abbreviation).arg(m_regionNode->name);
    setToolTip(tooltip);
  }
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
}

QPainterPath ROIGraphicsItem::shape() const
{
  return m_roi.shapePainterPath(m_slice, m_id);
}

void ROIGraphicsItem::setLocked(bool l)
{
  m_locked = l;
  if (l) {
    setFlags(QGraphicsItem::GraphicsItemFlags());
  }
}

//void ROIGraphicsItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event)
//{
////  setCursor(Qt::ClosedHandCursor);
//  if (event->button() == Qt::LeftButton) {
//    emit m_roi.selectShape(m_slice, m_id, event->modifiers() == Qt::ControlModifier);
//  } else {
//    QGraphicsPathItem::mouseDoubleClickEvent(event);
//  }
//}

//void ROIGraphicsItem::mouseReleaseEvent(QGraphicsSceneMouseEvent *event)
//{
//  setCursor(Qt::OpenHandCursor);
//  QGraphicsPathItem::mouseReleaseEvent(event);
//}
//
QVariant ROIGraphicsItem::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value)
{
  if (m_locked) {
    return value;
  }
//  if (change == ItemPositionChange && scene()) {
//    QPointF newPos = value.toPointF() - m_offset;
////    QRectF boundRect = path().boundingRect();
////    QRectF rect = scene()->sceneRect();
////    // Keep the item inside the scene rect.
////    newPos.setX(qMin(rect.right() - boundRect.width(), qMax(newPos.x(), rect.left())));
////    newPos.setY(qMin(rect.bottom() - boundRect.height(), qMax(newPos.y(), rect.top())));
//    m_roi.sliceSetTopLeft(m_slice, newPos.x(), newPos.y());
//    return newPos + m_offset;
//  }
  if (change == ItemSelectedHasChanged && scene()) {
    auto selected = value.toBool();
    //LOG(INFO) << selected;
    if (!selected) {
      emit m_roi.deselectShape(m_slice, m_id);
    } else {
      m_view.scene().performROISubtraction(&m_roi, m_slice, m_id);
      emit m_roi.selectShape(m_slice, m_id, true);
    }
    return value;
  }
  return QGraphicsPathItem::itemChange(change, value);
}

void ROIGraphicsItem::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
{
  if (m_locked) {
    return;
  }
  const auto& shapeOps = m_roi.shapeOperations(m_slice, m_id);
  for (const auto& shapeOp : shapeOps) {
    if (shapeOp.type == ROIType::Polygon || shapeOp.type == ROIType::Spline) {
      QMenu menu;
      QAction* addCtrlPointAction = menu.addAction("Add Ctrl Point Here");
//      QAction* subtractNextSelectedShapeAction = menu.addAction("Subtract Next Selected Shape...");
      QAction* toRegionAction = nullptr;
      QAction* subtractNextSelectedShapeAction = nullptr;
      if (m_regionNode) {
        // subtractNextSelectedShapeAction = menu.addAction("Subtract Next Selected Shape...");
        toRegionAction = menu.addAction("Change Region ID...");
      } else {
        subtractNextSelectedShapeAction = menu.addAction("Subtract Next Selected Shape...");
        // toRegionAction = menu.addAction("Add ROI to Region...");
      }
      QAction* selectedAction = menu.exec(event->screenPos());
      if (selectedAction == addCtrlPointAction) {
        m_roi.sliceAddCtrlPoint(m_slice, event->scenePos(), m_id);
      } else if (toRegionAction && selectedAction == toRegionAction) {
        try {
          ZChooseRegionDialog dlg(m_view.currentRegionAnnotationPack().regionAnnotation(), &m_view.graphicsView());
          if (dlg.exec() == QDialog::Accepted) {
            CHECK(m_regionNode);
            m_view.currentRegionAnnotationPack().regionAnnotation().changeROIRegion(m_roi, m_slice, m_id,
                                                                                    dlg.selectedID());
//            if (m_regionNode) {
//              m_view.currentRegionAnnotationPack().regionAnnotation().changeROIRegion(m_roi, m_slice, m_id,
//                                                                                      dlg.selectedID());
//            } else {
//              m_view.currentRegionAnnotationPack().regionAnnotation().mergeROIToRegion(m_roi, m_slice, m_id,
//                                                                                       dlg.selectedID());
//            }
          }
        }
        catch (const ZException& e) {
          QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                                QString("Can not create RegionAnnotation:\n%1").arg(e.what()));
        }
      } else if (subtractNextSelectedShapeAction && selectedAction == subtractNextSelectedShapeAction) {
        m_view.scene().registerROIForSubtraction(&m_roi, m_slice, m_id);
      }
    }
    //event->accept();
    return;
  }
}

void ROIGraphicsItem::hoverEnterEvent(QGraphicsSceneHoverEvent*)
{
  prepareGeometryChange();
  QPen p = pen();
  p.setCosmetic(true);
  p.setWidth(10);
  setPen(p);
}

void ROIGraphicsItem::hoverLeaveEvent(QGraphicsSceneHoverEvent*)
{
  prepareGeometryChange();
  QPen p = pen();
  p.setCosmetic(true);
  p.setWidth(1);
  setPen(p);
}

ROICtrlPtGraphicsItem::ROICtrlPtGraphicsItem(ZROI& roi, const ZROIControlPoint& controlPoint, QTransform  tfm,
                                             ZView& view, double viewScale, const RegionNode* regionNode,
                                             QGraphicsItem* parent)
  : QGraphicsRectItem(parent)
  , m_roi(roi)
  , m_controlPoint(controlPoint)
  , m_viewScale(viewScale)
  , m_transform(std::move(tfm))
  , m_view(view)
  , m_regionNode(regionNode)
{
//  if (m_shapeOp.type == ROIType::Polygon || m_shapeOp.type == ROIType::Spline) {
//    setFlags(QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsMovable |
//             QGraphicsItem::ItemIsSelectable);
//  } else {
//    setFlags(QGraphicsItem::ItemSendsGeometryChanges | QGraphicsItem::ItemIsMovable |
//             QGraphicsItem::ItemIsSelectable);
//  }
  setFlags(QGraphicsItem::ItemIsSelectable);

  m_basePos = m_transform.map(m_roi.controlPointCoord(m_controlPoint));
  setPos(m_basePos);
  QString tooltip = QString("Coord:(%1,%2) ").arg(m_basePos.x()).arg(m_basePos.y());
  if (m_regionNode) {
    tooltip += QString("Region: %1 (%2)").arg(m_regionNode->abbreviation).arg(m_regionNode->name);
  }
  setToolTip(tooltip);
  setPen(QPen(QColor(0, 0, 0), 0));
  setBrush(QBrush(QColor(255, 255, 255)));
  setCursor(Qt::PointingHandCursor);
}

void ROICtrlPtGraphicsItem::updateValue()
{
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
  m_basePos = m_transform.map(m_roi.controlPointCoord(m_controlPoint));
  QString tooltip = QString("Coord:(%1,%2) ").arg(m_basePos.x()).arg(m_basePos.y());
  if (m_regionNode) {
    tooltip += QString("Region: %1 (%2)").arg(m_regionNode->abbreviation).arg(m_regionNode->name);
  }
  setToolTip(tooltip);
  setPos(m_basePos);
  setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
}

void ROICtrlPtGraphicsItem::setFixedSize(bool v)
{
  m_fixedSize = v;
  updateRectSize();
}

void ROICtrlPtGraphicsItem::setViewScale(double s)
{
  m_viewScale = s;
  if (m_fixedSize) {
    updateRectSize();
  }
}

void ROICtrlPtGraphicsItem::setLocked(bool l)
{
  m_locked = l;
  if (l) {
    setFlags(QGraphicsItem::GraphicsItemFlags());
  } else {
    setFlags(QGraphicsItem::ItemIsSelectable);
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

//void ROICtrlPtGraphicsItem::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
//{
//  const auto& shapeOp = m_roi.shapeOperations(m_controlPoint.slice, m_controlPoint.shapeID)[m_controlPoint.shapeIndex];
//  if (shapeOp.type == ROIType::Polygon || shapeOp.type == ROIType::Spline) {
//    QMenu menu;
//    QAction* hFlipAction = menu.addAction("Flip Horizontally");
//    QAction* vFlipAction = menu.addAction("Flip Vertically");
//    QAction* selectedAction = menu.exec(event->screenPos());
//    if (selectedAction == hFlipAction) {
//
//    } else if (selectedAction == vFlipAction) {
//
//    }
//    //event->accept();
//  }
//}

QVariant ROICtrlPtGraphicsItem::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value)
{
  if (m_locked) {
    return value;
  }
//  if (change == ItemPositionChange && scene()) {
//    QPointF newPos = value.toPointF() - m_offset;
////    QRectF sceneRect = scene()->sceneRect();
////    if (true) {
////      // Keep the item inside the scene rect.
////      if (m_controlPoint.pos == ZROIControlPoint::Pos::Center) {
////        newPos.setX(qMin(sceneRect.right() - m_shapeOp.rect().width() / 2,
////                         qMax(newPos.x(), sceneRect.left() + m_shapeOp.rect().width() / 2)));
////        newPos.setY(qMin(sceneRect.bottom() - m_shapeOp.rect().height() / 2,
////                         qMax(newPos.y(), sceneRect.top() + m_shapeOp.rect().height() / 2)));
////      } else {
////        newPos.setX(qMin(sceneRect.right(), qMax(newPos.x(), sceneRect.left())));
////        newPos.setY(qMin(sceneRect.bottom(), qMax(newPos.y(), sceneRect.top())));
////      }
////    }
//    return m_roi.setControlPointCoord(m_controlPoint, newPos) + m_offset;
//  } else
  if (change == ItemSelectedHasChanged && scene()) {
    //LOG(INFO) << value.toBool();
    updateRectSize();
    return value;
  }
  return QGraphicsRectItem::itemChange(change, value);
}

void ROICtrlPtGraphicsItem::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
{
  if (m_locked) {
    return;
  }
  if (!isSelected()) {
    return;
  }
  const auto& shapeOps = m_roi.shapeOperations(m_controlPoint.slice, m_controlPoint.shapeID);
  for (const auto& shapeOp : shapeOps) {
    if (shapeOp.type == ROIType::Polygon || shapeOp.type == ROIType::Spline) {
      QMenu menu;
      QAction* subtractNextSelectedShapeAction = nullptr;
      QAction* toRegionAction = nullptr;
      if (m_regionNode) {
        toRegionAction = menu.addAction("Change Region ID...");
      } else {
        subtractNextSelectedShapeAction = menu.addAction("Subtract Next Selected Shape...");
        // toRegionAction = menu.addAction("Add ROI to Region...");
      }
      QAction* selectedAction = menu.exec(event->screenPos());
      if (toRegionAction && selectedAction == toRegionAction) {
        try {
          ZChooseRegionDialog dlg(m_view.currentRegionAnnotationPack().regionAnnotation(), &m_view.graphicsView());
          if (dlg.exec() == QDialog::Accepted) {
            CHECK(m_regionNode);
            m_view.currentRegionAnnotationPack().regionAnnotation().changeROIRegion(m_roi, m_controlPoint.slice,
                                                                                    m_controlPoint.shapeID,
                                                                                    dlg.selectedID());
//            if (m_regionNode) {
//              m_view.currentRegionAnnotationPack().regionAnnotation().changeROIRegion(m_roi, m_controlPoint.slice,
//                                                                                      m_controlPoint.shapeID,
//                                                                                      dlg.selectedID());
//            } else {
//              m_view.currentRegionAnnotationPack().regionAnnotation().mergeROIToRegion(m_roi, m_controlPoint.slice,
//                                                                                       m_controlPoint.shapeID,
//                                                                                       dlg.selectedID());
//            }
          }
        }
        catch (const ZException& e) {
          QMessageBox::critical(QApplication::activeWindow(), qApp->applicationName(),
                                QString("Can not create RegionAnnotation:\n%1").arg(e.what()));
        }
      } else if (subtractNextSelectedShapeAction && selectedAction == subtractNextSelectedShapeAction) {
        m_view.scene().registerROIForSubtraction(&m_roi, m_controlPoint.slice, m_controlPoint.shapeID);
      }
    }
    //event->accept();
    return;
  }
}

//void ROICtrlPtGraphicsItem::mousePressEvent(QGraphicsSceneMouseEvent* event)
//{
//  if (event->button() != Qt::LeftButton) {
//    event->accept();
//    return;
//  }
//  QGraphicsRectItem::mousePressEvent(event);
//}

void ROICtrlPtGraphicsItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event)
{
  if (m_locked) {
    return;
  }
  if (!m_doubleClicked && event->modifiers() != Qt::AltModifier) {
    QGraphicsRectItem::mouseReleaseEvent(event);
  }
  m_doubleClicked = false;
}

void ROICtrlPtGraphicsItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event)
{
  if (m_locked) {
    return;
  }
  if (event->button() == Qt::LeftButton) {
    if (event->modifiers() == Qt::AltModifier) {
      emit m_roi.deselectShape(m_controlPoint.slice, m_controlPoint.shapeID);
    } else {
      m_view.scene().performROISubtraction(&m_roi, m_controlPoint.slice, m_controlPoint.shapeID);
      emit m_roi.selectShape(m_controlPoint.slice, m_controlPoint.shapeID, event->modifiers() == Qt::ControlModifier);
    }
    m_doubleClicked = true;
  } else {
    m_view.scene().removeROIForSubtraction();
    QGraphicsRectItem::mouseDoubleClickEvent(event);
  }
}

void ROICtrlPtGraphicsItem::updateRectSize()
{
  if (m_fixedSize) {
    if (isSelected()) {
      double halfwidth = std::max(1.5, 4.5 / m_viewScale);
      QRectF rect(-halfwidth, -halfwidth, halfwidth * 2, halfwidth * 2);
      setRect(rect);
    } else {
      double halfwidth = std::max(1., 3. / m_viewScale);
      QRectF rect(-halfwidth, -halfwidth, halfwidth * 2, halfwidth * 2);
      setRect(rect);
    }
  } else {
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
}

ZROIFilter::ZROIFilter(ZView& view, const RegionNode* regionNode)
  : ZObjFilter(view)
  , m_showControlPoints("Show Control Points", true)
  , m_fixedControlPointsSize("Fixed Control Points Size", true)
  , m_outlineColor("Outline Color", glm::vec3(1, 1, 0), glm::vec3(0), glm::vec3(1))
  , m_regionColor("Region Color", glm::vec3(.2, .2, .2), glm::vec3(0), glm::vec3(1))
  , m_opacity("Opacity", .5, 0., 1.)
  , m_highlightRegionOnMouseHover("Highlight Region On Mouse Hover", true)
  , m_regionNode(regionNode)
{
  m_outlineColor.setStyle("COLOR");
  m_regionColor.setStyle("COLOR");
  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZROIFilter::visibleChanged);
  connect(&m_showControlPoints, &ZBoolParameter::valueChanged, this, &ZROIFilter::showControlPointsChanged);
  connect(&m_fixedControlPointsSize, &ZBoolParameter::valueChanged, this, &ZROIFilter::fixedControlPointsSizeChanged);
  connect(&m_outlineColor, &ZVec3Parameter::valueChanged, this, &ZROIFilter::outlineColorChanged);
  connect(&m_regionColor, &ZVec3Parameter::valueChanged, this, &ZROIFilter::regionColorChanged);
  connect(&m_opacity, &ZDoubleParameter::valueChanged, this, &ZROIFilter::opacityChanged);
  connect(&m_highlightRegionOnMouseHover, &ZBoolParameter::valueChanged, this, &ZROIFilter::highlightRegionOnMouseHoverChanged);
  addParameter(&m_showControlPoints);
  addParameter(&m_fixedControlPointsSize);
  addParameter(&m_outlineColor);
  addParameter(&m_regionColor);
  m_viewPrecedencePara.blockSignals(true);
  m_viewPrecedencePara.set(getViewPrecedence());
  m_viewPrecedencePara.blockSignals(false);
  addParameter(&m_opacity);
  addParameter(&m_highlightRegionOnMouseHover);

  connect(&view.graphicsView(), &ZGraphicsView::scaleChanged, this, &ZROIFilter::viewScaleChanged);
}

void ZROIFilter::setData(ZROI& roi, ZROIPack& roiPack)
{
  CHECK(m_regionNode == nullptr);
  m_ROI = &roi;
  m_ROIPack = &roiPack;
  m_sliceToROIItem.clear();
  m_sliceToCtrlPtItems.clear();
  if (!m_ROI->isEmpty()) {
    for (const auto& sliceROI : *m_ROI) {
      int slice = sliceROI.first;
      for (auto shapeID :m_ROI->sliceShapeIDs(slice)) {
        createShapeItem(slice, shapeID);
        createCtrlPtItems(slice, shapeID);
      }
    }
  }

  connect(m_ROI, &ZROI::boundBoxChanged, this, &ZROIFilter::boundBoxChanged);
  connect(m_ROI, &ZROI::roiChanged, this, &ZROIFilter::onRoiChanged);
  connect(m_ROI, &ZROI::roiMoved, this, &ZROIFilter::onRoiMoved);
  connect(m_ROI, &ZROI::roiDeleted, this, &ZROIFilter::onRoiDeleted);
  connect(m_ROI, &ZROI::selectShape, this, &ZROIFilter::selectCtrlPtItems);
  connect(m_ROI, &ZROI::deselectShape, this, &ZROIFilter::deselectCtrlPtItems);

  connect(m_ROIPack, &ZROIPack::lockedStateChanged, this, &ZROIFilter::onLockedStateChanged);
}

void ZROIFilter::setData(ZROI& roi, ZRegionAnnotationPack& raPack)
{
  CHECK(m_regionNode);
  m_ROI = &roi;
  m_regionAnnotationPack = &raPack;
  m_sliceToROIItem.clear();
  m_sliceToCtrlPtItems.clear();
  if (!m_ROI->isEmpty()) {
    for (const auto& sliceROI : *m_ROI) {
      int slice = sliceROI.first;
      for (auto shapeID :m_ROI->sliceShapeIDs(slice)) {
        createShapeItem(slice, shapeID);
        createCtrlPtItems(slice, shapeID);
      }
    }
  }

  connect(m_ROI, &ZROI::boundBoxChanged, this, &ZROIFilter::boundBoxChanged);
  connect(m_ROI, &ZROI::roiChanged, this, &ZROIFilter::onRoiChanged);
  connect(m_ROI, &ZROI::roiMoved, this, &ZROIFilter::onRoiMoved);
  connect(m_ROI, &ZROI::roiDeleted, this, &ZROIFilter::onRoiDeleted);
  connect(m_ROI, &ZROI::selectShape, this, &ZROIFilter::selectCtrlPtItems);
  connect(m_ROI, &ZROI::deselectShape, this, &ZROIFilter::deselectCtrlPtItems);

  connect(m_regionAnnotationPack, &ZRegionAnnotationPack::lockedStateChanged,
          this, &ZROIFilter::onLockedStateChanged);
}

bool ZROIFilter::isLocked() const
{
  return (m_ROIPack && m_ROIPack->isLocked()) || (m_regionAnnotationPack && m_regionAnnotationPack->isLocked());
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
  if (!m_visible.get()) {
    return;
  }
  int rz = realZ(z);

  m_sliceToROIItem.clear();
  m_sliceToCtrlPtItems.clear();
  size_t slice = rz;
  if (m_ROI && m_ROI->hasSlice(slice)) {
    for (auto shapeID : m_ROI->sliceShapeIDs(slice)) {
      createShapeItem(slice, shapeID);
      createCtrlPtItems(slice, shapeID);
    }
  }
}

void ZROIFilter::setMaxZProjView(int /*t*/)
{
  if (!m_visible.get()) {
    return;
  }

  m_sliceToROIItem.clear();
  m_sliceToCtrlPtItems.clear();
  if (m_ROI) {
    for (const auto& sliceROI : *m_ROI) {
      int slice = sliceROI.first;
      for (auto shapeID : m_ROI->sliceShapeIDs(slice)) {
        createShapeItem(slice, shapeID);
        createCtrlPtItems(slice, shapeID);
      }
    }
  }
}

QString ZROIFilter::regionName() const
{
  return m_regionNode ? m_regionNode->name : QString();
}

ZBBox<glm::ivec4> ZROIFilter::boundBox() const
{
  if (m_ROI) {
    ZBBox<glm::ivec4> res = m_ROI->boundBox();
    updateBoundBoxWithOffsetPara(res);
    return res;
  }
  return ZBBox<glm::ivec4>();
}

std::shared_ptr<ZWidgetsGroup> ZROIFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("ROI", 1);
    m_widgetsGroup->addChild(m_visible, 1);

    auto pb = new QPushButton("Bring to Front");
    connect(pb, &QPushButton::clicked, this, &ZROIFilter::bringToFront);
    m_widgetsGroup->addChild(*pb, 1);

    pb = new QPushButton("Send to Back");
    connect(pb, &QPushButton::clicked, this, &ZROIFilter::sendToBack);
    m_widgetsGroup->addChild(*pb, 1);

    m_widgetsGroup->addChild(m_viewPrecedencePara, 1);
    m_widgetsGroup->addChild(m_showControlPoints, 1);
    m_widgetsGroup->addChild(m_fixedControlPointsSize, 1);
    m_widgetsGroup->addChild(m_outlineColor, 1);
    m_widgetsGroup->addChild(m_regionColor, 1);
    m_widgetsGroup->addChild(m_transform, 1);
    m_widgetsGroup->addChild(m_offsetPara, 1);
    m_widgetsGroup->addChild(m_opacity, 1);
    m_widgetsGroup->addChild(m_highlightRegionOnMouseHover, 1);
  }
  return m_widgetsGroup;
}

std::shared_ptr<ZWidgetsGroup> ZROIFilter::viewSettingWidgetsGroupForAnnotationFilter()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    //m_widgetsGroup->addChild(m_showControlPoints, 1);
    //m_widgetsGroup->addChild(m_fixedControlPointsSize, 1);
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

  if (isLocked()) {
    return;
  }

  if (!m_ROI) {
    return;
  }

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
  if (!controlPoints.empty()) {
    m_ROI->deleteROIControlPoints(controlPoints);
  }
}

void ZROIFilter::copyKeyPressed()
{
  if (isLocked()) {
    return;
  }

  if (!m_ROI) {
    return;
  }

  m_ROI->clearCopy();

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
  if (!controlPoints.empty()) {
    m_ROI->copyROIFromControlPoints(controlPoints);
  }
}

void ZROIFilter::pasteKeyPressed(int slice, QPointF point, bool hFlip, bool vFlip)
{
  if (isLocked()) {
    return;
  }

  if (!m_ROI) {
    return;
  }

  m_ROI->pasteROIToCoord(slice, point, m_ROI->copiedItemBoundBox(), hFlip, vFlip);
}

void ZROIFilter::pasteKeyPressed(int slice, QPointF point, const nim::ZBBox<glm::ivec4>& srcBoundBox, bool hFlip,
                                 bool vFlip)
{
  if (isLocked()) {
    return;
  }

  if (!m_ROI) {
    return;
  }

  m_ROI->pasteROIToCoord(slice, point, srcBoundBox, hFlip, vFlip);
}

void ZROIFilter::mousePressed(const QPointF& scenePos)
{
  if (isLocked()) {
    return;
  }

  if (!m_ROI) {
    return;
  }

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
        if (m_hasSelectedItems) {
          break;
        }
      }
      if (m_hasSelectedItems) {
        break;
      }
    }
  } else if (m_sliceToCtrlPtItems.find(realZ()) != m_sliceToCtrlPtItems.end()) {
    for (auto&[id, ctrlItems] : m_sliceToCtrlPtItems.at(realZ())) {
      for (auto& item : ctrlItems) {
        if (item->isSelected()) {
          m_hasSelectedItems = true;
          break;
        }
      }
      if (m_hasSelectedItems) {
        break;
      }
    }
  }
  if (m_hasSelectedItems) {
    m_ROI->startMoveSelectedControlPointsCommand();
    m_startPoint = scenePos;
  }
}

void ZROIFilter::mouseMoved(const QPointF& scenePos)
{
  if (isLocked()) {
    return;
  }

  if (!m_ROI) {
    return;
  }

  auto trans = getQTransform();
  if (m_hasSelectedItems && scenePos != m_startPoint) {
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
    if (!controlPoints.empty()) {
      if (trans.isIdentity()) {
        m_ROI->shiftControlPointsCoords(controlPoints, scenePos - m_startPoint);
      } else {
        auto itrans = trans.inverted();
        m_ROI->shiftControlPointsCoords(controlPoints, itrans.map(scenePos) - itrans.map(m_startPoint));
      }
      m_startPoint = scenePos;
    }
  }
}

void ZROIFilter::mouseReleased(const QPointF& /*scenePos*/)
{
  if (isLocked()) {
    return;
  }

  if (!m_ROI) {
    return;
  }

  if (m_hasSelectedItems) {
    m_ROI->endMoveSelectedControlPointsCommand();
    m_hasSelectedItems = false;
  }
}

void ZROIFilter::rotateClockwise(double x, double y)
{
  if (isLocked()) {
    return;
  }

  if (!m_ROI) {
    return;
  }

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
  if (!controlPoints.empty()) {
    m_ROI->rotateROIControlPoints(controlPoints, 1. * degree, x, y);
  }
}

void ZROIFilter::rotateCounterclockwise(double x, double y)
{
  if (isLocked()) {
    return;
  }

  if (!m_ROI) {
    return;
  }

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
  if (!controlPoints.empty()) {
    m_ROI->rotateROIControlPoints(controlPoints, -1. * degree, x, y);
  }
}

void ZROIFilter::viewPrecedenceChanged()
{
  if (!m_ROI) {
    return;
  }

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
  if (!m_ROI) {
    return;
  }

  auto trans = getQTransform();
  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      shapeItem->setTransform(trans);
    }
  }
  if (m_showControlPoints.get()) {
    for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
      for (auto&[id, ctrlItems] : sliceItem) {
        for (auto& item : ctrlItems) {
          item->setTransform_(trans);
        }
      }
    }
  }

//  m_sliceToROIItem.clear();
//  m_sliceToCtrlPtItems.clear();
//  if (!m_ROI->isEmpty()) {
//    for (const auto& sliceROI : *m_ROI) {
//      int slice = sliceROI.first;
//      for (auto shapeID : m_ROI->sliceShapeIDs(slice)) {
//        createShapeItem(slice, shapeID);
//        createCtrlPtItems(slice, shapeID);
//      }
//    }
//  }

//  QTransform trans = getQTransform();
//  QList<QGraphicsItem*> items;
//  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
//    for (auto&[id, shapeItem] : sliceItem) {
//      shapeItem->resetTransform();
//      items.push_back(shapeItem.get());
//    }
//  }
//  if (m_showControlPoints.get()) {
//    for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
//      for (auto&[id, ctrlItems] : sliceItem) {
//        for (auto& item : ctrlItems) {
//          item->resetTransform();
//          items.push_back(item.get());
//        }
//      }
//    }
//  }
//
//  QGraphicsItemGroup* gr = m_view.scene().createItemGroup(items);
//  gr->setTransform(trans);
//  m_view.scene().destroyItemGroup(gr);

  ZObjFilter::transformChanged();
}

void ZROIFilter::offsetChanged()
{
  if (!m_visible.get()) {
    return;
  }

  m_sliceToROIItem.clear();
  m_sliceToCtrlPtItems.clear();
  if (m_ROI) {
    for (const auto& sliceROI : *m_ROI) {
      int slice = sliceROI.first;
      for (auto shapeID : m_ROI->sliceShapeIDs(slice)) {
        createShapeItem(slice, shapeID);
        createCtrlPtItems(slice, shapeID);
      }
    }
  }
  ZObjFilter::offsetChanged();
}

void ZROIFilter::createShapeItem(int slice, size_t shapeID)
{
  if (!((realZ() == slice || m_view.isMaxZProjView()) && m_visible.get())) {
    return;
  }

  QTransform trans = getQTransform();
  auto roiItem = new ROIGraphicsItem(*m_ROI, slice, shapeID, m_view, m_regionNode);
  roiItem->setZValue(m_viewPrecedencePara.get());
  QPen pen(QColor(m_outlineColor.get().x * 255,
                  m_outlineColor.get().y * 255,
                  m_outlineColor.get().z * 255),
           1);
  pen.setCosmetic(true);
  roiItem->setPen(pen);
  roiItem->setBrush_(QColor(m_regionColor.get().x * 255,
                            m_regionColor.get().y * 255,
                            m_regionColor.get().z * 255,
                            m_opacity.get() * 255));
  //roiItem->setOpacity(m_opacity.get());
  roiItem->setVisible((realZ() == slice || m_view.isMaxZProjView()) && m_visible.get());
  m_view.scene().addItem(roiItem);
  roiItem->setTransformOriginPoint(roiItem->mapFromScene(QPointF(0, 0)));
  roiItem->setTransform(trans);
  roiItem->setHighlightOnHover(m_highlightRegionOnMouseHover.get());
  roiItem->setLocked(isLocked());

//  QList<QGraphicsItem*> items{roiItem};
//  QGraphicsItemGroup* gr = m_view.scene().createItemGroup(items);
//  gr->setTransform(trans);
//  m_view.scene().destroyItemGroup(gr);

  m_sliceToROIItem[slice][shapeID] = std::unique_ptr<ROIGraphicsItem>(roiItem);
}

void ZROIFilter::createCtrlPtItems(int slice, size_t shapeID)
{
  if (!((realZ() == slice || m_view.isMaxZProjView()) && m_visible.get() && m_showControlPoints.get())) {
    return;
  }

  //QList<QGraphicsItem*> items_all;

  std::vector<std::unique_ptr<ROICtrlPtGraphicsItem>> items;
  std::vector<ZROIControlPoint> controlPoints = m_ROI->sliceControlPoints(slice, shapeID);
  QTransform trans = getQTransform();
  for (const auto& controlPoint : controlPoints) {
    auto rectItem = new ROICtrlPtGraphicsItem(*m_ROI, controlPoint, trans, m_view,
                                              m_view.graphicsView().currentScale(), m_regionNode);
    int zValue = m_ROI->shapeOperations(slice, shapeID)[controlPoint.shapeIndex].isAdd ? m_viewPrecedencePara.get() + 1
                                                                                       : m_viewPrecedencePara.get();
    rectItem->setZValue(zValue);
    rectItem->setVisible((realZ() == slice || m_view.isMaxZProjView()) && m_visible.get() && m_showControlPoints.get());
    rectItem->setFixedSize(m_fixedControlPointsSize.get());
    rectItem->setLocked(isLocked());
    m_view.scene().addItem(rectItem);
    items.emplace_back(rectItem);
    //items_all.push_front(rectItem);
  }

//  QGraphicsItemGroup* gr = m_view.scene().createItemGroup(items_all);
//  gr->setTransform(trans);
//  m_view.scene().destroyItemGroup(gr);

  m_sliceToCtrlPtItems[slice][shapeID].swap(items);
}

void ZROIFilter::selectCtrlPtItems(int slice, size_t shapeID, bool append)
{
  if (isLocked()) {
    return;
  }

  if (!m_ROI) {
    return;
  }
  //LOG(INFO) << slice << " " << shapeID << " " << m_view.scene().selectedItems().size();
  if (!append) {
    for (auto& item : m_view.scene().selectedItems()) {
      item->setSelected(false);
    }
  }
  for (auto& item : m_sliceToCtrlPtItems[slice][shapeID]) {
    item->setSelected(true);
  }
  //LOG(INFO) << slice << " " << shapeID << " " << m_view.scene().selectedItems().size();
}

void ZROIFilter::deselectCtrlPtItems(int slice, size_t shapeID)
{
  if (isLocked()) {
    return;
  }

  if (!m_ROI) {
    return;
  }
  for (auto& item : m_sliceToCtrlPtItems[slice][shapeID]) {
    item->setSelected(false);
  }
}

void ZROIFilter::visibleChanged()
{
  if (!m_ROI) {
    return;
  }
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
  if (!m_ROI) {
    return;
  }
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

void ZROIFilter::fixedControlPointsSizeChanged()
{
  if (!m_ROI) {
    return;
  }
  auto s = m_view.graphicsView().currentScale();
  for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
    for (auto&[id, ctrlItems] : sliceItem) {
      for (auto& item : ctrlItems) {
        if (m_fixedControlPointsSize.get()) {
          item->setViewScale(s);
        }
        item->setFixedSize(m_fixedControlPointsSize.get());
      }
    }
  }
}

void ZROIFilter::outlineColorChanged()
{
  if (!m_ROI) {
    return;
  }
  QPen pen(QColor(m_outlineColor.get().x * 255,
                  m_outlineColor.get().y * 255,
                  m_outlineColor.get().z * 255),
           1);
  pen.setCosmetic(true);
  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      shapeItem->setPen(pen);
    }
  }
}

void ZROIFilter::regionColorChanged()
{
  if (!m_ROI) {
    return;
  }
  QBrush brush(QColor(m_regionColor.get().x * 255,
                      m_regionColor.get().y * 255,
                      m_regionColor.get().z * 255,
                      m_opacity.get() * 255));
  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      shapeItem->setBrush_(brush);
    }
  }
}

void ZROIFilter::opacityChanged()
{
  if (!m_ROI) {
    return;
  }
  QBrush brush(QColor(m_regionColor.get().x * 255,
                      m_regionColor.get().y * 255,
                      m_regionColor.get().z * 255,
                      m_opacity.get() * 255));
  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      shapeItem->setBrush(brush);
    }
  }
}

void ZROIFilter::highlightRegionOnMouseHoverChanged()
{
  if (!m_ROI) {
    return;
  }
  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      shapeItem->setHighlightOnHover(m_highlightRegionOnMouseHover.get());
    }
  }
}

void ZROIFilter::onRoiChanged(int slice, const std::set<size_t>& newShapes,
                              const std::set<size_t>& deletedShapes,
                              const std::set<size_t>& changedShapes)
{
  if (!m_ROI) {
    return;
  }
  if (newShapes.empty() && deletedShapes.empty() && changedShapes.empty()) {
    m_sliceToCtrlPtItems.erase(slice);
    m_sliceToROIItem.erase(slice);
    if (m_ROI->hasSlice(slice)) {
      for (auto shapeID : m_ROI->sliceShapeIDs(slice)) {
        createShapeItem(slice, shapeID);
        createCtrlPtItems(slice, shapeID);
      }
    }
  } else {
    if (!deletedShapes.empty()) {
      for (auto shapeID : deletedShapes) {
        m_sliceToROIItem[slice].erase(shapeID);
        m_sliceToCtrlPtItems[slice].erase(shapeID);
      }
    }
    if (!changedShapes.empty()) {
      for (auto shapeID : changedShapes) {
        if (m_sliceToROIItem.find(slice) == m_sliceToROIItem.end() ||
            m_sliceToROIItem[slice].find(shapeID) == m_sliceToROIItem[slice].end()) {
          continue;
        }
        m_sliceToROIItem[slice][shapeID]->updateValue();
        m_sliceToCtrlPtItems[slice].erase(shapeID);
        createCtrlPtItems(slice, shapeID);
      }
    }
    if (!newShapes.empty()) {
      for (auto shapeID : newShapes) {
        createShapeItem(slice, shapeID);
        createCtrlPtItems(slice, shapeID);
      }
    }
  }
}

void ZROIFilter::onRoiMoved(int slice, const std::set<size_t>& changedShapes)
{
  if (!m_ROI) {
    return;
  }
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
  if (!m_ROI) {
    return;
  }
  m_sliceToROIItem.erase(slice);
  m_sliceToCtrlPtItems.erase(slice);
}

void ZROIFilter::viewScaleChanged(double s)
{
  if (!m_ROI) {
    return;
  }
  if (m_fixedControlPointsSize.get()) {
    for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
      for (auto&[id, ctrlItems] : sliceItem) {
        for (auto& item : ctrlItems) {
          item->setViewScale(s);
        }
      }
    }
  }
}

void ZROIFilter::onLockedStateChanged(bool l)
{
  CHECK(isLocked() == l);
  for (auto&[slice, sliceItem] : m_sliceToROIItem) {
    for (auto&[id, shapeItem] : sliceItem) {
      if (l) {
        shapeItem->setSelected(false);
      }
      shapeItem->setLocked(l);
    }
  }
  for (auto&[slice, sliceItem] : m_sliceToCtrlPtItems) {
    for (auto&[id, ctrlItems] : sliceItem) {
      for (auto& item : ctrlItems) {
        if (l) {
          item->setSelected(false);
        }
        item->setLocked(l);
      }
    }
  }
}

} // namespace nim
