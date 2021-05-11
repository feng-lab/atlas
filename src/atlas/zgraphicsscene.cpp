#include "zgraphicsscene.h"

#include "zview.h"
#include "zlog.h"
#include "zroi.h"
#include "zroifilter.h"
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsRectItem>
#include <QApplication>
#include <QMenu>

namespace nim {

ZGraphicsScene::ZGraphicsScene(ZView* view)
  : QGraphicsScene(view)
  , m_view(view)
  , m_lastPressedPt(0, 0)
{
  setSceneRect(QRectF(0, 0, 200, 200));
  setItemIndexMethod(NoIndex);
}

void ZGraphicsScene::registerROIForSubtraction(ZROI* roi, int slice, size_t shapeID)
{
  m_roi = roi;
  m_slice = slice;
  m_shapeID = shapeID;
}

void ZGraphicsScene::removeROIForSubtraction()
{
  m_roi = nullptr;
  m_slice = -1;
  m_shapeID = 0;
}

void ZGraphicsScene::performROISubtraction(const ZROI* roi, int slice, size_t shapeID)
{
  if (m_roi && m_slice >= 0 &&
      !(m_roi == roi && m_slice == slice && m_shapeID == shapeID)) {
    m_roi->sliceSubtractShape(m_slice, m_shapeID, roi->shapeOperations(slice, shapeID));
  }
  m_roi = nullptr;
  m_slice = -1;
  m_shapeID = 0;
}

void ZGraphicsScene::escKeyPressed()
{
  if (m_splineItem) {
    //LOG(INFO) << m_ctrlPtsItem.size() << " " << m_spline.size();
    if (!m_ctrlPtsItem.empty()) {
      m_ctrlPtsItem.pop_back();
      std::swap(m_spline[m_spline.size() - 1], m_spline[m_spline.size() - 2]);
      m_spline.pop_back();
      m_splineItem->setPath(ZROI::splineToPainterPath(m_spline));
    } else {
      m_startPtItem.reset();
      m_ctrlPtsItem.clear();
      m_splineItem.reset();
    }
  }
  if (m_polygonItem) {
    //LOG(INFO) << m_ctrlPtsItem.size() << " " << m_spline.size();
    if (!m_ctrlPtsItem.empty()) {
      m_ctrlPtsItem.pop_back();
      std::swap(m_polygon[m_polygon.size() - 1], m_polygon[m_polygon.size() - 2]);
      m_polygon.pop_back();
      QPainterPath path;
      path.addPolygon(m_polygon);
      m_polygonItem->setPath(path);
    } else {
      m_startPtItem.reset();
      m_ctrlPtsItem.clear();
      m_polygonItem.reset();
    }
  }
}

void ZGraphicsScene::contextMenuEvent(QGraphicsSceneContextMenuEvent* contextMenuEvent)
{
  if (m_splineItem || m_polygonItem || m_rectItem || m_ellipseItem) {
    return;
  }

  QGraphicsScene::contextMenuEvent(contextMenuEvent);
  if (!contextMenuEvent->isAccepted()) {
    removeROIForSubtraction();
    QMenu menu;
    QAction* pasteAction = menu.addAction("Paste Here");
    QAction* pasteHFlipAction = menu.addAction("Paste Horizonally Flipped");
    QAction* pasteVFlipAction = menu.addAction("Paste Vertically Flipped");
    QAction* selectedAction = menu.exec(contextMenuEvent->screenPos());
    if (selectedAction == pasteAction) {
      m_view->pasteHere(m_view->currentSlice(), contextMenuEvent->scenePos());
    } else if (selectedAction == pasteHFlipAction) {
      m_view->pasteHere(m_view->currentSlice(), contextMenuEvent->scenePos(), true);
    } else if (selectedAction == pasteVFlipAction) {
      m_view->pasteHere(m_view->currentSlice(), contextMenuEvent->scenePos(), false, true);
    }
  }
}

void ZGraphicsScene::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
  if (event->button() != Qt::LeftButton) {
    event->accept();
    return;
  }
  m_lastPressedPt = event->scenePos();

