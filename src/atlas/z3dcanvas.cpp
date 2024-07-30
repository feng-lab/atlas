#include "z3dcanvas.h"

#include "z3drenderingengine.h"
#ifndef ATLAS_USE_OFFLINE_RENDERING
#include "z3dscene.h"
#include "z3dopenglwidget.h"
#endif
#include "zlog.h"
#include <QCoreApplication>
#include <algorithm>

// #define ATLAS_USE_OPENGLWINDOW

namespace nim {

#ifdef ATLAS_USE_OPENGLWIDGET

Z3DCanvas::Z3DCanvas(const QString& title, int width, int height, QWidget* parent, Qt::WindowFlags f)
  : QOpenGLWidget(parent, f)
{
  setWindowTitle(title);

  setAcceptDrops(true);
  setFocusPolicy(Qt::StrongFocus);

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

void Z3DCanvas::enterEvent(QEnterEvent* e)
{
  broadcastEvent(e, width(), height());
}

void Z3DCanvas::leaveEvent(QEvent* e)
{
  broadcastEvent(e, width(), height());
}

void Z3DCanvas::mousePressEvent(QMouseEvent* e)
{
  broadcastEvent(e, width(), height());
}

void Z3DCanvas::mouseReleaseEvent(QMouseEvent* e)
{
  broadcastEvent(e, width(), height());
}

void Z3DCanvas::mouseMoveEvent(QMouseEvent* e)
{
  broadcastEvent(e, width(), height());
}

void Z3DCanvas::mouseDoubleClickEvent(QMouseEvent* e)
{
  broadcastEvent(e, width(), height());
}

void Z3DCanvas::wheelEvent(QWheelEvent* e)
{
  broadcastEvent(e, width(), height());
}

void Z3DCanvas::keyPressEvent(QKeyEvent* event)
{
  QOpenGLWidget::keyPressEvent(event);
  broadcastEvent(event, width(), height());
}

void Z3DCanvas::keyReleaseEvent(QKeyEvent* event)
{
  broadcastEvent(event, width(), height());
}

void Z3DCanvas::resizeEvent(QResizeEvent* event)
{
  QOpenGLWidget::resizeEvent(event);
  Q_EMIT canvasSizeChanged(event->size().width() * devicePixelRatioF(), event->size().height() * devicePixelRatioF());
}

void Z3DCanvas::paintEvent(QPaintEvent* event)
{
  QOpenGLWidget::paintEvent(event);
}

void Z3DCanvas::dragEnterEvent(QDragEnterEvent* event)
{
  event->ignore();
}

void Z3DCanvas::dropEvent(QDropEvent* event)
{
  event->ignore();
}

void Z3DCanvas::initializeGL()
{
  Q_EMIT openGLContextInitialized();
}

void Z3DCanvas::resizeGL(int, int) {}

void Z3DCanvas::paintGL()
{
  if (!m_networkEvaluator) {
    return;
  }

  // QPainter set glclearcolor to white, we set it back
  glClearColor(0.f, 0.f, 0.f, 0.f);

  m_networkEvaluator->process(m_isStereoScene || m_fakeStereoOnce);
  m_fakeStereoOnce = false;
}

void Z3DCanvas::rotateX()
{
  getGLFocus();
  for (auto listener : m_listeners) {
    listener->rotateX();
  }
}

void Z3DCanvas::rotateY()
{
  getGLFocus();
  for (auto listener : m_listeners) {
    listener->rotateY();
  }
}

void Z3DCanvas::rotateZ()
{
  getGLFocus();
  for (auto listener : m_listeners) {
    listener->rotateZ();
  }
}

void Z3DCanvas::rotateXM()
{
  getGLFocus();
  for (auto listener : m_listeners) {
    listener->rotateXM();
  }
}

void Z3DCanvas::rotateYM()
{
  getGLFocus();
  for (auto listener : m_listeners) {
    listener->rotateYM();
  }
}

void Z3DCanvas::rotateZM()
{
  getGLFocus();
  for (auto listener : m_listeners) {
    listener->rotateZM();
  }
}

void Z3DCanvas::timerEvent(QTimerEvent* e)
{
  broadcastEvent(e, width(), height());
}

void Z3DCanvas::setNetworkEvaluator(Z3DNetworkEvaluator* n)
{
  m_networkEvaluator = n;
}

void Z3DCanvas::setFakeStereoOnce()
{
  m_fakeStereoOnce = true;
}

void Z3DCanvas::addEventListenerToBack(Z3DCanvasEventListener* e)
{
  if (e) {
    m_listeners.push_back(e);
  }
}

void Z3DCanvas::addEventListenerToFront(Z3DCanvasEventListener* e)
{
  if (e) {
    m_listeners.push_front(e);
  }
}

void Z3DCanvas::removeEventListener(Z3DCanvasEventListener* e)
{
  std::deque<Z3DCanvasEventListener*>::iterator pos;
  pos = std::find(m_listeners.begin(), m_listeners.end(), e);

  if (pos != m_listeners.end()) {
    m_listeners.erase(pos);
  }
}

void Z3DCanvas::clearEventListeners()
{
  m_listeners.clear();
}

void Z3DCanvas::broadcastEvent(QEvent* e, int w, int h)
{
  getGLFocus();
  for (auto listener : m_listeners) {
    listener->onEvent(e, w, h);
    if (e->isAccepted()) {
      break;
    }
  }
}

#else

Z3DCanvas::Z3DCanvas(const QString& title, int width, int height, QWidget* parent, Qt::WindowFlags f)
  : QGraphicsView(parent)
{
  setAlignment(Qt::AlignLeft | Qt::AlignTop);
  resize(width, height);

#ifdef ATLAS_USE_OFFLINE_RENDERING
  Q_UNUSED(f)
  m_scene = std::make_unique<QGraphicsScene>(0, 0, width, height);
  m_pixmapItem = new QGraphicsPixmapItem();
  // m_pixmapItem->setTransformationMode(Qt::SmoothTransformation);
  m_pixmapItem->setScale(1.0 / devicePixelRatio());
  m_scene->addItem(m_pixmapItem);
  setScene(m_scene.get());
#else
#ifdef ATLAS_USE_OPENGLWINDOW
  m_glWindow = new ZOpenGLWindow();
  m_glWindow->setFlags(Qt::WindowDoesNotAcceptFocus);
  m_3dScene = std::make_unique<Z3DScene>(width, height, m_glWindow->format().stereo(), *this);
  setViewport(QWidget::createWindowContainer(m_glWindow, this, f));
#else
  m_glWidget = new ZOpenGLWidget(this, f);
  m_3dScene = std::make_unique<Z3DScene>(width, height, m_glWidget->format().stereo(), *this);
  setViewport(m_glWidget);
#endif
  setScene(m_3dScene.get());
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

#ifndef ATLAS_USE_OFFLINE_RENDERING
#ifdef ATLAS_USE_OPENGLWINDOW
  connect(m_glWindow, &ZOpenGLWindow::openGLContextInitialized, this, &Z3DCanvas::openGLContextInitialized);
  connect(m_glWindow, &ZOpenGLWindow::openGLContextInitialized, m_3dScene.get(), &Z3DScene::initPainter);
#else
  connect(m_glWidget, &ZOpenGLWidget::openGLContextInitialized, this, &Z3DCanvas::openGLContextInitialized);
  connect(m_glWidget, &ZOpenGLWidget::openGLContextInitialized, m_3dScene.get(), &Z3DScene::initPainter);
#endif
#endif
}

Z3DCanvas::~Z3DCanvas()
{
  LOG(INFO) << "in canvas destructor";
  getGLFocus();
}

QOpenGLContext* Z3DCanvas::context() const
{
#ifdef ATLAS_USE_OFFLINE_RENDERING
  return nullptr;
#else
#ifdef ATLAS_USE_OPENGLWINDOW
  return m_glWindow->context();
#else
  return m_glWidget->context();
#endif
#endif
}

void Z3DCanvas::setRenderingEngine(Z3DRenderingEngine* engine)
{
  m_engine = engine;
#ifndef ATLAS_USE_OFFLINE_RENDERING
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
#ifdef ATLAS_USE_OFFLINE_RENDERING

    const std::lock_guard<std::mutex> lock(m_engine->targetSwitchMutex());
    if (m_engine->monoReadyTarget()) {
      auto startTarget = m_engine->monoReadyTarget();
      // VLOG(1) << m_engine->monoReadyPixmap().width() << " " << m_engine->monoReadyPixmap().height();
      m_pixmapItem->setPixmap(m_engine->monoReadyPixmap());
      auto endTarget = m_engine->monoReadyTarget();

      m_engine->clearNewRenderingFlag();
      VLOG(1) << startTarget << " " << endTarget << " " << physicalSize();
    }

#else
#ifdef ATLAS_USE_OPENGLWINDOW
    m_glWindow->update();
#else
    m_glWidget->update();
#endif
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
#ifdef ATLAS_USE_OFFLINE_RENDERING
  CHECK(m_scene);
  m_scene->setSceneRect(QRect(QPoint(0, 0), event->size()));
#else
  if (m_3dScene) {
    m_3dScene->setSceneRect(QRect(QPoint(0, 0), event->size()));
  }
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
#ifndef ATLAS_USE_OFFLINE_RENDERING
#ifdef ATLAS_USE_OPENGLWINDOW
  m_glWindow->makeCurrent();
#else
  m_glWidget->makeCurrent();
#endif
#endif
}

#endif

} // namespace nim
