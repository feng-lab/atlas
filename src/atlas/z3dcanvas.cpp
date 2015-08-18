#include "z3dcanvas.h"

#include "z3dnetworkevaluator.h"
#include <algorithm>
#include "z3dcanvaseventlistener.h"
#include "z3dscene.h"
#include "QsLog/QsLog.h"
#ifndef _QT4_
#include <QWindow>
#include <QOpenGLWidget>
#else
#include <QGLWidget>
#endif

namespace nim {

#ifdef nothing

Z3DCanvas::Z3DCanvas(const QString &title, QWidget* parent, Qt::WindowFlags f)
  : QOpenGLWidget(parent, f)
  , m_fullscreen(false)
  , m_networkEvaluator(nullptr)
  , m_isStereoScene(false)
  , m_fakeStereoOnce(false)
{
  setWindowTitle(title);

  setAcceptDrops(true);
  setFocusPolicy(Qt::StrongFocus);

  m_rotateXShortCut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_X), this);
  connect(m_rotateXShortCut, SIGNAL(activated()), this, SLOT(rotateX()));
  m_rotateYShortCut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_Y), this);
  connect(m_rotateYShortCut, SIGNAL(activated()), this, SLOT(rotateY()));
  m_rotateZShortCut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_Z), this);
  connect(m_rotateZShortCut, SIGNAL(activated()), this, SLOT(rotateZ()));
  m_rotateXMShortCut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_X + Qt::SHIFT), this);
  connect(m_rotateXMShortCut, SIGNAL(activated()), this, SLOT(rotateXM()));
  m_rotateYMShortCut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_Y + Qt::SHIFT), this);
  connect(m_rotateYMShortCut, SIGNAL(activated()), this, SLOT(rotateYM()));
  m_rotateZMShortCut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_Z + Qt::SHIFT), this);
  connect(m_rotateZMShortCut, SIGNAL(activated()), this, SLOT(rotateZM()));
}

Z3DCanvas::~Z3DCanvas() {}

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

void Z3DCanvas::enterEvent(QEvent* e)
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
  emit canvasSizeChanged(event->size().width() * devicePixelRatio(),
                         event->size().height() * devicePixelRatio());
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
  for (size_t i = 0 ; i < m_listeners.size() ; ++i) {
    m_listeners[i]->rotateX();
  }
}

void Z3DCanvas::rotateY()
{
  getGLFocus();
  for (size_t i = 0 ; i < m_listeners.size() ; ++i) {
    m_listeners[i]->rotateY();
  }
}

void Z3DCanvas::rotateZ()
{
  getGLFocus();
  for (size_t i = 0 ; i < m_listeners.size() ; ++i) {
    m_listeners[i]->rotateZ();
  }
}

void Z3DCanvas::rotateXM()
{
  getGLFocus();
  for (size_t i = 0 ; i < m_listeners.size() ; ++i) {
    m_listeners[i]->rotateXM();
  }
}

void Z3DCanvas::rotateYM()
{
  getGLFocus();
  for (size_t i = 0 ; i < m_listeners.size() ; ++i) {
    m_listeners[i]->rotateYM();
  }
}

void Z3DCanvas::rotateZM()
{
  getGLFocus();
  for (size_t i = 0 ; i < m_listeners.size() ; ++i) {
    m_listeners[i]->rotateZM();
  }
}

void Z3DCanvas::timerEvent(QTimerEvent* e)
{
  broadcastEvent(e, width(), height());
}

void Z3DCanvas::setNetworkEvaluator(Z3DNetworkEvaluator *n)
{
  m_networkEvaluator = n;
  if (n)
    n->setOpenGLContext(this);
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

  if (pos != m_listeners.end())
    m_listeners.erase(pos);
}

void Z3DCanvas::clearEventListeners()
{
  m_listeners.clear();
}

void Z3DCanvas::broadcastEvent(QEvent *e, int w, int h)
{
  getGLFocus();
  for (size_t i = 0 ; i < m_listeners.size() ; ++i) {
    m_listeners[i]->onEvent(e, w, h);
    if (e->isAccepted())
      break;
  }
}

double Z3DCanvas::devicePixelRatio()
{
  return (window() && window()->windowHandle()) ?
        window()->windowHandle()->devicePixelRatio() : 1.0;
}

#else


#ifndef _QT4_
Z3DCanvas::Z3DCanvas(const QString &title, int width, int height, QWidget* parent, Qt::WindowFlags f)
#else
Z3DCanvas::Z3DCanvas(const QString &title, int width, int height, const QGLFormat &format,
                     QWidget* parent, const QGLWidget *shareWidget, Qt::WindowFlags f)
