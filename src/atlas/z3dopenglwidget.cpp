#include "z3dopenglwidget.h"

#include "zlog.h"

namespace nim {

#if defined(ATLAS_USE_OPENGLWIDGET)

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

#endif

#if defined(ATLAS_USE_OPENGLWINDOW)

void ZOpenGLWindow::initializeGL()
{
  Q_EMIT openGLContextInitialized();
}

#endif

} // namespace nim
