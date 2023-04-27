#pragma once

#include "z3dgl.h"
#include <vector>

namespace nim {

class Z3DVertexBufferObject
{
public:
  explicit Z3DVertexBufferObject(GLsizei n = 1);

  ~Z3DVertexBufferObject();

  Z3DVertexBufferObject(Z3DVertexBufferObject&&) = default;

  Z3DVertexBufferObject& operator=(Z3DVertexBufferObject&&) = default;

  Z3DVertexBufferObject(const Z3DVertexBufferObject&) = delete;

  Z3DVertexBufferObject& operator=(const Z3DVertexBufferObject&) = delete;

  void bind(GLenum target, size_t idx = 0);

  void release(GLenum target);

  void resize(GLsizei n);

private:
  std::vector<GLuint> m_arrays;
};

} // namespace nim
