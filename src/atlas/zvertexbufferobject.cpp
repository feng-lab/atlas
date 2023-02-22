#include "zvertexbufferobject.h"

namespace nim {

ZVertexBufferObject::ZVertexBufferObject(GLsizei n)
  : m_arrays(n, 0)
{
  CHECK(n > 0);
  glGenBuffers(m_arrays.size(), m_arrays.data());
}

ZVertexBufferObject::~ZVertexBufferObject()
{
  glDeleteBuffers(m_arrays.size(), m_arrays.data());
}

void ZVertexBufferObject::bind(GLenum target, size_t idx)
{
  glBindBuffer(target, m_arrays[idx]);
}

void ZVertexBufferObject::release(GLenum target)
{
  glBindBuffer(target, 0);
}

void ZVertexBufferObject::resize(GLsizei n)
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
