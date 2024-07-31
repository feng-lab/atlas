#include "z3dcanvas.h"

#include "z3drenderingengine.h"
#if defined(ATLAS_USE_OPENGLWIDGET) || defined(ATLAS_USE_OPENGLWINDOW)
#include "z3dscene.h"
#include "z3dopenglwidget.h"
#endif
#include "zlog.h"
#include <QCoreApplication>
#include <algorithm>

namespace nim {

Z3DCanvas::Z3DCanvas(const QString& title, int width, int height, QWidget* parent, Qt::WindowFlags f)
  : QGraphicsView(parent)
{
  setAlignment(Qt::AlignLeft | Qt::AlignTop);
  resize(width, height);

#ifdef ATLAS_USE_OPENGLWINDOW
  m_glWindow = new ZOpenGLWindow();
  m_glWindow->setFlags(Qt::WindowDoesNotAcceptFocus);
  m_3dScene = std::make_unique<Z3DScene>(width, height, m_glWindow->format().stereo(), *this);
  setViewport(QWidget::createWindowContainer(m_glWindow, this, f));
  setScene(m_3dScene.get());
#elif defined(ATLAS_USE_OPENGLWIDGET)
  m_glWidget = new ZOpenGLWidget(this, f);
  m_3dScene = std::make_unique<Z3DScene>(width, height, m_glWidget->format().stereo(), *this);
  setViewport(m_glWidget);
  setScene(m_3dScene.get());
#else
  Q_UNUSED(f)
  m_scene = std::make_unique<QGraphicsScene>(0, 0, width, height);
  m_pixmapItem = new QGraphicsPixmapItem();
  // m_pixmapItem->setTransformationMode(Qt::SmoothTransformation);
  m_pixmapItem->setScale(1.0 / devicePixelRatio());
  m_scene->addItem(m_pixmapItem);
  setScene(m_scene.get());
#endif

  setViewportUpdateMode(FullViewportUpdate);

  setWindowTitle(title);
  setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
  setOptimizationFlags(QGraphicsView::DontSavePainterState | QGraphicsView::DontAdjustForAntialiasing);

  setAcceptDrops(true);
  setFocusPolicy(Qt::StrongFocus);

#if !defined(__APPLE__) && !defined(_WIN32)
  setStyleSheet("border-style: none;");
#endif

  m_rotateXShortCut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_X), this);
  connect(m_rotateXShortCut, &QShortcut::activated, this, &Z3DCanvas::rotateX);
  m_rotateYShortCut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Y), this);
  connect(m_rotateYShortCut, &QShortcut::activated, this, &Z3DCanvas::rotateY);
  m_rotateZShortCut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Z), this);
  connect(m_rotateZShortCut, &QShortcut::activated, this, &Z3DCanvas::rotateZ);
  m_rotateXMShortCut = new QShortcut(QKeySequence(QKeyCombination(Qt::ALT | Qt::SHIFT, Qt::Key_X)), this);
  connect(m_rotateXMShortCut, &QShortcut::activated, this, &Z3DCanvas::rotateXM);
  m_rotateYMShortCut = new QShortcut(QKeySequence(QKeyCombination(Qt::ALT | Qt::SHIFT, Qt::Key_Y)), this);
  connect(m_rotateYMShortCut, &QShortcut::activated, this, &Z3DCanvas::rotateYM);
  m_rotateZMShortCut = new QShortcut(QKeySequence(QKeyCombination(Qt::ALT | Qt::SHIFT, Qt::Key_Z)), this);
  connect(m_rotateZMShortCut, &QShortcut::activated, this, &Z3DCanvas::rotateZM);

#ifdef ATLAS_USE_OPENGLWINDOW
  connect(m_glWindow, &ZOpenGLWindow::openGLContextInitialized, this, &Z3DCanvas::openGLContextInitialized);
  connect(m_glWindow, &ZOpenGLWindow::openGLContextInitialized, m_3dScene.get(), &Z3DScene::initPainter);
#elif defined(ATLAS_USE_OPENGLWIDGET)
  connect(m_glWidget, &ZOpenGLWidget::openGLContextInitialized, this, &Z3DCanvas::openGLContextInitialized);
  connect(m_glWidget, &ZOpenGLWidget::openGLContextInitialized, m_3dScene.get(), &Z3DScene::initPainter);
#endif
}

Z3DCanvas::~Z3DCanvas()
{
  LOG(INFO) << "in canvas destructor";
  getGLFocus();
}

QOpenGLContext* Z3DCanvas::context() const
{
#ifdef ATLAS_USE_OPENGLWINDOW
  return m_glWindow->context();
#elif defined(ATLAS_USE_OPENGLWIDGET)
  return m_glWidget->context();
#else
  return nullptr;
#endif
}

