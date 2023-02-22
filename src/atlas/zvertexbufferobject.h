#pragma once

#include "z3dgl.h"
#include <vector>

namespace nim {

class ZVertexBufferObject
{
public:
  explicit ZVertexBufferObject(GLsizei n = 1);

  ~ZVertexBufferObject();

  ZVertexBufferObject(ZVertexBufferObject&&) = default;

  ZVertexBufferObject& operator=(ZVertexBufferObject&&) = default;

  ZVertexBufferObject(const ZVertexBufferObject&) = default;

  ZVertexBufferObject& operator=(const ZVertexBufferObject&) = default;

  void bind(GLenum target, size_t idx = 0);

  void release(GLenum target);

  void resize(GLsizei n);

private:
  std::vector<GLuint> m_arrays;
};

} // namespace nim
