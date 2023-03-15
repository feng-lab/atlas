#include "z3dcontext.h"

#include "zlog.h"
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QOffscreenSurface>

namespace nim {

Z3DContext::Z3DContext(QOffscreenSurface& offscreenSurface, QOpenGLContext* sharedContext)
: m_offscreenSurface(offscreenSurface)
{
  m_context = new QOpenGLContext();
  m_context->setFormat(QSurfaceFormat::defaultFormat());
  if (sharedContext) {
    m_context->setShareContext(sharedContext);
  }
  m_context->create();
  if (!m_context->isValid()) {
    LOG(ERROR) << "Can not create OpenGL context";
  }
}

Z3DContext::~Z3DContext()
{
  delete m_context;
}

void Z3DContext::logCurrentContext()
{
  LOG(INFO) << QOpenGLContext::currentContext();
}

ProcAddress Z3DContext::getProcAddress(const char* name) const
{
  if (!name) {
    return nullptr;
  }

  return m_context->getProcAddress(name);
}

void Z3DContext::makeCurrent() const
{
  m_context->makeCurrent(&m_offscreenSurface);
}

Z3DContextGroup::Z3DContextGroup()
  : m_contextGroup(QOpenGLContext::currentContext()->shareGroup())
{
  CHECK(m_contextGroup);
}

bool Z3DContextGroup::operator<(const Z3DContextGroup& rhs) const
{
  return m_contextGroup < rhs.m_contextGroup;
}

bool Z3DContextGroup::operator==(const Z3DContextGroup& rhs) const
{
  return m_contextGroup == rhs.m_contextGroup;
}

bool Z3DContextGroup::operator!=(const Z3DContextGroup& rhs) const
{
  return m_contextGroup != rhs.m_contextGroup;
}

} // namespace nim
