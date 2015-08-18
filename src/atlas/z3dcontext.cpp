#include "z3dcontext.h"

#ifndef _QT4_
#include <QOpenGLContext>
#else
#include <QGLContext>
#endif
#include <cassert>

namespace nim {

Z3DContext::Z3DContext()
#ifndef _QT4_
  : m_context(QOpenGLContext::currentContext()->shareGroup())
#else
  : m_context(QGLContext::currentContext())
#endif
{
  assert(m_context);
}

Z3DContext::~Z3DContext()
{
}

bool Z3DContext::operator<(const Z3DContext &rhs) const
{
  return m_context < rhs.m_context;
}

bool Z3DContext::operator==(const Z3DContext &rhs) const
{
  return m_context == rhs.m_context;
}

bool Z3DContext::operator!=(const Z3DContext &rhs) const
{
  return m_context != rhs.m_context;
}

} // namespace nim
