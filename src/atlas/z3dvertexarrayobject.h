#pragma once

#include "z3dgl.h"
#ifdef CHECK_OPENGL_ERROR_FOR_ALL_GL_CALLS
#include "z3dcontext.h"
#endif
#include <vector>

#if defined(__APPLE__) && !defined(ATLAS_USE_CORE_PROFILE)
#undef glGenVertexArrays
#undef glBindVertexArray
#undef glDeleteVertexArrays
#define glGenVertexArrays glGenVertexArraysAPPLE
#define glBindVertexArray glBindVertexArrayAPPLE
#define glDeleteVertexArrays glDeleteVertexArraysAPPLE
#endif

namespace nim {

class Z3DVertexArrayObject
{
public:
  explicit Z3DVertexArrayObject(GLsizei n = 1)
    : m_arrays(n, 0)
  {
    CHECK(n > 0);
    glGenVertexArrays(m_arrays.size(), m_arrays.data());
  }

  ~Z3DVertexArrayObject()
  {
#ifdef CHECK_OPENGL_ERROR_FOR_ALL_GL_CALLS
    CHECK(m_context == Z3DContext());
#endif
    glDeleteVertexArrays(m_arrays.size(), m_arrays.data());
  }

  Z3DVertexArrayObject(Z3DVertexArrayObject&&) = default;

  Z3DVertexArrayObject& operator=(Z3DVertexArrayObject&&) = default;

  Z3DVertexArrayObject(const Z3DVertexArrayObject&) = delete;

  Z3DVertexArrayObject& operator=(const Z3DVertexArrayObject&) = delete;

  void bind(size_t idx = 0) const
  {
#ifdef CHECK_OPENGL_ERROR_FOR_ALL_GL_CALLS
    CHECK(m_context == Z3DContext());
#endif
    glBindVertexArray(m_arrays[idx]);
  }

  void release() const
  {
#ifdef CHECK_OPENGL_ERROR_FOR_ALL_GL_CALLS
    CHECK(m_context == Z3DContext());
#endif
    glBindVertexArray(0);
  }

  void resize(GLsizei n)
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

private:
  std::vector<GLuint> m_arrays;

#ifdef CHECK_OPENGL_ERROR_FOR_ALL_GL_CALLS
  Z3DContext m_context;
#endif
};

} // namespace nim