  QGraphicsItem* item = itemAt(event->scenePos(), QTransform());
  auto roiItem = qgraphicsitem_cast<ROIGraphicsItem*>(item);
  auto roiCtrlPtItem = qgraphicsitem_cast<ROICtrlPtGraphicsItem*>(item);
  bool canUpdateROI = (!roiItem && !roiCtrlPtItem)
                      || event->modifiers() == Qt::ControlModifier
                      || event->modifiers() == Qt::AltModifier
                      || m_view->isRegionAnnotationMode();

  CHECK(!m_rectItem);
  CHECK(!m_ellipseItem);

  QPointF startPolyHalfWidth(5.0 / m_view->currentScale(), 5.0 / m_view->currentScale());
  QPointF ctrlPtHalfWidth(4.0 / m_view->currentScale(), 4.0 / m_view->currentScale());

  if ((m_splineItem || m_view->state() == ZView::State::ROISpline || m_view->state() == ZView::State::ROICut) && canUpdateROI) {
    QPointF scenePt = event->scenePos();
    if (event->button() == Qt::LeftButton) {
      if (!m_splineItem) {  //new
        CHECK(!m_startPtItem);
        CHECK(m_ctrlPtsItem.empty());
        m_startScenePt = scenePt;
        switch (event->modifiers()) {
          case Qt::ControlModifier :
            m_roiAction = ROIAction::Add;
            break;
          case Qt::AltModifier:
            m_roiAction = ROIAction::Subtract;
            break;
          default:
            m_roiAction = ROIAction::New;
            break;
        }
//        if (m_roiAction == ROIAction::New) {
//          std::pair<int, int> sliceRange = m_view->currentSliceRange();
//          for (auto i = sliceRange.first; i < sliceRange.second; ++i)
//            m_view->roi().deleteSliceROI(i);
//        }
        QPolygonF startPoly = QRectF(scenePt - startPolyHalfWidth, scenePt + startPolyHalfWidth);
        m_startPtItem = std::make_unique<QGraphicsPolygonItem>(startPoly);
        m_startPtItem->setZValue(m_zValue);
        m_startPtItem->setPen(QPen(QColor(0, 0, 0), 0));
        m_startPtItem->setBrush(QBrush(QColor(255, 255, 255, 128)));
        addItem(m_startPtItem.get());
        m_spline.clear();
        m_spline << m_startScenePt << m_startScenePt;  // pt and next potential pt
        m_splineItem = std::make_unique<QGraphicsPathItem>(ZROI::splineToPainterPath(m_spline));
        m_splineItem->setZValue(m_zValue);
        m_splineItem->setPen(QPen(QColor(255, 255, 0), 0));
        addItem(m_splineItem.get());
      } else {
        if (m_startPtItem->contains(m_spline.last()) && m_view->state() == ZView::State::ROISpline) {  // finish here
          m_spline.last() = m_spline[0];
          if (m_spline.size() > 3) {
            std::pair<int, int> sliceRange = m_view->currentSliceRange();
            if (m_view->isROIMode()) {
              for (auto i = sliceRange.first; i < sliceRange.second; ++i) {
                if (m_roiAction == ROIAction::New) {
                  m_view->currentROIPack().roi().newSpline(i, m_spline);
                } else if (m_roiAction == ROIAction::Add) {
                  m_view->currentROIPack().roi().addSpline(i, m_spline);
                } else if (m_roiAction == ROIAction::Subtract) {
                  m_view->currentROIPack().roi().subtractSpline(i, m_spline);
                }
              }
            } else {
              CHECK(m_view->isRegionAnnotationMode());
              ZROI roi;
              for (auto i = sliceRange.first; i < sliceRange.second; ++i) {
                roi.newSpline(i, m_spline);
              }
              m_view->currentRegionAnnotationPack().mergeROIToCurrentRegion(roi);
            }
          }
          m_startPtItem.reset();
          m_ctrlPtsItem.clear();
          m_splineItem.reset();
        } else if (!isScenePtOverlap(m_spline.last(), m_spline[m_spline.size() - 2])) {
          QPolygonF ctrlPoly = QRectF(scenePt - ctrlPtHalfWidth, scenePt + ctrlPtHalfWidth);
          auto ctrlPtItem = std::make_unique<QGraphicsPolygonItem>(ctrlPoly);
          ctrlPtItem->setZValue(m_zValue);
          ctrlPtItem->setPen(QPen(QColor(0, 0, 0), 0));
          ctrlPtItem->setBrush(QBrush(QColor(255, 255, 0, 128)));
          addItem(ctrlPtItem.get());
          m_ctrlPtsItem.push_back(std::move(ctrlPtItem));

          m_spline << scenePt;   // add next potential pt
          m_splineItem->setPath(ZROI::splineToPainterPath(m_spline));
        }
      }
    }
  } else if ((m_polygonItem || m_view->state() == ZView::State::ROIPolygon) && canUpdateROI) {
    QPointF scenePt = event->scenePos();
    if (event->button() == Qt::LeftButton) {
      if (!m_polygonItem) {  //new
        CHECK(!m_startPtItem);
        CHECK(m_ctrlPtsItem.empty());
        m_startScenePt = scenePt;
        switch (event->modifiers()) {
          case Qt::ControlModifier :
            m_roiAction = ROIAction::Add;
            break;
          case Qt::AltModifier:
            m_roiAction = ROIAction::Subtract;
            break;
          default:
            m_roiAction = ROIAction::New;
            break;
        }
//        if (m_roiAction == ROIAction::New) {
//          std::pair<int, int> sliceRange = m_view->currentSliceRange();
//          for (auto i = sliceRange.first; i < sliceRange.second; ++i)
//            m_view->roi().deleteSliceROI(i);
//        }
        QPolygonF startPoly = QRectF(scenePt - startPolyHalfWidth, scenePt + startPolyHalfWidth);
        m_startPtItem = std::make_unique<QGraphicsPolygonItem>(startPoly);
        m_startPtItem->setZValue(m_zValue);
        m_startPtItem->setPen(QPen(QColor(0, 0, 0), 0));
        m_startPtItem->setBrush(QBrush(QColor(255, 255, 255, 128)));
        addItem(m_startPtItem.get());
        m_polygon.clear();
        m_polygon << m_startScenePt << m_startScenePt;  // pt and next potential pt
        LOG(INFO) << "";
        LOG(INFO) << "first: " << m_startScenePt;
        QPainterPath path;
        path.addPolygon(m_polygon);
        m_polygonItem = std::make_unique<QGraphicsPathItem>(path);
        m_polygonItem->setZValue(m_zValue);
        m_polygonItem->setPen(QPen(QColor(255, 255, 0), 0));
        addItem(m_polygonItem.get());
      } else {
        if (m_startPtItem->contains(m_polygon.last())) {  // finish here
          m_polygon.last() = m_polygon[0];
          if (m_polygon.size() > 3) {
            std::pair<int, int> sliceRange = m_view->currentSliceRange();
            if (m_view->isROIMode()) {
              for (auto i = sliceRange.first; i < sliceRange.second; ++i) {
                if (m_roiAction == ROIAction::New) {
                  m_view->currentROIPack().roi().newPolygon(i, m_polygon);
                } else if (m_roiAction == ROIAction::Add) {
                  m_view->currentROIPack().roi().addPolygon(i, m_polygon);
                } else if (m_roiAction == ROIAction::Subtract) {
                  m_view->currentROIPack().roi().subtractPolygon(i, m_polygon);
                }
              }
            } else {
              CHECK(m_view->isRegionAnnotationMode());
              ZROI roi;
              for (auto i = sliceRange.first; i < sliceRange.second; ++i) {
                roi.newPolygon(i, m_polygon);
              }
              m_view->currentRegionAnnotationPack().mergeROIToCurrentRegion(roi);
            }
          }
          m_startPtItem.reset();
          m_ctrlPtsItem.clear();
          m_polygonItem.reset();
        } else if (!isScenePtOverlap(m_polygon.last(), m_polygon[m_polygon.size() - 2])) {
          if (m_polygon.size() == 2) {
            LOG(INFO) << "second: " << scenePt;
            double xdiff1 = m_polygon[0].x() - m_polygon[1].x();
            double ydiff1 = m_polygon[0].y() - m_polygon[1].y();
            LOG(INFO) << "soma Length: " << std::sqrt(xdiff1 * xdiff1 + ydiff1 * ydiff1);
          } else if (m_polygon.size() == 3) {
            //LOG(INFO) << "third: " << scenePt;
            //            double xdiff1 = m_polygon[0].x() - m_polygon[1].x();
            //            double ydiff1 = m_polygon[0].y() - m_polygon[1].y();
            //            double xdiff2 = m_polygon[2].x() - m_polygon[1].x();
            //            double ydiff2 = m_polygon[2].y() - m_polygon[1].y();
            //            double len1 = std::sqrt(xdiff1*xdiff1 + ydiff1*ydiff1);
            //            double len2 = std::sqrt(xdiff2*xdiff2 + ydiff2*ydiff2);

            //            LOG(INFO) << "length1: " << len1
            //                    << "length2: " << len2
            //                    << "ratio: " << (len1 / (len1 + len2));
          } else if (m_polygon.size() == 4) {
            double xdiff1 = m_polygon[0].x() - m_polygon[1].x();
            double ydiff1 = m_polygon[0].y() - m_polygon[1].y();
            double xdiff2 = m_polygon[2].x() - m_polygon[1].x();
            double ydiff2 = m_polygon[2].y() - m_polygon[1].y();
            double xdiff3 = m_polygon[2].x() - m_polygon[3].x();
            double ydiff3 = m_polygon[2].y() - m_polygon[3].y();
            double len1 = std::sqrt(xdiff1 * xdiff1 + ydiff1 * ydiff1);
            double len2 = std::sqrt(xdiff2 * xdiff2 + ydiff2 * ydiff2);
            double len3 = std::sqrt(xdiff3 * xdiff3 + ydiff3 * ydiff3);
            LOG(INFO) << len1 << " " << len2 << " " << len3;
          }

          QPolygonF ctrlPoly = QRectF(scenePt - ctrlPtHalfWidth, scenePt + ctrlPtHalfWidth);
          auto ctrlPtItem = std::make_unique<QGraphicsPolygonItem>(ctrlPoly);
          ctrlPtItem->setZValue(m_zValue);
          ctrlPtItem->setPen(QPen(QColor(0, 0, 0), 0));
          ctrlPtItem->setBrush(QBrush(QColor(255, 255, 0, 128)));
          addItem(ctrlPtItem.get());
          m_ctrlPtsItem.push_back(std::move(ctrlPtItem));

          m_polygon << scenePt;   // add next potential pt
          QPainterPath path;
          path.addPolygon(m_polygon);
          m_polygonItem->setPath(path);
        }
      }
    }
  } else if (m_view->state() == ZView::State::ROIRect && event->button() == Qt::LeftButton && canUpdateROI) {
    QPointF scenePt = event->scenePos();
    CHECK(!m_startPtItem);
    m_startScenePt = scenePt;
    switch (event->modifiers()) {
      case Qt::ControlModifier :
        m_roiAction = ROIAction::Add;
        break;
      case Qt::AltModifier:
        m_roiAction = ROIAction::Subtract;
        break;
      default:
        m_roiAction = ROIAction::New;
        break;
    }
//    if (m_roiAction == ROIAction::New) {
//      std::pair<int, int> sliceRange = m_view->currentSliceRange();
//      for (auto i = sliceRange.first; i < sliceRange.second; ++i)
//        m_view->roi().deleteSliceROI(i);
//    }
    QPolygonF startPoly = QRectF(scenePt - startPolyHalfWidth, scenePt + startPolyHalfWidth);
    m_startPtItem = std::make_unique<QGraphicsPolygonItem>(startPoly);
    m_startPtItem->setZValue(m_zValue);
    m_startPtItem->setPen(QPen(QColor(0, 0, 0), 0));
    m_startPtItem->setBrush(QBrush(QColor(255, 255, 255, 128)));
    addItem(m_startPtItem.get());
    m_rectItem = std::make_unique<QGraphicsRectItem>(QRectF(scenePt, QSizeF(0, 0)));
    m_rectItem->setZValue(m_zValue);
    m_rectItem->setPen(QPen(QColor(255, 255, 0), 0));
    addItem(m_rectItem.get());
  } else if (m_view->state() == ZView::State::ROIEllipse && event->button() == Qt::LeftButton && canUpdateROI) {
    QPointF scenePt = event->scenePos();
    CHECK(!m_startPtItem);
    m_startScenePt = scenePt;
    switch (event->modifiers()) {
      case Qt::ControlModifier :
        m_roiAction = ROIAction::Add;
        break;
      case Qt::AltModifier:
        m_roiAction = ROIAction::Subtract;
        break;
      default:
        m_roiAction = ROIAction::New;
        break;
    }
//    if (m_roiAction == ROIAction::New) {
//      std::pair<int, int> sliceRange = m_view->currentSliceRange();
//      for (auto i = sliceRange.first; i < sliceRange.second; ++i)
//        m_view->roi().deleteSliceROI(i);
//    }
    QPolygonF startPoly = QRectF(scenePt - startPolyHalfWidth, scenePt + startPolyHalfWidth);
    m_startPtItem = std::make_unique<QGraphicsPolygonItem>(startPoly);
    m_startPtItem->setZValue(m_zValue);
    m_startPtItem->setPen(QPen(QColor(0, 0, 0), 0));
    m_startPtItem->setBrush(QBrush(QColor(255, 255, 255, 128)));
    addItem(m_startPtItem.get());
    m_ellipseItem = std::make_unique<QGraphicsEllipseItem>(QRectF(m_startScenePt, QSizeF(0, 0)));
    m_ellipseItem->setZValue(m_zValue);
    m_ellipseItem->setPen(QPen(QColor(255, 255, 0), 0));
    addItem(m_ellipseItem.get());
  } else {
    QGraphicsScene::mousePressEvent(event);
    if (!selectedItems().empty() && event->button() == Qt::LeftButton) {
      Q_EMIT mousePressed(event->scenePos(), event->modifiers());
    }
  }
}

