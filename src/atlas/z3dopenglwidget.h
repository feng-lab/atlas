#pragma once

#include <QOpenGLWidget>
#include <QOpenGLWindow>

namespace nim {

class ZOpenGLWidget : public QOpenGLWidget
{
Q_OBJECT
public:
  using QOpenGLWidget::QOpenGLWidget;

  // QOpenGLWidget interface
protected:
  void initializeGL() override;

Q_SIGNALS:
  void openGLContextInitialized();
};

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

} // namespace nim
