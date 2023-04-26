#pragma once

#include "z3dgl.h"
#include "z3dcontext.h"
#include <vector>

namespace nim {

class ZVertexArrayObject
{
public:
  explicit ZVertexArrayObject(GLsizei n = 1);

  ~ZVertexArrayObject();

  ZVertexArrayObject(ZVertexArrayObject&&) = default;

  ZVertexArrayObject& operator=(ZVertexArrayObject&&) = default;

  ZVertexArrayObject(const ZVertexArrayObject&) = default;

  ZVertexArrayObject& operator=(const ZVertexArrayObject&) = default;

  void bind(size_t idx = 0) const;

  void release() const;

  void resize(GLsizei n);

private:
  std::vector<GLuint> m_arrays;

#ifdef CHECK_OPENGL_ERROR_FOR_ALL_GL_CALLS
  Z3DContext m_context;
#endif
};

} // namespace nim
