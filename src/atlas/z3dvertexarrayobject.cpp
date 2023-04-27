#include "z3dvertexarrayobject.h"

#include "z3dgpuinfo.h"

#if defined(__APPLE__) && !defined(ATLAS_USE_CORE_PROFILE)
#undef glGenVertexArrays
#undef glBindVertexArray
#undef glDeleteVertexArrays
#define glGenVertexArrays glGenVertexArraysAPPLE
#define glBindVertexArray glBindVertexArrayAPPLE
#define glDeleteVertexArrays glDeleteVertexArraysAPPLE
#endif

namespace nim {

Z3DVertexArrayObject::Z3DVertexArrayObject(GLsizei n)
  : m_arrays(n, 0)
{
  CHECK(n > 0);
  glGenVertexArrays(m_arrays.size(), m_arrays.data());
}

Z3DVertexArrayObject::~Z3DVertexArrayObject()
{
#ifdef CHECK_OPENGL_ERROR_FOR_ALL_GL_CALLS
  CHECK(m_context == Z3DContext());
#endif
  glDeleteVertexArrays(m_arrays.size(), m_arrays.data());
}

void Z3DVertexArrayObject::bind(size_t idx) const
{
#ifdef CHECK_OPENGL_ERROR_FOR_ALL_GL_CALLS
  CHECK(m_context == Z3DContext());
#endif
  glBindVertexArray(m_arrays[idx]);
}

void Z3DVertexArrayObject::release() const
{
#ifdef CHECK_OPENGL_ERROR_FOR_ALL_GL_CALLS
  CHECK(m_context == Z3DContext());
#endif
  glBindVertexArray(0);
}

void Z3DVertexArrayObject::resize(GLsizei n)
{
  CHECK(n > 0);
#ifdef CHECK_OPENGL_ERROR_FOR_ALL_GL_CALLS
  CHECK(m_context == Z3DContext());
#endif
  if (n == GLsizei(m_arrays.size())) {
    return;
  }
  glDeleteVertexArrays(m_arrays.size(), m_arrays.data());
  m_arrays.resize(n, 0);
  glGenVertexArrays(m_arrays.size(), m_arrays.data());
}

} // namespace nim
