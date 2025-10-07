#include "z3dcanvas.h"

#include "zlog.h"
#include "z3drenderingengine.h"
#if defined(ATLAS_USE_OPENGLWIDGET)
#include "z3dscene.h"
#include "z3dopenglwidget.h"
#endif
#include <QCoreApplication>
#include <algorithm>

DECLARE_bool(atlas_vk_copy_yflip_in_shader);

namespace nim {

Z3DCanvas::Z3DCanvas(const QString& title, int width, int height, QWidget* parent, Qt::WindowFlags f)
  : QGraphicsView(parent)
{
  setAlignment(Qt::AlignLeft | Qt::AlignTop);
  resize(width, height);

#if defined(ATLAS_USE_OPENGLWIDGET)
  m_glWidget = new ZOpenGLWidget(this, f);
  m_3dScene = std::make_unique<Z3DScene>(width, height, m_glWidget->format().stereo(), *this);
  setViewport(m_glWidget);
  setScene(m_3dScene.get());
#else
  Q_UNUSED(f)
  m_scene = std::make_unique<QGraphicsScene>(0, 0, width, height);
  m_pixmapItem = new QGraphicsPixmapItem();
  // m_pixmapItem->setTransformationMode(Qt::SmoothTransformation);
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

#if defined(ATLAS_USE_OPENGLWIDGET)
  connect(m_glWidget, &ZOpenGLWidget::openGLContextInitialized, this, &Z3DCanvas::openGLContextInitialized);
  connect(m_glWidget, &ZOpenGLWidget::openGLContextInitialized, m_3dScene.get(), &Z3DScene::initPainter);
#endif
}

#ifdef ATLAS_USE_OPENGLWIDGET
Z3DCanvas::~Z3DCanvas()
{
  VLOG(1) << "in canvas destructor";
  getGLFocus();
}

QOpenGLContext* Z3DCanvas::context() const
{
  return m_glWidget->context();
}

void Z3DCanvas::getGLFocus()
{
  m_glWidget->makeCurrent();
}
#endif

void Z3DCanvas::setRenderingEngine(Z3DRenderingEngine* engine)
{
  m_engine = engine;
#if defined(ATLAS_USE_OPENGLWIDGET)
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
  // Engine may have been detached/destroyed while a queued signal is in flight
  if (m_engine && m_engine->hasNewRenderingFlag()) {
    VLOG(1) << "update";

#if defined(ATLAS_USE_OPENGLWIDGET)
    m_glWidget->update();
#else
    const std::scoped_lock lock(m_engine->targetSwitchMutex());
    auto localBuffer = m_engine->monoReadyLocalBuffer();
    QImage image;
    const bool vulkanZeroCopy = localBuffer->external && localBuffer->width > 0 && localBuffer->height > 0;
    if (vulkanZeroCopy) {
      // Vulkan zero-copy path: mapped RGBA8 staging
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
      image =
        QImage(localBuffer->external,
               static_cast<int>(localBuffer->width),
               static_cast<int>(localBuffer->height),
               static_cast<int>(localBuffer->externalStride ? localBuffer->externalStride : localBuffer->width * 4),
               QImage::Format_RGBA8888_Premultiplied);
#else
      image =
        QImage(localBuffer->external,
               static_cast<int>(localBuffer->width),
               static_cast<int>(localBuffer->height),
               static_cast<int>(localBuffer->externalStride ? localBuffer->externalStride : localBuffer->width * 4),
               QImage::Format_RGB32); // Fallback; older Qt may lack RGBA8888
#endif
    } else {
      image = QImage(localBuffer->data.data(),
                     static_cast<int>(localBuffer->width),
                     static_cast<int>(localBuffer->height),
                     QImage::Format_ARGB32_Premultiplied);
    }
    // If shader y-flip is enabled for Vulkan path, present directly; otherwise flip vertically.
    auto pixmap =
      QPixmap::fromImage((vulkanZeroCopy && FLAGS_atlas_vk_copy_yflip_in_shader) ? image : image.flipped(Qt::Vertical));
    pixmap.setDevicePixelRatio(devicePixelRatio());
    m_pixmapItem->setPixmap(pixmap);
    if (m_engine) {
      m_engine->clearNewRenderingFlag();
    }
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
  // VLOG(1) << "mousemoveevent";
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
  // VLOG(1) << "wheelevent";
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
#if defined(ATLAS_USE_OPENGLWIDGET)
  if (m_3dScene) {
    m_3dScene->setSceneRect(QRect(QPoint(0, 0), event->size()));
  }
#else
  CHECK(m_scene);
  m_scene->setSceneRect(QRect(QPoint(0, 0), event->size()));
#endif

  // VLOG(1) << devicePixelRatio() << " " << event->size() << " " << logicalDpiX() << " " << physicalDpiX();
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

} // namespace nim
