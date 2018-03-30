#include "z3dcontext.h"

#include "zlog.h"
#include <QOpenGLContext>

namespace nim {

Z3DContext::Z3DContext()
  : m_context(QOpenGLContext::currentContext())
{
  CHECK(m_context);
}

bool Z3DContext::operator<(const Z3DContext& rhs) const
{
  return m_context < rhs.m_context;
}

bool Z3DContext::operator==(const Z3DContext& rhs) const
{
  return m_context == rhs.m_context;
}

bool Z3DContext::operator!=(const Z3DContext& rhs) const
{
  return m_context != rhs.m_context;
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

  const auto qtSymbol = QByteArray::fromStdString(name);
  return m_context->getProcAddress(qtSymbol);
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
