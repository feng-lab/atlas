#include "z3dopenglwidget.h"

namespace nim {

void ZOpenGLWidget::initializeGL()
{
  Q_EMIT openGLContextInitialized();
}

void ZOpenGLWindow::initializeGL()
{
  Q_EMIT openGLContextInitialized();
}

} // namespace nim

