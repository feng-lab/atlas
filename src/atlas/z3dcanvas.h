#pragma once

#include "zglmutils.h"
#include <QGraphicsView>
#include <QSurfaceFormat>

class QOpenGLWidget;

#include <QInputEvent>
#include <QShortcut>
#include <deque>

namespace nim {

class Z3DScene;

class Z3DNetworkEvaluator;

class Z3DCanvasEventListener;

#ifdef nothing

class Z3DCanvas : public QOpenGLWidget
{
  Q_OBJECT
public:
  Z3DCanvas(const QString &title, QWidget* parent = nullptr, Qt::WindowFlags f = 0);
  virtual ~Z3DCanvas();

  void setNetworkEvaluator(Z3DNetworkEvaluator *n);
  void setFakeStereoOnce();

  void addEventListenerToBack(Z3DCanvasEventListener* e);
  void addEventListenerToFront(Z3DCanvasEventListener* e);
  void removeEventListener(Z3DCanvasEventListener *e);
  void clearEventListeners();
  void broadcastEvent(QEvent* e, int w, int h);

  // Set the opengl context of this canvas as the current one.
  inline void getGLFocus() { makeCurrent(); }
  void toggleFullScreen();
  void forceUpdate() { auto pe = std::make_unique<QPaintEvent>(rect()); paintEvent(pe.get()); }
  void updateAll() { update(); }

  // for high dpi support like retina
  glm::ivec2 physicalSize() { return glm::ivec2(width() * devicePixelRatio(),
                                                height() * devicePixelRatio()); }
  glm::ivec2 logicalSize() { return glm::ivec2(width(), height()); }

signals:
  // w and h is physical size not logical size, opengl works in physical pixel
  void canvasSizeChanged(int w, int h);

protected:
  virtual void enterEvent(QEvent* e);
  virtual void leaveEvent(QEvent* e);
  virtual void mousePressEvent(QMouseEvent* e);
  virtual void mouseReleaseEvent (QMouseEvent* e);
  virtual void mouseMoveEvent(QMouseEvent*  e);
  virtual void mouseDoubleClickEvent(QMouseEvent* e);
  virtual void wheelEvent(QWheelEvent* e);
  virtual void timerEvent(QTimerEvent* e);

  virtual void keyPressEvent(QKeyEvent* event);
  virtual void keyReleaseEvent(QKeyEvent* event);

  virtual void resizeEvent(QResizeEvent *event);
  virtual void paintEvent(QPaintEvent *event);
  virtual void dragEnterEvent(QDragEnterEvent *event);
  virtual void dropEvent(QDropEvent *event);

  void rotateX();
  void rotateY();
  void rotateZ();
  void rotateXM();
  void rotateYM();
  void rotateZM();

  // QOpenGLWidget interface
protected:
  virtual void initializeGL();
  virtual void resizeGL(int w, int h);
  virtual void paintGL();

private:
  double devicePixelRatio();

private:
  bool m_fullscreen;

  std::deque<Z3DCanvasEventListener*> m_listeners;

  QShortcut* m_rotateXShortCut;
  QShortcut* m_rotateYShortCut;
  QShortcut* m_rotateZShortCut;
  QShortcut* m_rotateXMShortCut;
  QShortcut* m_rotateYMShortCut;
  QShortcut* m_rotateZMShortCut;

  Z3DNetworkEvaluator* m_networkEvaluator;
  bool m_isStereoScene;
  bool m_fakeStereoOnce;
};

#else

class Z3DCanvas : public QGraphicsView
{
Q_OBJECT
public:
  Z3DCanvas(const QString& title, int width, int height, QWidget* parent = nullptr, Qt::WindowFlags f = 0);

  virtual ~Z3DCanvas();

  QSurfaceFormat format() const;

  void setNetworkEvaluator(Z3DNetworkEvaluator* n);

  void setFakeStereoOnce();

  void addEventListenerToBack(Z3DCanvasEventListener* e);

  void addEventListenerToFront(Z3DCanvasEventListener* e);

  void removeEventListener(Z3DCanvasEventListener* e);

  void clearEventListeners();

  void broadcastEvent(QEvent* e, int w, int h);

  // Set the opengl context of this canvas as the current one.
  inline void getGLFocus()
  {}

  void toggleFullScreen();

  void forceUpdate()
  {
    auto pe = std::make_unique<QPaintEvent>(rect());
    paintEvent(pe.get());
  }

  void updateAll();

  // for high dpi support like retina
  glm::uvec2 physicalSize()
  {
    return glm::uvec2(width() * devicePixelRatio(),
                      height() * devicePixelRatio());
  }

  glm::uvec2 logicalSize()
  { return glm::uvec2(width(), height()); }

signals:

  // w and h is physical size not logical size, opengl works in physical pixel
  void canvasSizeChanged(size_t w, size_t h);

protected:
  virtual void enterEvent(QEvent* e) override;

  virtual void leaveEvent(QEvent* e) override;

  virtual void mousePressEvent(QMouseEvent* e) override;

  virtual void mouseReleaseEvent(QMouseEvent* e) override;

  virtual void mouseMoveEvent(QMouseEvent* e) override;

  virtual void mouseDoubleClickEvent(QMouseEvent* e) override;

  virtual void wheelEvent(QWheelEvent* e) override;

  virtual void timerEvent(QTimerEvent* e) override;

  virtual void keyPressEvent(QKeyEvent* event) override;

  virtual void keyReleaseEvent(QKeyEvent* event) override;

  virtual void resizeEvent(QResizeEvent* event) override;

  virtual void paintEvent(QPaintEvent* event) override;

  virtual void dragEnterEvent(QDragEnterEvent* event) override;

  virtual void dropEvent(QDropEvent* event) override;

  void setCursor(const QCursor& c)
  { viewport()->setCursor(c); }

  void rotateX();

  void rotateY();

  void rotateZ();

  void rotateXM();

  void rotateYM();

  void rotateZM();

private:
  double devicePixelRatio();

private:
  bool m_fullscreen;

  QOpenGLWidget* m_glWidget;
  Z3DScene* m_3dScene;
  std::deque<Z3DCanvasEventListener*> m_listeners;

  QShortcut* m_rotateXShortCut;
  QShortcut* m_rotateYShortCut;
  QShortcut* m_rotateZShortCut;
  QShortcut* m_rotateXMShortCut;
  QShortcut* m_rotateYMShortCut;
  QShortcut* m_rotateZMShortCut;
};

#endif

} // namespace nim

