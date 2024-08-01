#pragma once

#include "z3dgl.h"
#include <vector>

namespace nim {

class Z3DVertexBufferObject
{
public:
  explicit Z3DVertexBufferObject(GLsizei n = 1)
    : m_arrays(n, 0)
  {
    CHECK(n > 0);
    glGenBuffers(m_arrays.size(), m_arrays.data());
  }

  ~Z3DVertexBufferObject()
  {
    glDeleteBuffers(m_arrays.size(), m_arrays.data());
  }

  Z3DVertexBufferObject(Z3DVertexBufferObject&&) = default;

  Z3DVertexBufferObject& operator=(Z3DVertexBufferObject&&) = default;

  Z3DVertexBufferObject(const Z3DVertexBufferObject&) = delete;

  Z3DVertexBufferObject& operator=(const Z3DVertexBufferObject&) = delete;

  void bind(GLenum target, size_t idx = 0)
  {
    glBindBuffer(target, m_arrays[idx]);
  }

  void release(GLenum target)
  {
    glBindBuffer(target, 0);
  }

  void resize(GLsizei n)
  {
    CHECK(n > 0);
    if (n == GLsizei(m_arrays.size())) {
      return;
    }
    glDeleteBuffers(m_arrays.size(), m_arrays.data());
    m_arrays.resize(n, 0);
    glGenBuffers(m_arrays.size(), m_arrays.data());
  }

private:
  std::vector<GLuint> m_arrays;
};

} // namespace nim