void ZGraphicsScene::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event)
{
  if (event->button() != Qt::LeftButton) {
    return;
  }
  QGraphicsItem* item = itemAt(event->scenePos(), QTransform());
  auto roiItem = qgraphicsitem_cast<ROIGraphicsItem*>(item);
  auto roiCtrlPtItem = qgraphicsitem_cast<ROICtrlPtGraphicsItem*>(item);
  bool canUpdateROI = (!roiItem && !roiCtrlPtItem)
                      || event->modifiers() == Qt::ControlModifier
                      || event->modifiers() == Qt::AltModifier
                      || m_view->isRegionAnnotationMode();

  CHECK(!m_rectItem);
  CHECK(!m_ellipseItem);
  if (m_splineItem && (m_view->state() == ZView::State::ROISpline || m_view->state() == ZView::State::ROICut) && canUpdateROI) {
    if (m_view->state() == ZView::State::ROISpline) {
      m_spline.last() = m_spline[0];

      if (m_spline.size() > 3) {
        std::pair<int, int> sliceRange = m_view->currentSliceRange();
        if (m_view->isROIMode()) {
          for (auto i = sliceRange.first; i < sliceRange.second; ++i) {
            if (m_roiAction == ROIAction::New) {
              m_view->currentROIPack().roi().newSpline(i, m_spline);
            } else if (m_roiAction == ROIAction::Add) {
              m_view->currentROIPack().roi().addSpline(i, m_spline);
            } else if (m_roiAction == ROIAction::Subtract) {
              m_view->currentROIPack().roi().subtractSpline(i, m_spline);
            }
          }
        } else {
          CHECK(m_view->isRegionAnnotationMode());
          ZROI roi;
          for (auto i = sliceRange.first; i < sliceRange.second; ++i) {
            roi.newSpline(i, m_spline);
          }
          m_view->currentRegionAnnotationPack().mergeROIToCurrentRegion(roi);
        }
      }
    } else {
      m_spline.pop_back();

      if (m_spline.size() >= 2 && m_view->isRegionAnnotationMode()) {
        std::pair<int, int> sliceRange = m_view->currentSliceRange();
        ZROI roi;
        for (auto i = sliceRange.first; i < sliceRange.second; ++i) {
          roi.newLine(i, m_spline);
        }
        m_view->currentRegionAnnotationPack().mergeLineROI(roi);
      }
    }

    m_startPtItem.reset();
    m_ctrlPtsItem.clear();
    m_splineItem.reset();
  } else if (m_polygonItem && m_view->state() == ZView::State::ROIPolygon && canUpdateROI) {
    m_polygon.last() = m_polygon[0];
    if (m_polygon.size() > 3) {
      std::pair<int, int> sliceRange = m_view->currentSliceRange();
      if (m_view->isROIMode()) {
        for (auto i = sliceRange.first; i < sliceRange.second; ++i) {
          if (m_roiAction == ROIAction::New) {
            m_view->currentROIPack().roi().newPolygon(i, m_polygon);
          } else if (m_roiAction == ROIAction::Add) {
            m_view->currentROIPack().roi().addPolygon(i, m_polygon);
          } else if (m_roiAction == ROIAction::Subtract) {
            m_view->currentROIPack().roi().subtractPolygon(i, m_polygon);
          }
        }
      } else {
        CHECK(m_view->isRegionAnnotationMode());
        ZROI roi;
        for (auto i = sliceRange.first; i < sliceRange.second; ++i) {
          roi.newPolygon(i, m_polygon);
        }
        m_view->currentRegionAnnotationPack().mergeROIToCurrentRegion(roi);
      }
    }
    m_startPtItem.reset();
    m_ctrlPtsItem.clear();
    m_polygonItem.reset();
  } else {
    LOG(INFO) << "d " << event->scenePos() << " " << m_view->currentSlice();
    QGraphicsScene::mouseDoubleClickEvent(event);
  }
}

