#include "z3dopenglwidget.h"

namespace nim {

void ZOpenGLWidget::initializeGL()
{
  emit openGLContextInitialized();
}

void ZOpenGLWindow::initializeGL()
{
  emit openGLContextInitialized();
}

} // namespace nim

