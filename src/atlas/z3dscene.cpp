#include "z3dscene.h"

#if defined(ATLAS_USE_OPENGLWIDGET)

#include "z3dgl.h"
#include "z3dcanvas.h"
#include "z3dcanvaspainter.h"
#include <glbinding/glbinding.h>
#include <glbinding-aux/Meta.h>
#include <QOpenGLContext>

namespace nim {

Z3DScene::Z3DScene(int width, int height, bool stereo, Z3DCanvas& canvas)
  : QGraphicsScene(0, 0, width, height)
  , m_isStereoScene(stereo)
  , m_canvas(canvas)
{}

void Z3DScene::drawBackground(QPainter* /*painter*/, const QRectF& /*rect*/)
{
  VLOG(1) << "draw background";
  m_canvas.getGLFocus();
  // glbinding::useContext(1);
  m_painter->paint(m_isStereoScene);
}

void Z3DScene::initPainter()
{
  m_canvas.getGLFocus();
  glbinding::initialize(1, [](const char* name) {
    return QOpenGLContext::currentContext()->getProcAddress(name);
  });
  glbinding::useContext(1);
  glbinding::setCallbackMaskExcept(glbinding::CallbackMask::After |
                                     glbinding::CallbackMask::ParametersAndReturnValue |
                                     glbinding::CallbackMask::Unresolved,
                                   {"glGetError"});
  glbinding::setAfterCallback([](const glbinding::FunctionCall& call) {
    gl::GLenum error = gl::glGetError();
    if (error != GL_NO_ERROR) {
      std::ostringstream os;

      os << call.function->name() << "(";
      for (size_t i = 0; i < call.parameters.size(); ++i) {
        os << call.parameters[i].get();
        if (i + 1 < call.parameters.size()) {
          os << ", ";
        }
      }
      os << ")";

      if (call.returnValue) {
        os << " -> " << call.returnValue.get();
      }

      LOG(ERROR) << "OpenGL error: " << glbinding::aux::Meta::getString(error) << " with " << os.str();
    }
  });
  glbinding::setUnresolvedCallback([](const glbinding::AbstractFunction& call) {
    LOG(ERROR) << "OpenGL function " << call.name() << " can not be resolved.";
  });

  m_painter = std::make_shared<Z3DCanvasPainter>(m_canvas);
  LOG(INFO) << "painter inited";
}

void Z3DScene::setRenderingEngine(Z3DRenderingEngine* engine)
{
  m_painter->setRenderingEngine(engine);
}

} // namespace nim

#endif
