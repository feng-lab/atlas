#pragma once

#if defined(ATLAS_USE_OPENGLWIDGET)

#include <QOpenGLWidget>

namespace nim {

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

} // namespace nim

#endif
