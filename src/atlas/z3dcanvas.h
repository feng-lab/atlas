#pragma once

// #define ATLAS_USE_OPENGLWIDGET
// #define ATLAS_USE_OPENGLWINDOW

#include "zglmutils.h"
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QSurfaceFormat>
#include <QInputEvent>
#include <QShortcut>

#include <memory>

namespace nim {

class Z3DRenderingEngine;

#ifdef ATLAS_USE_OPENGLWIDGET
class ZOpenGLWidget;
class Z3DScene;
#endif
#ifdef ATLAS_USE_OPENGLWINDOW
class ZOpenGLWindow;
class Z3DScene;
#endif

class Z3DCanvas : public QGraphicsView
{
  Q_OBJECT

public:
  Z3DCanvas(const QString& title,
            int width,
            int height,
            QWidget* parent = nullptr,
            Qt::WindowFlags f = Qt::WindowFlags());

  ~Z3DCanvas() override;

  QOpenGLContext* context() const;

  void setRenderingEngine(Z3DRenderingEngine* engine);

  // Set the opengl context of this canvas as the current one.
  void getGLFocus();

  void toggleFullScreen();

  void sceneParaUpdated();

  void renderingFinished();

  // for high dpi support like retina
  glm::uvec2 physicalSize()
  {
    return glm::uvec2(width() * devicePixelRatio(), height() * devicePixelRatio());
  }

  glm::uvec2 logicalSize()
  {
    return glm::uvec2(width(), height());
  }

Q_SIGNALS:
  // w and h is physical size not logical size, opengl works in physical pixel
  void canvasSizeChanged(size_t w, size_t h);

#if defined(ATLAS_USE_OPENGLWIDGET) || defined(ATLAS_USE_OPENGLWINDOW)
  void openGLContextInitialized();
#endif

  void rotateX();

  void rotateY();

  void rotateZ();

  void rotateXM();

  void rotateYM();

  void rotateZM();

protected:
  void contextMenuEvent(QContextMenuEvent* e) override;

  //  void enterEvent(QEnterEvent* e) override;
  //
  //  void leaveEvent(QEvent* e) override;

  void mousePressEvent(QMouseEvent* e) override;

  void mouseReleaseEvent(QMouseEvent* e) override;

  void mouseMoveEvent(QMouseEvent* e) override;

  void mouseDoubleClickEvent(QMouseEvent* e) override;

  void wheelEvent(QWheelEvent* e) override;

  void timerEvent(QTimerEvent* e) override;

  void keyPressEvent(QKeyEvent* e) override;

  void keyReleaseEvent(QKeyEvent* e) override;

  void resizeEvent(QResizeEvent* e) override;

  void dragEnterEvent(QDragEnterEvent* e) override;

  void dropEvent(QDropEvent* e) override;

  //  void setCursor(const QCursor& c)
  //  { viewport()->setCursor(c); }

private:
  // double devicePixelRatio();

private:
  bool m_fullscreen = false;

#ifdef ATLAS_USE_OPENGLWIDGET
  ZOpenGLWidget* m_glWidget = nullptr;
  std::unique_ptr<Z3DScene> m_3dScene;
#elif defined(ATLAS_USE_OPENGLWINDOW)
  ZOpenGLWindow* m_glWindow = nullptr;
  std::unique_ptr<Z3DScene> m_3dScene;
#else
  std::unique_ptr<QGraphicsScene> m_scene;
  QGraphicsPixmapItem* m_pixmapItem = nullptr;
#endif

  QShortcut* m_rotateXShortCut = nullptr;
  QShortcut* m_rotateYShortCut = nullptr;
  QShortcut* m_rotateZShortCut = nullptr;
  QShortcut* m_rotateXMShortCut = nullptr;
  QShortcut* m_rotateYMShortCut = nullptr;
  QShortcut* m_rotateZMShortCut = nullptr;

  Z3DRenderingEngine* m_engine = nullptr;
};

} // namespace nim
