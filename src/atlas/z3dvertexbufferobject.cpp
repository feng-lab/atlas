#include "z3dvertexbufferobject.h"

namespace nim {

Z3DVertexBufferObject::Z3DVertexBufferObject(GLsizei n)
  : m_arrays(n, 0)
{
  CHECK(n > 0);
  glGenBuffers(m_arrays.size(), m_arrays.data());
}

Z3DVertexBufferObject::~Z3DVertexBufferObject()
{
  glDeleteBuffers(m_arrays.size(), m_arrays.data());
}

void Z3DVertexBufferObject::bind(GLenum target, size_t idx)
{
  glBindBuffer(target, m_arrays[idx]);
}

void Z3DVertexBufferObject::release(GLenum target)
{
  glBindBuffer(target, 0);
}

void Z3DVertexBufferObject::resize(GLsizei n)
{
  CHECK(n > 0);
  if (n == GLsizei(m_arrays.size())) {
    return;
  }
  glDeleteBuffers(m_arrays.size(), m_arrays.data());
  m_arrays.resize(n, 0);
  glGenBuffers(m_arrays.size(), m_arrays.data());
}

} // namespace nim
