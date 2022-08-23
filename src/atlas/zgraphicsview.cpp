#include "zgraphicsview.h"

#include "zimg.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QWheelEvent>
#include <QScrollBar>
#include <QPinchGesture>
#include <QPanGesture>

namespace nim {

ZGraphicsView::ZGraphicsView(QGraphicsScene* scene, ZView* parent)
  : QGraphicsView(scene, parent)
  , m_view(parent)
  , m_scale("Scale", 100, 1, 10000)
{
  setMinimumSize(400, 400);
  setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
  setOptimizationFlags(QGraphicsView::DontSavePainterState | QGraphicsView::DontAdjustForAntialiasing);
  setFocusPolicy(Qt::NoFocus);
  setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
  setRenderHint(QPainter::Antialiasing);
  setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
  // setResizeAnchor(QGraphicsView::AnchorUnderMouse);
  setMouseTracking(true);
  // viewport()->setCursor(Qt::ArrowCursor);
  setBackgroundRole(QPalette::NoRole);
  setAcceptDrops(true);
  // setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  // setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

  grabGesture(Qt::PanGesture);
  grabGesture(Qt::PinchGesture);

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
  if (viewRect.isEmpty()) {
    return;
  }
  QRectF sceneRect = scene()->sceneRect();
  if (sceneRect.isEmpty()) {
    return;
  }
  double xratio = viewRect.width() / sceneRect.width();
  double yratio = viewRect.height() / sceneRect.height();

  double ratio = std::min(xratio, yratio) * 100;
  if (ratio < m_scale.rangeMin()) {
    m_scale.setRange(ratio, m_scale.rangeMax());
  } else if (ratio > m_scale.rangeMax()) {
    m_scale.setRange(m_scale.rangeMin(), ratio);
  }
}

QRectF ZGraphicsView::getCurrrentlyVisibleRegion() const
{
  QPointF topLeft = mapToScene(0, 0);
  QPointF bottomRight = mapToScene(this->width(), this->height());
  return QRectF(topLeft, bottomRight);
}

QWidget* ZGraphicsView::createScaleWidget(QWidget* parent)
{
  QWidget* res = m_scale.createWidget(parent);
  res->setToolTip("Zoom");
  return res;
}

void ZGraphicsView::fitRect(const QRectF& rect)
{
  fitInView(rect, Qt::KeepAspectRatio);
  // LOG(INFO) << rect << transform().m11();
  setScale(transform().m11());
}

bool ZGraphicsView::renderToImage(const QString& filename, QString* err)
{
  try {
    QRect vp = viewport()->rect();
    QImage img(vp.width(), vp.height(), QImage::Format_ARGB32);
    img.fill(0);
    QPainter painter(&img);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    render(&painter);
    ZImg::fromQImage(img).save(filename);
    return true;
  }
  catch (const ZException& e) {
    *err = QString(e.what());
    LOG(ERROR) << "Exception: " << e.what();
    return false;
  }
}

bool ZGraphicsView::renderToImage(const QString& filename, int width, int height, QString* err)
{
  try {
    QImage img(width, height, QImage::Format_ARGB32);
    img.fill(0);
    QPainter painter(&img);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    render(&painter, QRectF(), QRect(), Qt::KeepAspectRatioByExpanding);
    ZImg::fromQImage(img).save(filename);
    return true;
  }
  catch (const ZException& e) {
    *err = QString(e.what());
    LOG(ERROR) << "Exception: " << e.what();
    return false;
  }
}

void ZGraphicsView::scaleParaChanged()
{
  double targetScale = m_scale.get() / 100.0;
  if (targetScale == transform().m11() * 100) {
    return;
  }
  double scaleFactor = targetScale / transform().m11();

  // double factor = transform().scale(scaleFactor, scaleFactor).mapRect(QRectF(0, 0, 1, 1)).width();

  scale(scaleFactor, scaleFactor);

  checkViewport();
}

void ZGraphicsView::checkViewport()
{
  static QRectF vp;
  QRectF rect = getCurrrentlyVisibleRegion();
  if (vp != rect) {
    vp = rect;
    Q_EMIT viewportChanged();
  }
}

void ZGraphicsView::dragEnterEvent(QDragEnterEvent* event)
{
  event->ignore();
}

void ZGraphicsView::dropEvent(QDropEvent* event)
{
  event->ignore();
}

void ZGraphicsView::wheelEvent(QWheelEvent* event)
{
  QGraphicsView::wheelEvent(event);
  checkViewport();
}

bool ZGraphicsView::event(QEvent* event)
{
  if (event->type() == QEvent::Gesture) {
    return gestureEvent(static_cast<QGestureEvent*>(event));
  }
  return QGraphicsView::event(event);
}

bool ZGraphicsView::gestureEvent(QGestureEvent* event)
{
  LOG(INFO) << "gestureEvent():" << event;
  if (QGesture* pan = event->gesture(Qt::PanGesture)) {
    panTriggered(static_cast<QPanGesture*>(pan));
  }
  if (QGesture* pinch = event->gesture(Qt::PinchGesture)) {
    pinchTriggered(static_cast<QPinchGesture*>(pinch));
  }
  return true;
}

void ZGraphicsView::panTriggered(QPanGesture* gesture)
{
#ifndef QT_NO_CURSOR
  switch (gesture->state()) {
    case Qt::GestureStarted:
    case Qt::GestureUpdated:
      setCursor(Qt::SizeAllCursor);
      break;
    default:
      setCursor(Qt::ArrowCursor);
  }
#endif
  QPointF delta = gesture->delta();
  LOG(INFO) << "panTriggered():" << gesture;
  translate(delta.x(), delta.y());
}

void ZGraphicsView::pinchTriggered(QPinchGesture* gesture)
{
  QPinchGesture::ChangeFlags changeFlags = gesture->changeFlags();
  if (changeFlags & QPinchGesture::RotationAngleChanged) {
    //    qreal rotationDelta = gesture->rotationAngle() - gesture->lastRotationAngle();
    //    rotationAngle += rotationDelta;
    //    qCDebug(lcExample) << "pinchTriggered(): rotate by" <<
    //                       rotationDelta << "->" << rotationAngle;
  }
  if (changeFlags & QPinchGesture::ScaleFactorChanged) {
    m_currentStepScaleFactor = gesture->scaleFactor();
    LOG(INFO) << "pinchTriggered(): zoom by " << gesture->scaleFactor() << " -> " << m_currentStepScaleFactor;
    setScale(currentScale() * m_currentStepScaleFactor);
  }
  //  if (gesture->state() == Qt::GestureFinished) {
  //    setScale(currentScale() * m_currentStepScaleFactor)
  //    m_currentStepScaleFactor = 1.;
  //  }
}

} // namespace nim