#endif
  : QGraphicsView(parent)
  , m_fullscreen(false)
  , m_glWidget(nullptr)
  , m_3dScene(nullptr)
{
  setAlignment(Qt::AlignLeft | Qt::AlignTop);
  resize(width, height);

#ifndef _QT4_
  m_glWidget = new QOpenGLWidget(nullptr, f);
#else
  m_glWidget = new QGLWidget(format, NULL, shareWidget, f);
  m_glWidget->makeCurrent();
#endif
  m_3dScene = new Z3DScene(width, height, m_glWidget->format().stereo(), this);

  setViewport(m_glWidget);
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

  m_rotateXShortCut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_X), this);
  connect(m_rotateXShortCut, SIGNAL(activated()), this, SLOT(rotateX()));
  m_rotateYShortCut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_Y), this);
  connect(m_rotateYShortCut, SIGNAL(activated()), this, SLOT(rotateY()));
  m_rotateZShortCut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_Z), this);
  connect(m_rotateZShortCut, SIGNAL(activated()), this, SLOT(rotateZ()));
  m_rotateXMShortCut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_X + Qt::SHIFT), this);
  connect(m_rotateXMShortCut, SIGNAL(activated()), this, SLOT(rotateXM()));
  m_rotateYMShortCut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_Y + Qt::SHIFT), this);
  connect(m_rotateYMShortCut, SIGNAL(activated()), this, SLOT(rotateYM()));
  m_rotateZMShortCut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_Z + Qt::SHIFT), this);
  connect(m_rotateZMShortCut, SIGNAL(activated()), this, SLOT(rotateZM()));
}

Z3DCanvas::~Z3DCanvas() {}

#ifndef _QT4_
QSurfaceFormat Z3DCanvas::format() const
{
  return m_glWidget->format();
}
#else
QGLFormat Z3DCanvas::format() const
{
  return m_glWidget->format();
}
#endif

#ifdef _QT4_
void Z3DCanvas::getGLFocus()
{
  m_glWidget->makeCurrent();
}
#endif

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
  m_glWidget->update();
}

void Z3DCanvas::enterEvent(QEvent* e)
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
  QGraphicsView::keyPressEvent(event);
  broadcastEvent(event, width(), height());
}

void Z3DCanvas::keyReleaseEvent(QKeyEvent* event)
{
  broadcastEvent(event, width(), height());
}

void Z3DCanvas::resizeEvent(QResizeEvent *event)
{
  getGLFocus();
  QGraphicsView::resizeEvent(event);
  if (m_3dScene)
    m_3dScene->setSceneRect(QRect(QPoint(0, 0), event->size()));

  emit canvasSizeChanged(event->size().width() * devicePixelRatio(),
                         event->size().height() * devicePixelRatio());
}

void Z3DCanvas::paintEvent(QPaintEvent *event)
{
  getGLFocus();
  QGraphicsView::paintEvent(event);
}

void Z3DCanvas::dragEnterEvent(QDragEnterEvent *event)
{
  event->ignore();
}

void Z3DCanvas::dropEvent(QDropEvent *event)
{
  event->ignore();
}

void Z3DCanvas::rotateX()
{
  getGLFocus();
  for (size_t i = 0 ; i < m_listeners.size() ; ++i) {
    m_listeners[i]->rotateX();
  }
}

void Z3DCanvas::rotateY()
{
  getGLFocus();
  for (size_t i = 0 ; i < m_listeners.size() ; ++i) {
    m_listeners[i]->rotateY();
  }
}

void Z3DCanvas::rotateZ()
{
  getGLFocus();
  for (size_t i = 0 ; i < m_listeners.size() ; ++i) {
    m_listeners[i]->rotateZ();
  }
}

void Z3DCanvas::rotateXM()
{
  getGLFocus();
  for (size_t i = 0 ; i < m_listeners.size() ; ++i) {
    m_listeners[i]->rotateXM();
  }
}

void Z3DCanvas::rotateYM()
{
  getGLFocus();
  for (size_t i = 0 ; i < m_listeners.size() ; ++i) {
    m_listeners[i]->rotateYM();
  }
}

void Z3DCanvas::rotateZM()
{
  getGLFocus();
  for (size_t i = 0 ; i < m_listeners.size() ; ++i) {
    m_listeners[i]->rotateZM();
  }
}

void Z3DCanvas::timerEvent(QTimerEvent* e)
{
  broadcastEvent(e, width(), height());
}

void Z3DCanvas::setNetworkEvaluator(Z3DNetworkEvaluator *n)
{
  m_3dScene->setNetworkEvaluator(n);
  if (n)
    n->setOpenGLContext(this);
}

void Z3DCanvas::setFakeStereoOnce()
{
  m_3dScene->setFakeStereoOnce();
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

  if (pos != m_listeners.end())
    m_listeners.erase(pos);
}

void Z3DCanvas::clearEventListeners()
{
  m_listeners.clear();
}

void Z3DCanvas::broadcastEvent(QEvent *e, int w, int h)
{
  getGLFocus();
  for (size_t i = 0 ; i < m_listeners.size() ; ++i) {
    m_listeners[i]->onEvent(e, w, h);
    if (e->isAccepted())
      break;
  }
}

double Z3DCanvas::devicePixelRatio()
{
#ifndef _QT4_
  return (window() && window()->windowHandle()) ?
        window()->windowHandle()->devicePixelRatio() : 1.0;
#else
  return 1.0;
#endif
}

#endif

} // namespace nim
