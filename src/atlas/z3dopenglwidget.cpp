#include "z3dopenglwidget.h"

#if defined(ATLAS_USE_OPENGLWIDGET)

#include "zlog.h"

namespace nim {

ZOpenGLWidget::~ZOpenGLWidget()
{
  VLOG(1) << "in OpenGLWidget destructor";
}

void ZOpenGLWidget::initializeGL()
{
  Q_EMIT openGLContextInitialized();
}

void ZOpenGLWidget::paintGL()
{
  VLOG(1) << "paint GL";
}

} // namespace nim

#endif
