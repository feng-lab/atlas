#include "zgraphicsview.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QWheelEvent>
#include <QScrollBar>
#include <cassert>
#include "QsLog.h"
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QGraphicsEllipseItem>
#include "zroi.h"
#include "zroifilter.h"
#include <QImageWriter>

namespace nim {

ZGraphicsView::ZGraphicsView(QGraphicsScene *scene, ZView *parent)
  : QGraphicsView(scene, parent)
  , m_view(parent)
  , m_scale("Scale", 100, 7, 10000)
  , m_roiAction(ROIAction::New)
  , m_startPtItem(nullptr)
  , m_ellipseItem(nullptr)
  , m_rectItem(nullptr)
  , m_polygonItem(nullptr)
  , m_splineItem(nullptr)

{
  setMinimumSize(400, 400);
  setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
  setOptimizationFlags(QGraphicsView::DontSavePainterState | QGraphicsView::DontAdjustForAntialiasing);
  setFocusPolicy(Qt::NoFocus);
  setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
  setRenderHint(QPainter::Antialiasing);
  setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
  setMouseTracking(true);
  //viewport()->setCursor(Qt::ArrowCursor);
  setBackgroundRole(QPalette::NoRole);
  setAcceptDrops(true);
  //setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  //setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

  m_scale.setStyle("SPINBOX");
  m_scale.setSuffix("%");
  m_scale.setDecimal(2);
  m_scale.setSingleStep(10);
  connect(&m_scale, &ZDoubleParameter::valueChanged, this, &ZGraphicsView::scaleParaChanged);

  connect(horizontalScrollBar(), &QScrollBar::sliderMoved, this, &ZGraphicsView::checkViewport);
  connect(verticalScrollBar(), &QScrollBar::sliderMoved, this, &ZGraphicsView::checkViewport);
}

void ZGraphicsView::updateScaleFactorRange()
{
  // find the ideal x / y scaling ratio to fit scene in the view.
  int margin = 2;
  QRectF viewRect = viewport()->rect().adjusted(margin, margin, -margin, -margin);
  if (viewRect.isEmpty())
    return;
  QRectF sceneRect = scene()->sceneRect();
  if (sceneRect.isEmpty())
    return;
  double xratio = viewRect.width() / sceneRect.width();
  double yratio = viewRect.height() / sceneRect.height();

  double ratio = std::min(xratio, yratio) * 100;
  if (ratio < m_scale.rangeMin())
    m_scale.setRange(ratio, m_scale.rangeMax());
  else if (ratio > m_scale.rangeMax())
    m_scale.setRange(m_scale.rangeMin(), ratio);
}

QRectF ZGraphicsView::getCurrrentlyVisibleRegion() const
{
  QPointF topLeft = mapToScene(0, 0);
  QPointF bottomRight = mapToScene(this->width(), this->height());
  return QRectF(topLeft, bottomRight);
}

QWidget *ZGraphicsView::createScaleWidget(QWidget *parent)
{
  QWidget* res = m_scale.createWidget(parent);
  res->setToolTip("Zoom");
  return res;
}

void ZGraphicsView::fitRect(const QRectF &rect)
{
  fitInView(rect, Qt::KeepAspectRatio);
  setScale(transform().m11());
}

bool ZGraphicsView::renderToImage(const QString &filename, QString *err)
{
  QRect vp = viewport()->rect();
  QImage img(vp.width(), vp.height(), QImage::Format_ARGB32_Premultiplied);
  img.fill(0);
  QPainter painter(&img);
  painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
  render(&painter);
  QImageWriter writer(filename);
  writer.setCompression(1);
  if (!writer.write(img)) {
    if (err)
      *err = writer.errorString();
    return false;
  }
  return true;
}

bool ZGraphicsView::renderToImage(const QString &filename, int width, int height, QString *err)
{
  QImage img(width, height, QImage::Format_ARGB32_Premultiplied);
  img.fill(0);
  QPainter painter(&img);
  painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
  render(&painter);
  QImageWriter writer(filename);
  writer.setCompression(1);
  if (!writer.write(img)) {
    if (err)
      *err = writer.errorString();
    return false;
  }
  return true;
}

void ZGraphicsView::setScrollHandDragMode()
{
  setDragMode(QGraphicsView::ScrollHandDrag);
  setInteractive(false);
}

void ZGraphicsView::setRubberBandDragMode()
{
  setDragMode(QGraphicsView::RubberBandDrag);
  setInteractive(true);
}

void ZGraphicsView::scaleParaChanged()
{
  double targetScale = m_scale.get() / 100.0;
  if (targetScale == transform().m11() * 100)
    return;
  double scaleFactor = targetScale / transform().m11();

  //double factor = transform().scale(scaleFactor, scaleFactor).mapRect(QRectF(0, 0, 1, 1)).width();

  scale(scaleFactor, scaleFactor);

  checkViewport();
}

void ZGraphicsView::checkViewport()
{
  static QRectF vp;
  QRectF rect = getCurrrentlyVisibleRegion();
  if (vp != rect) {
    vp = rect;
    emit viewportChanged();
  }
}

void ZGraphicsView::mousePressEvent(QMouseEvent *event)
{
  QGraphicsItem *item = itemAt(event->x(), event->y());
  ROIGraphicsItem* roiItem = qgraphicsitem_cast<ROIGraphicsItem*>(item);
  ROICtrlPtGraphicsItem* roiCtrlPtItem = qgraphicsitem_cast<ROICtrlPtGraphicsItem*>(item);
  bool canUpdateROI = (!roiItem && !roiCtrlPtItem)
      || event->modifiers() == Qt::ControlModifier
      || event->modifiers() == Qt::AltModifier;

  assert(!m_rectItem);
  assert(!m_ellipseItem);
  if ((m_splineItem || m_view->state() == ZView::State::ROISpline) && canUpdateROI) {
    QPointF scenePt = mapToScene(event->x(), event->y());
    if (event->button() == Qt::LeftButton) {
      if (!m_splineItem) {  //new
        assert(!m_startPtItem);
        assert(m_ctrlPtsItem.empty());
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
        if (m_roiAction == ROIAction::New) {
          std::pair<int,int> sliceRange = m_view->currentSliceRange();
          for (int i=sliceRange.first; i<sliceRange.second; ++i)
            m_view->roi().deleteSliceROI(i);
        }
        QPolygonF startPoly = mapToScene(event->x()-5, event->y()-5, 10, 10);
        m_startPtItem = new QGraphicsPolygonItem(startPoly);
        m_startPtItem->setPen(QPen(QColor(0,0,0), 0));
        m_startPtItem->setBrush(QBrush(QColor(255,255,255,128)));
        scene()->addItem(m_startPtItem);
        m_spline.clear();
        m_spline << m_startScenePt << m_startScenePt;  // pt and next potential pt
        m_splineItem = new QGraphicsPathItem(ZROI::splineToPainterPath(m_spline));
        m_splineItem->setPen(QPen(QColor(255,255,0), 0));
        scene()->addItem(m_splineItem);
      } else {
        if (m_startPtItem->contains(m_spline.last())) {  // finish here
          m_spline.last() = m_spline[0];
          if (m_spline.size() > 3) {
            std::pair<int,int> sliceRange = m_view->currentSliceRange();
            for (int i=sliceRange.first; i<sliceRange.second; ++i) {
              if (m_roiAction == ROIAction::Add || m_roiAction == ROIAction::New)
                m_view->roi().addSpline(i, m_spline);
              else if (m_roiAction == ROIAction::Subtract)
                m_view->roi().subtractSpline(i, m_spline);
            }
          }
          delete m_startPtItem;
          m_startPtItem = nullptr;
          for (auto ctrlItem : m_ctrlPtsItem)
            delete ctrlItem;
          m_ctrlPtsItem.clear();
          delete m_splineItem;
          m_splineItem = nullptr;
        } else if (!isScenePtOverlap(m_spline.last(), m_spline[m_spline.size()-2])) {
          QPolygonF ctrlPoly = mapToScene(event->x()-4, event->y()-4, 8, 8);
          QGraphicsPolygonItem *ctrlPtItem = new QGraphicsPolygonItem(ctrlPoly);
          ctrlPtItem->setPen(QPen(QColor(0,0,0), 0));
          ctrlPtItem->setBrush(QBrush(QColor(255,255,0,128)));
          scene()->addItem(ctrlPtItem);
          m_ctrlPtsItem.push_back(ctrlPtItem);

          m_spline << scenePt;   // add next potential pt
          m_splineItem->setPath(ZROI::splineToPainterPath(m_spline));
        }
      }
    } else if (event->button() == Qt::RightButton) {
      if (m_splineItem) {
        if (m_startPtItem->contains(m_spline.last())) {
          m_spline.last() = m_spline[0];
        } else {
          m_spline << m_spline[0];
        }
        if (m_spline.size() > 3) {
          std::pair<int,int> sliceRange = m_view->currentSliceRange();
          for (int i=sliceRange.first; i<sliceRange.second; ++i) {
            if (m_roiAction == ROIAction::Add || m_roiAction == ROIAction::New)
              m_view->roi().addSpline(i, m_spline);
            else if (m_roiAction == ROIAction::Subtract)
              m_view->roi().subtractSpline(i, m_spline);
          }
        }
        delete m_startPtItem;
        m_startPtItem = nullptr;
        for (auto ctrlItem : m_ctrlPtsItem)
          delete ctrlItem;
        m_ctrlPtsItem.clear();
        delete m_splineItem;
        m_splineItem = nullptr;
      }
    }
  } else if ((m_polygonItem || m_view->state() == ZView::State::ROIPolygon) && canUpdateROI) {
    QPointF scenePt = mapToScene(event->x(), event->y());
    if (event->button() == Qt::LeftButton) {
      if (!m_polygonItem) {  //new
        assert(!m_startPtItem);
        assert(m_ctrlPtsItem.empty());
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
        if (m_roiAction == ROIAction::New) {
          std::pair<int,int> sliceRange = m_view->currentSliceRange();
          for (int i=sliceRange.first; i<sliceRange.second; ++i)
            m_view->roi().deleteSliceROI(i);
        }
        QPolygonF startPoly = mapToScene(event->x()-5, event->y()-5, 10, 10);
        m_startPtItem = new QGraphicsPolygonItem(startPoly);
        m_startPtItem->setPen(QPen(QColor(0,0,0), 0));
        m_startPtItem->setBrush(QBrush(QColor(255,255,255,128)));
        scene()->addItem(m_startPtItem);
        m_polygon.clear();
        m_polygon << m_startScenePt << m_startScenePt;  // pt and next potential pt
        LINFO() << "";
        LINFO() << "first:" << m_startScenePt;
        QPainterPath path;
        path.addPolygon(m_polygon);
        m_polygonItem = new QGraphicsPathItem(path);
        m_polygonItem->setPen(QPen(QColor(255,255,0), 0));
        scene()->addItem(m_polygonItem);
      } else {
        if (m_startPtItem->contains(m_polygon.last())) {  // finish here
          m_polygon.last() = m_polygon[0];
          if (m_polygon.size() > 3) {
            std::pair<int,int> sliceRange = m_view->currentSliceRange();
            for (int i=sliceRange.first; i<sliceRange.second; ++i) {
              if (m_roiAction == ROIAction::Add || m_roiAction == ROIAction::New)
                m_view->roi().addPolygon(i, m_polygon);
              else if (m_roiAction == ROIAction::Subtract)
                m_view->roi().subtractPolygon(i, m_polygon);
            }
          }
          delete m_startPtItem;
          m_startPtItem = nullptr;
          for (auto ctrlItem : m_ctrlPtsItem)
            delete ctrlItem;
          m_ctrlPtsItem.clear();
          delete m_polygonItem;
          m_polygonItem = nullptr;
        } else if (!isScenePtOverlap(m_polygon.last(), m_polygon[m_polygon.size()-2])) {
          if (m_polygon.size() == 2) {
            LINFO() << "second:" << scenePt;
            double xdiff1 = m_polygon[0].x() - m_polygon[1].x();
            double ydiff1 = m_polygon[0].y() - m_polygon[1].y();
            LINFO() << "soma Length:" << std::sqrt(xdiff1*xdiff1 + ydiff1*ydiff1);
          } else if (m_polygon.size() == 3) {
            //LINFO() << "third:" << scenePt;
            //            double xdiff1 = m_polygon[0].x() - m_polygon[1].x();
            //            double ydiff1 = m_polygon[0].y() - m_polygon[1].y();
            //            double xdiff2 = m_polygon[2].x() - m_polygon[1].x();
            //            double ydiff2 = m_polygon[2].y() - m_polygon[1].y();
            //            double len1 = std::sqrt(xdiff1*xdiff1 + ydiff1*ydiff1);
            //            double len2 = std::sqrt(xdiff2*xdiff2 + ydiff2*ydiff2);

            //            LINFO() << "length1:" << len1
            //                    << "length2:" << len2
            //                    << "ratio:" << (len1 / (len1 + len2));
          } else if (m_polygon.size() == 4) {
            double xdiff1 = m_polygon[0].x() - m_polygon[1].x();
            double ydiff1 = m_polygon[0].y() - m_polygon[1].y();
            double xdiff2 = m_polygon[2].x() - m_polygon[1].x();
            double ydiff2 = m_polygon[2].y() - m_polygon[1].y();
            double xdiff3 = m_polygon[2].x() - m_polygon[3].x();
            double ydiff3 = m_polygon[2].y() - m_polygon[3].y();
            double len1 = std::sqrt(xdiff1*xdiff1 + ydiff1*ydiff1);
            double len2 = std::sqrt(xdiff2*xdiff2 + ydiff2*ydiff2);
            double len3 = std::sqrt(xdiff3*xdiff3 + ydiff3*ydiff3);
            LINFO() << len1 << len2 << len3;
          }

          QPolygonF ctrlPoly = mapToScene(event->x()-4, event->y()-4, 8, 8);
          QGraphicsPolygonItem *ctrlPtItem = new QGraphicsPolygonItem(ctrlPoly);
          ctrlPtItem->setPen(QPen(QColor(0,0,0), 0));
          ctrlPtItem->setBrush(QBrush(QColor(255,255,0,128)));
          scene()->addItem(ctrlPtItem);
          m_ctrlPtsItem.push_back(ctrlPtItem);

          m_polygon << scenePt;   // add next potential pt
          QPainterPath path;
          path.addPolygon(m_polygon);
          m_polygonItem->setPath(path);
        }
      }
    } else if (event->button() == Qt::RightButton) {
      if (m_polygonItem) {
        if (m_startPtItem->contains(m_polygon.last())) {
          m_polygon.last() = m_polygon[0];
        } else {
          m_polygon << m_polygon[0];
        }
        if (m_polygon.size() > 3) {
          std::pair<int,int> sliceRange = m_view->currentSliceRange();
          for (int i=sliceRange.first; i<sliceRange.second; ++i) {
            if (m_roiAction == ROIAction::Add || m_roiAction == ROIAction::New)
              m_view->roi().addPolygon(i, m_polygon);
            else if (m_roiAction == ROIAction::Subtract)
              m_view->roi().subtractPolygon(i, m_polygon);
          }
        }
        delete m_startPtItem;
        m_startPtItem = nullptr;
        for (auto ctrlItem : m_ctrlPtsItem)
          delete ctrlItem;
        m_ctrlPtsItem.clear();
        delete m_polygonItem;
        m_polygonItem = nullptr;
      }
    }
  } else if (m_view->state() == ZView::State::ROIRect && event->button() == Qt::LeftButton && canUpdateROI) {
    QPointF scenePt = mapToScene(event->x(), event->y());
    assert(!m_startPtItem);
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
    if (m_roiAction == ROIAction::New) {
      std::pair<int,int> sliceRange = m_view->currentSliceRange();
      for (int i=sliceRange.first; i<sliceRange.second; ++i)
        m_view->roi().deleteSliceROI(i);
    }
    QPolygonF startPoly = mapToScene(event->x()-5, event->y()-5, 10, 10);
    m_startPtItem = new QGraphicsPolygonItem(startPoly);
    m_startPtItem->setPen(QPen(QColor(0,0,0), 0));
    m_startPtItem->setBrush(QBrush(QColor(255,255,255,128)));
    scene()->addItem(m_startPtItem);
    m_rectItem = new QGraphicsRectItem(QRectF(scenePt, QSizeF(0,0)));
    m_rectItem->setPen(QPen(QColor(255,255,0), 0));
    scene()->addItem(m_rectItem);
  } else if (m_view->state() == ZView::State::ROIEllipse && event->button() == Qt::LeftButton && canUpdateROI) {
    QPointF scenePt = mapToScene(event->x(), event->y());
    assert(!m_startPtItem);
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
    if (m_roiAction == ROIAction::New) {
      std::pair<int,int> sliceRange = m_view->currentSliceRange();
      for (int i=sliceRange.first; i<sliceRange.second; ++i)
        m_view->roi().deleteSliceROI(i);
    }
    QPolygonF startPoly = mapToScene(event->x()-5, event->y()-5, 10, 10);
    m_startPtItem = new QGraphicsPolygonItem(startPoly);
    m_startPtItem->setPen(QPen(QColor(0,0,0), 0));
    m_startPtItem->setBrush(QBrush(QColor(255,255,255,128)));
    scene()->addItem(m_startPtItem);
    m_ellipseItem = new QGraphicsEllipseItem(QRectF(m_startScenePt, QSizeF(0,0)));
    m_ellipseItem->setPen(QPen(QColor(255,255,0), 0));
    scene()->addItem(m_ellipseItem);
  } else {
    QPointF scenePt = mapToScene(event->x(), event->y());
    LINFO() << scenePt.x() << scenePt.y() << m_view->currentSlice();
    QGraphicsView::mousePressEvent(event);
    //viewport()->setCursor(Qt::ArrowCursor);
  }
}

void ZGraphicsView::mouseMoveEvent(QMouseEvent *event)
{
  if (m_splineItem) {
    QPointF scenePt = mapToScene(event->x(), event->y());
    scenePt.setX(glm::clamp(scenePt.x(), sceneRect().left(), sceneRect().right()));
    scenePt.setY(glm::clamp(scenePt.y(), sceneRect().top(), sceneRect().bottom()));
    m_spline.last() = scenePt;
    m_splineItem->setPath(ZROI::splineToPainterPath(m_spline));
  } else if (m_polygonItem) {
    QPointF scenePt = mapToScene(event->x(), event->y());
    scenePt.setX(glm::clamp(scenePt.x(), sceneRect().left(), sceneRect().right()));
    scenePt.setY(glm::clamp(scenePt.y(), sceneRect().top(), sceneRect().bottom()));
    m_polygon.last() = scenePt;
    QPainterPath path;
    path.addPolygon(m_polygon);
    m_polygonItem->setPath(path);
  } else if (m_rectItem) {
    QPointF scenePt = mapToScene(event->x(), event->y());
    scenePt.setX(glm::clamp(scenePt.x(), sceneRect().left(), sceneRect().right()));
    scenePt.setY(glm::clamp(scenePt.y(), sceneRect().top(), sceneRect().bottom()));
    m_rectItem->setRect(std::min(scenePt.x(), m_startScenePt.x()),
                        std::min(scenePt.y(), m_startScenePt.y()),
                        std::max(scenePt.x(), m_startScenePt.x()) - std::min(scenePt.x(), m_startScenePt.x()),
                        std::max(scenePt.y(), m_startScenePt.y()) - std::min(scenePt.y(), m_startScenePt.y()));
  } else if (m_ellipseItem) {
    QPointF scenePt = mapToScene(event->x(), event->y());
    scenePt.setX(glm::clamp(scenePt.x(), sceneRect().left(), sceneRect().right()));
    scenePt.setY(glm::clamp(scenePt.y(), sceneRect().top(), sceneRect().bottom()));
    m_ellipseItem->setRect(std::min(scenePt.x(), m_startScenePt.x()),
                           std::min(scenePt.y(), m_startScenePt.y()),
                           std::max(scenePt.x(), m_startScenePt.x()) - std::min(scenePt.x(), m_startScenePt.x()),
                           std::max(scenePt.y(), m_startScenePt.y()) - std::min(scenePt.y(), m_startScenePt.y()));
  } else {
    QGraphicsView::mouseMoveEvent(event);
    checkViewport();
  }
}

void ZGraphicsView::mouseReleaseEvent(QMouseEvent *event)
{
  if (m_rectItem) {
    QRectF rect = m_rectItem->rect();
    if (rect.width() >= 1. && rect.height() >= 1.) {
      std::pair<int,int> sliceRange = m_view->currentSliceRange();
      for (int i=sliceRange.first; i<sliceRange.second; ++i) {
        if (m_roiAction == ROIAction::Add || m_roiAction == ROIAction::New)
          m_view->roi().addRect(i, rect);
        else if (m_roiAction == ROIAction::Subtract)
          m_view->roi().subtractRect(i, rect);
      }
    }
    delete m_startPtItem;
    m_startPtItem = nullptr;
    delete m_rectItem;
    m_rectItem = nullptr;
  } else if (m_ellipseItem) {
    QRectF rect = m_ellipseItem->rect();
    if (rect.width() >= 1. && rect.height() >= 1.) {
      std::pair<int,int> sliceRange = m_view->currentSliceRange();
      for (int i=sliceRange.first; i<sliceRange.second; ++i) {
        if (m_roiAction == ROIAction::Add || m_roiAction == ROIAction::New)
          m_view->roi().addEllipse(i, rect);
        else if (m_roiAction == ROIAction::Subtract)
          m_view->roi().subtractEllipse(i, rect);
      }
    }
    delete m_startPtItem;
    m_startPtItem = nullptr;
    delete m_ellipseItem;
    m_ellipseItem = nullptr;
  } else {
    QGraphicsView::mouseReleaseEvent(event);
    //viewport()->setCursor(Qt::ArrowCursor);
  }
}

void ZGraphicsView::dragEnterEvent(QDragEnterEvent *event)
{
  event->ignore();
}

void ZGraphicsView::dropEvent(QDropEvent *event)
{
  event->ignore();
}

void ZGraphicsView::wheelEvent(QWheelEvent *event)
{
  QGraphicsView::wheelEvent(event);
  checkViewport();
}

} // namespace nim
