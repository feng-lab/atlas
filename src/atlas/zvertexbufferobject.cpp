#include "zvertexbufferobject.h"

namespace nim {

ZVertexBufferObject::ZVertexBufferObject(GLsizei n)
  : m_arrays(std::max((GLsizei)0, n), 0)
{
  glGenBuffers(m_arrays.size(), &m_arrays[0]);
}

ZVertexBufferObject::~ZVertexBufferObject()
{
  glDeleteBuffers(m_arrays.size(), &m_arrays[0]);
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
  if (n == (GLsizei)m_arrays.size())
    return;
  glDeleteBuffers(m_arrays.size(), &m_arrays[0]);
  m_arrays.resize(std::max((GLsizei)0, n), 0);
  glGenBuffers(m_arrays.size(), &m_arrays[0]);
}

} // namespace nim