void Z3DCanvas::setRenderingEngine(Z3DRenderingEngine* engine)
{
  m_engine = engine;
#if defined(ATLAS_USE_OPENGLWIDGET) || defined(ATLAS_USE_OPENGLWINDOW)
  m_3dScene->setRenderingEngine(engine);
#endif
  sceneParaUpdated();
}

void Z3DCanvas::toggleFullScreen()
{
  if (m_fullscreen) {
    m_fullscreen = false;
    showNormal();
  } else {
    showFullScreen();
    m_fullscreen = !m_fullscreen;
  }
}

void Z3DCanvas::sceneParaUpdated()
{
  VLOG(1) << "sceneParaUpdated";
  if (m_engine) {
    QCoreApplication::postEvent(m_engine, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
  }
}

void Z3DCanvas::renderingFinished()
{
  if (m_engine->hasNewRenderingFlag()) {
    VLOG(1) << "update";

#ifdef ATLAS_USE_OPENGLWINDOW
    m_glWindow->update();
#elif defined(ATLAS_USE_OPENGLWIDGET)
    m_glWidget->update();
#else
    const std::lock_guard<std::mutex> lock(m_engine->targetSwitchMutex());
    auto localBuffer = m_engine->monoReadyLocalBuffer();
    // CHECK(localBuffer);
    QImage image(localBuffer->data.data(),
                 localBuffer->width,
                 localBuffer->height,
                 QImage::Format_ARGB32_Premultiplied);
    m_pixmapItem->setPixmap(QPixmap::fromImage(image.mirrored()));

    m_engine->clearNewRenderingFlag();
    VLOG(1) << localBuffer << " " << localBuffer->width << " " << localBuffer->height;
#endif
  }
}

void Z3DCanvas::contextMenuEvent(QContextMenuEvent* e)
{
  if (m_engine) {
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

// void Z3DCanvas::enterEvent(QEnterEvent* e)
//{
//   if (m_engine) {
//     QCoreApplication::postEvent(m_engine, e->clone());
//   }
// }
//
// void Z3DCanvas::leaveEvent(QEvent* e)
//{
//   if (m_engine) {
//     QCoreApplication::postEvent(m_engine, e->clone());
//   }
// }

void Z3DCanvas::mousePressEvent(QMouseEvent* e)
{
  if (m_engine) {
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::mouseReleaseEvent(QMouseEvent* e)
{
  if (m_engine) {
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::mouseMoveEvent(QMouseEvent* e)
{
  // LOG(INFO) << "mousemoveevent";
  if (m_engine) {
    m_engine->cancelLongRendering();
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::mouseDoubleClickEvent(QMouseEvent* e)
{
  if (m_engine) {
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::wheelEvent(QWheelEvent* e)
{
  // LOG(INFO) << "wheelevent";
  if (m_engine && e->angleDelta().y() != 0) {
    m_engine->cancelLongRendering();
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::keyPressEvent(QKeyEvent* e)
{
  QGraphicsView::keyPressEvent(e);
  if (m_engine) {
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::keyReleaseEvent(QKeyEvent* e)
{
  if (m_engine) {
    QCoreApplication::postEvent(m_engine, e->clone());
  }
}

void Z3DCanvas::resizeEvent(QResizeEvent* event)
{
  QGraphicsView::resizeEvent(event);
#if defined(ATLAS_USE_OPENGLWIDGET) || defined(ATLAS_USE_OPENGLWINDOW)
  if (m_3dScene) {
    m_3dScene->setSceneRect(QRect(QPoint(0, 0), event->size()));
  }
#else
  CHECK(m_scene);
  m_scene->setSceneRect(QRect(QPoint(0, 0), event->size()));
#endif

  Q_EMIT canvasSizeChanged(event->size().width() * devicePixelRatio(), event->size().height() * devicePixelRatio());
}

void Z3DCanvas::dragEnterEvent(QDragEnterEvent* event)
{
  event->ignore();
}

void Z3DCanvas::dropEvent(QDropEvent* event)
{
  event->ignore();
}

void Z3DCanvas::timerEvent(QTimerEvent* e)
{
  //  if (m_engine) {
  //    QCoreApplication::postEvent(m_engine, e->clone());
  //  }
  QGraphicsView::timerEvent(e);
}

void Z3DCanvas::getGLFocus()
{
#ifdef ATLAS_USE_OPENGLWINDOW
  m_glWindow->makeCurrent();
#elif defined(ATLAS_USE_OPENGLWIDGET)
  m_glWidget->makeCurrent();
#endif
}

} // namespace nim