void ZGraphicsScene::mouseReleaseEvent(QGraphicsSceneMouseEvent* event)
{
  if (m_rectItem) {
    QRectF rect = m_rectItem->rect();
    if (rect.width() >= 1. && rect.height() >= 1.) {
      std::pair<int, int> sliceRange = m_view->currentSliceRange();
      if (m_view->isROIMode()) {
        for (auto i = sliceRange.first; i < sliceRange.second; ++i) {
          if (m_roiAction == ROIAction::New) {
            m_view->currentROIPack().roi().newRect(i, rect);
          } else if (m_roiAction == ROIAction::Add) {
            m_view->currentROIPack().roi().addRect(i, rect);
          } else if (m_roiAction == ROIAction::Subtract) {
            m_view->currentROIPack().roi().subtractRect(i, rect);
          }
        }
      } else {
        CHECK(m_view->isRegionAnnotationMode());
        ZROI roi;
        for (auto i = sliceRange.first; i < sliceRange.second; ++i) {
          roi.newRect(i, rect);
        }
        m_view->currentRegionAnnotationPack().mergeROIToCurrentRegion(roi);
      }
    }
    m_startPtItem.reset();
    m_rectItem.reset();
  } else if (m_ellipseItem) {
    QRectF rect = m_ellipseItem->rect();
    if (rect.width() >= 1. && rect.height() >= 1.) {
      std::pair<int, int> sliceRange = m_view->currentSliceRange();
      if (m_view->isROIMode()) {
        for (auto i = sliceRange.first; i < sliceRange.second; ++i) {
          if (m_roiAction == ROIAction::New) {
            m_view->currentROIPack().roi().newEllipse(i, rect);
          } else if (m_roiAction == ROIAction::Add) {
            m_view->currentROIPack().roi().addEllipse(i, rect);
          } else if (m_roiAction == ROIAction::Subtract) {
            m_view->currentROIPack().roi().subtractEllipse(i, rect);
          }
        }
      } else {
        CHECK(m_view->isRegionAnnotationMode());
        ZROI roi;
        for (auto i = sliceRange.first; i < sliceRange.second; ++i) {
          roi.newEllipse(i, rect);
        }
        m_view->currentRegionAnnotationPack().mergeROIToCurrentRegion(roi);
      }
    }
    m_startPtItem.reset();
    m_ellipseItem.reset();
  } else {
    QGraphicsScene::mouseReleaseEvent(event);
    if (!selectedItems().empty() && event->button() == Qt::LeftButton) {
      Q_EMIT mouseReleased(event->scenePos());
    }
  }
}

