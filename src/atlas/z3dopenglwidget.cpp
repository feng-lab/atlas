#include "z3dopenglwidget.h"

#include "zlog.h"

namespace nim {

ZOpenGLWidget::~ZOpenGLWidget()
{
  LOG(INFO) << "in OpenGLWidget destructor";
}

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
