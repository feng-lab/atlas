#pragma once

#if defined(ATLAS_USE_OPENGLWIDGET)
#include <QOpenGLWidget>
#endif
#if defined(ATLAS_USE_OPENGLWINDOW)
#include <QOpenGLWindow>
#endif

namespace nim {

#if defined(ATLAS_USE_OPENGLWIDGET)

class ZOpenGLWidget : public QOpenGLWidget
{
  Q_OBJECT

public:
  using QOpenGLWidget::QOpenGLWidget;

  ~ZOpenGLWidget() override;

  // QOpenGLWidget interface
protected:
  void initializeGL() override;

  void paintGL() override;

Q_SIGNALS:
  void openGLContextInitialized();
};

#endif

#if defined(ATLAS_USE_OPENGLWINDOW)

class ZOpenGLWindow : public QOpenGLWindow
{
  Q_OBJECT

public:
  using QOpenGLWindow::QOpenGLWindow;

  // QOpenGLWidget interface

protected:
  void initializeGL() override;

Q_SIGNALS:
  void openGLContextInitialized();
};

#endif

} // namespace nim
