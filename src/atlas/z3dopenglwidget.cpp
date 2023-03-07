#include "z3dopenglwidget.h"

#include "zlog.h"

namespace nim {

void ZOpenGLWidget::initializeGL()
{
  Q_EMIT openGLContextInitialized();
}

void ZOpenGLWidget::paintGL()
{
  LOG(INFO) << "paint GL";
}

void ZOpenGLWindow::initializeGL()
{
  Q_EMIT openGLContextInitialized();
}

} // namespace nim
