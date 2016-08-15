#pragma once

#include <vector>
#include "z3dgl.h"

namespace nim {

class ZVertexArrayObject
{
public:
  ZVertexArrayObject(GLsizei n = 1);

  ~ZVertexArrayObject();

  ZVertexArrayObject(ZVertexArrayObject&&) = default;

  ZVertexArrayObject& operator=(ZVertexArrayObject&&) = default;

  ZVertexArrayObject(const ZVertexArrayObject&) = default;

  ZVertexArrayObject& operator=(const ZVertexArrayObject&) = default;

  void bind(size_t idx = 0) const;

  void release() const;

  void resize(GLsizei n);

  inline void clear()
  { resize(0); }

private:
  bool m_hardwareSupportVAO;
  std::vector<GLuint> m_arrays;
};

} // namespace nim

