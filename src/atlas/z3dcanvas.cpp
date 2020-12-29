#include "z3dcanvas.h"

#include "z3dnetworkevaluator.h"
#include "z3dcanvaseventlistener.h"
#include "z3dscene.h"
#include "zlog.h"
#include "z3dopenglwidget.h"
#include <algorithm>

namespace nim {

#ifdef ATLAS_USE_OPENGLWIDGET

Z3DCanvas::Z3DCanvas(const QString &title, int width, int height, QWidget* parent, Qt::WindowFlags f)
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

void Z3DCanvas::mouseReleaseEvent (QMouseEvent* e)
{
  broadcastEvent(e, width(), height());
}

void Z3DCanvas::mouseMoveEvent(QMouseEvent*  e)
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

void Z3DCanvas::resizeEvent(QResizeEvent *event)
{
  QOpenGLWidget::resizeEvent(event);
  emit canvasSizeChanged(event->size().width() * devicePixelRatioF(),
                         event->size().height() * devicePixelRatioF());
}

void Z3DCanvas::paintEvent(QPaintEvent *event)
{
  QOpenGLWidget::paintEvent(event);
}

void Z3DCanvas::dragEnterEvent(QDragEnterEvent *event)
{
  event->ignore();
}

void Z3DCanvas::dropEvent(QDropEvent *event)
{
  event->ignore();
}

void Z3DCanvas::initializeGL()
{
  emit openGLContextInitialized();
}

void Z3DCanvas::resizeGL(int w, int h)
{
  Q_UNUSED(w)
  Q_UNUSED(h)
}

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

void Z3DCanvas::setNetworkEvaluator(Z3DNetworkEvaluator *n)
{
  m_networkEvaluator = n;
}

void Z3DCanvas::setFakeStereoOnce()
{
  m_fakeStereoOnce = true;
}

void Z3DCanvas::addEventListenerToBack(Z3DCanvasEventListener *e)
{
  if (e)
    m_listeners.push_back(e);
}

void Z3DCanvas::addEventListenerToFront(Z3DCanvasEventListener *e)
{
  if (e)
    m_listeners.push_front(e);
}

void Z3DCanvas::removeEventListener(Z3DCanvasEventListener *e)
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

void Z3DCanvas::broadcastEvent(QEvent *e, int w, int h)
{
  getGLFocus();
  for (auto listener : m_listeners) {
    listener->onEvent(e, w, h);
    if (e->isAccepted())
      break;
  }
}

#else


Z3DCanvas::Z3DCanvas(const QString& title, int width, int height, QWidget* parent, Qt::WindowFlags f)
  : QGraphicsView(parent)
{
  setAlignment(Qt::AlignLeft | Qt::AlignTop);
  resize(width, height);

#ifdef ATLAS_USE_OPENGLWINDOW
  m_glWindow = new ZOpenGLWindow();
  m_glWindow->setFlags(Qt::WindowDoesNotAcceptFocus);
  m_3dScene = new Z3DScene(width, height, m_glWindow->format().stereo(), this);
  setViewport(QWidget::createWindowContainer(m_glWindow, nullptr, f));
#else
  m_glWidget = new ZOpenGLWidget(nullptr, f);
  m_3dScene = new Z3DScene(width, height, m_glWidget->format().stereo(), this);
  setViewport(m_glWidget);
#endif

  setViewportUpdateMode(FullViewportUpdate);
  setScene(m_3dScene);

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
#else
  connect(m_glWidget, &ZOpenGLWidget::openGLContextInitialized, this, &Z3DCanvas::openGLContextInitialized);
#endif
}

QSurfaceFormat Z3DCanvas::format() const
{
#ifdef ATLAS_USE_OPENGLWINDOW
  return m_glWindow->format();
#else
  return m_glWidget->format();
#endif
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

void Z3DCanvas::updateAll()
{
#ifdef ATLAS_USE_OPENGLWINDOW
  m_glWindow->update();
#else
  m_glWidget->update();
#endif
}

void Z3DCanvas::contextMenuEvent(QContextMenuEvent* event)
{
  broadcastEvent(event, width(), height());
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
  QGraphicsView::keyPressEvent(event);
  broadcastEvent(event, width(), height());
}

void Z3DCanvas::keyReleaseEvent(QKeyEvent* event)
{
  broadcastEvent(event, width(), height());
}

void Z3DCanvas::resizeEvent(QResizeEvent* event)
{
  getGLFocus();
  QGraphicsView::resizeEvent(event);
  if (m_3dScene)
    m_3dScene->setSceneRect(QRect(QPoint(0, 0), event->size()));

  emit canvasSizeChanged(event->size().width() * devicePixelRatioF(),
                         event->size().height() * devicePixelRatioF());
}

void Z3DCanvas::paintEvent(QPaintEvent* event)
{
  getGLFocus();
  QGraphicsView::paintEvent(event);
}

void Z3DCanvas::dragEnterEvent(QDragEnterEvent* event)
{
  event->ignore();
}

void Z3DCanvas::dropEvent(QDropEvent* event)
{
  event->ignore();
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

void Z3DCanvas::setNetworkEvaluator(Z3DNetworkEvaluator *n)
{
  m_3dScene->setNetworkEvaluator(n);
}

void Z3DCanvas::setFakeStereoOnce()
{
  m_3dScene->setFakeStereoOnce();
}

void Z3DCanvas::removeEventListener(Z3DCanvasEventListener& e)
{
  erase(m_listeners, &e);
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
    if (e->isAccepted())
      break;
  }
}

void Z3DCanvas::getGLFocus()
{
#ifdef ATLAS_USE_OPENGLWINDOW
  m_glWindow->makeCurrent();
#else
  m_glWidget->makeCurrent();
#endif
}

#endif

} // namespace nim
