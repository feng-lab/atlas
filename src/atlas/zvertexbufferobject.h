#pragma once

#include "z3dgl.h"
#include <vector>

namespace nim {

class ZVertexBufferObject
{
public:
  ZVertexBufferObject(GLsizei n = 1);

  ~ZVertexBufferObject();

  void bind(GLenum target, size_t idx = 0);

  void release(GLenum target);

  void resize(GLsizei n);

  inline void clear()
  { resize(0); }

private:
  std::vector<GLuint> m_arrays;
};

} // namespace nim