void ZGraphicsScene::mouseMoveEvent(QGraphicsSceneMouseEvent* event)
{
  QPointF scenePt = event->scenePos();
  scenePt.setX(glm::clamp(scenePt.x(), sceneRect().left(), sceneRect().right()));
  scenePt.setY(glm::clamp(scenePt.y(), sceneRect().top(), sceneRect().bottom()));
  m_view->setInfo(scenePt.x(), scenePt.y());
  m_view->checkViewport();

  if (m_splineItem) {
    if (event->buttons() == Qt::LeftButton) {
      double distThreshold = std::max(20.0, 20.0 / m_view->currentScale());
      distThreshold = distThreshold * distThreshold;
      if (QPointF::dotProduct(m_spline.last() - scenePt, m_spline.last() - scenePt) >= distThreshold) {
        QPointF ctrlPtHalfWidth(4.0 / m_view->currentScale(), 4.0 / m_view->currentScale());
        QPolygonF ctrlPoly = QRectF(scenePt - ctrlPtHalfWidth, scenePt + ctrlPtHalfWidth);
        auto ctrlPtItem = std::make_unique<QGraphicsPolygonItem>(ctrlPoly);
        ctrlPtItem->setZValue(m_zValue);
        ctrlPtItem->setPen(QPen(QColor(0, 0, 0), 0));
        ctrlPtItem->setBrush(QBrush(QColor(255, 255, 0, 128)));
        addItem(ctrlPtItem.get());
        m_ctrlPtsItem.push_back(std::move(ctrlPtItem));
        if (isScenePtOverlap(m_spline.last(), m_spline[m_spline.size() - 2])) {
          m_spline.last() = scenePt;
        } else {
          m_spline.push_back(scenePt);
        }
        m_spline << scenePt;   // add next potential pt
        m_splineItem->setPath(ZROI::splineToPainterPath(m_spline));
      }
    } else if (event->buttons() == Qt::NoButton) {
      m_spline.last() = scenePt;
      m_splineItem->setPath(ZROI::splineToPainterPath(m_spline));
    }
  } else if (m_polygonItem) {
    if (event->buttons() == Qt::LeftButton) {
      if (m_polygon.last() != scenePt) {
        if (isScenePtOverlap(m_polygon.last(), m_polygon[m_polygon.size() - 2])) {
          m_polygon.last() = scenePt;
        } else {
          m_polygon.push_back(scenePt);
        }
        QPainterPath path;
        path.addPolygon(m_polygon);
        m_polygonItem->setPath(path);
      }
    } else if (event->buttons() == Qt::NoButton) {
      m_polygon.last() = scenePt;
      QPainterPath path;
      path.addPolygon(m_polygon);
      m_polygonItem->setPath(path);
    }
  } else if (m_rectItem) {
    m_rectItem->setRect(std::min(scenePt.x(), m_startScenePt.x()),
                        std::min(scenePt.y(), m_startScenePt.y()),
                        std::max(scenePt.x(), m_startScenePt.x()) - std::min(scenePt.x(), m_startScenePt.x()),
                        std::max(scenePt.y(), m_startScenePt.y()) - std::min(scenePt.y(), m_startScenePt.y()));
  } else if (m_ellipseItem) {
    m_ellipseItem->setRect(std::min(scenePt.x(), m_startScenePt.x()),
                           std::min(scenePt.y(), m_startScenePt.y()),
                           std::max(scenePt.x(), m_startScenePt.x()) - std::min(scenePt.x(), m_startScenePt.x()),
                           std::max(scenePt.y(), m_startScenePt.y()) - std::min(scenePt.y(), m_startScenePt.y()));
  } else {
    QGraphicsScene::mouseMoveEvent(event);
    if (!selectedItems().empty()) {
      Q_EMIT mouseMoved(scenePt, event->modifiers());
    }
  }
}

} // namespace nim
