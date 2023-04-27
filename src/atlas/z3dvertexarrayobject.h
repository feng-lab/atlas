#pragma once

#include "z3dgl.h"
#include "z3dcontext.h"
#include <vector>

namespace nim {

class Z3DVertexArrayObject
{
public:
  explicit Z3DVertexArrayObject(GLsizei n = 1);

  ~Z3DVertexArrayObject();

  Z3DVertexArrayObject(Z3DVertexArrayObject&&) = default;

  Z3DVertexArrayObject& operator=(Z3DVertexArrayObject&&) = default;

  Z3DVertexArrayObject(const Z3DVertexArrayObject&) = delete;

  Z3DVertexArrayObject& operator=(const Z3DVertexArrayObject&) = delete;

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
