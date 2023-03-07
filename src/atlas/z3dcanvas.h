#pragma once

// #define ATLAS_USE_OPENGLWINDOW
// #define ATLAS_USE_OPENGLWIDGET

#include "zglmutils.h"
#include <QGraphicsView>
#include <QSurfaceFormat>
#include <QInputEvent>
#include <QShortcut>
#ifdef ATLAS_USE_OPENGLWIDGET
#include <QOpenGLWidget>
#endif

#include <memory>

namespace nim {

class Z3DScene;

class Z3DRenderingEngine;

class ZOpenGLWidget;

class ZOpenGLWindow;

#ifdef ATLAS_USE_OPENGLWIDGET

class Z3DCanvas : public QOpenGLWidget
{
  Q_OBJECT

public:
  Z3DCanvas(const QString& title,
            int width,
            int height,
            QWidget* parent = nullptr,
            Qt::WindowFlags f = Qt::WindowFlags());

  void setNetworkEvaluator(Z3DNetworkEvaluator* n);

  void setFakeStereoOnce();

  void addEventListenerToBack(Z3DCanvasEventListener* e);

  void addEventListenerToFront(Z3DCanvasEventListener* e);

  void removeEventListener(Z3DCanvasEventListener* e);

  void clearEventListeners();

  void broadcastEvent(QEvent* e, int w, int h);

  // Set the opengl context of this canvas as the current one.
  inline void getGLFocus()
  {
    makeCurrent();
  }

  void toggleFullScreen();

  void forceUpdate()
  {
    auto pe = std::make_unique<QPaintEvent>(rect());
    paintEvent(pe.get());
  }

  void updateAll()
  {
    update();
  }

  // for high dpi support like retina
  glm::ivec2 physicalSize()
  {
    return glm::ivec2(width() * devicePixelRatioF(), height() * devicePixelRatioF());
  }

  glm::ivec2 logicalSize()
  {
    return glm::ivec2(width(), height());
  }

Q_SIGNALS:

  // w and h is physical size not logical size, opengl works in physical pixel
  void canvasSizeChanged(int w, int h);

  void openGLContextInitialized();

protected:
  void enterEvent(QEnterEvent* e) override;

  void leaveEvent(QEvent* e) override;

  void mousePressEvent(QMouseEvent* e) override;

  void mouseReleaseEvent(QMouseEvent* e) override;

  void mouseMoveEvent(QMouseEvent* e) override;

  void mouseDoubleClickEvent(QMouseEvent* e) override;

  void wheelEvent(QWheelEvent* e) override;

  void timerEvent(QTimerEvent* e) override;

  void keyPressEvent(QKeyEvent* event) override;

  void keyReleaseEvent(QKeyEvent* event) override;

  void resizeEvent(QResizeEvent* event) override;

  void paintEvent(QPaintEvent* event) override;

  void dragEnterEvent(QDragEnterEvent* event) override;

  void dropEvent(QDropEvent* event) override;

  void rotateX();

  void rotateY();

  void rotateZ();

  void rotateXM();

  void rotateYM();

  void rotateZM();

  // QOpenGLWidget interface

protected:
  void initializeGL() override;

  void resizeGL(int w, int h) override;

  void paintGL() override;

private:
  bool m_fullscreen = false;

  std::deque<Z3DCanvasEventListener*> m_listeners;

  QShortcut* m_rotateXShortCut = nullptr;
  QShortcut* m_rotateYShortCut = nullptr;
  QShortcut* m_rotateZShortCut = nullptr;
  QShortcut* m_rotateXMShortCut = nullptr;
  QShortcut* m_rotateYMShortCut = nullptr;
  QShortcut* m_rotateZMShortCut = nullptr;

  Z3DNetworkEvaluator* m_networkEvaluator = nullptr;
  bool m_isStereoScene = false;
  bool m_fakeStereoOnce = false;
};

#else

class Z3DCanvas : public QGraphicsView
{
  Q_OBJECT

public:
  Z3DCanvas(const QString& title,
            int width,
            int height,
            QWidget* parent = nullptr,
            Qt::WindowFlags f = Qt::WindowFlags());

  [[nodiscard]] QSurfaceFormat format() const;

  QOpenGLContext* context() const;

  void setRenderingEngine(Z3DRenderingEngine* engine);

  // Set the opengl context of this canvas as the current one.
  void getGLFocus();

  void toggleFullScreen();

  void forceUpdate()
  {
    auto pe = std::make_unique<QPaintEvent>(rect());
    paintEvent(pe.get());
  }

  void sceneParaUpdated();

  void renderingFinished();

  void updateAll();

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

  void openGLContextInitialized();

  void rotateX();

  void rotateY();

  void rotateZ();

  void rotateXM();

  void rotateYM();

  void rotateZM();

protected:
  void contextMenuEvent(QContextMenuEvent* event) override;

  void enterEvent(QEnterEvent* e) override;

  void leaveEvent(QEvent* e) override;

  void mousePressEvent(QMouseEvent* e) override;

  void mouseReleaseEvent(QMouseEvent* e) override;

  void mouseMoveEvent(QMouseEvent* e) override;

  void mouseDoubleClickEvent(QMouseEvent* e) override;

  void wheelEvent(QWheelEvent* e) override;

  void timerEvent(QTimerEvent* e) override;

  void keyPressEvent(QKeyEvent* event) override;

  void keyReleaseEvent(QKeyEvent* event) override;

  void resizeEvent(QResizeEvent* event) override;

  void paintEvent(QPaintEvent* event) override;

  void dragEnterEvent(QDragEnterEvent* event) override;

  void dropEvent(QDropEvent* event) override;

  //  void setCursor(const QCursor& c)
  //  { viewport()->setCursor(c); }

private:
  // double devicePixelRatio();

private:
  bool m_fullscreen = false;

  ZOpenGLWidget* m_glWidget = nullptr;
  ZOpenGLWindow* m_glWindow = nullptr;
  Z3DScene* m_3dScene = nullptr;

  QShortcut* m_rotateXShortCut = nullptr;
  QShortcut* m_rotateYShortCut = nullptr;
  QShortcut* m_rotateZShortCut = nullptr;
  QShortcut* m_rotateXMShortCut = nullptr;
  QShortcut* m_rotateYMShortCut = nullptr;
  QShortcut* m_rotateZMShortCut = nullptr;

  Z3DRenderingEngine* m_engine = nullptr;

  bool m_renderingFinished = false;
};

#endif

} // namespace nim
