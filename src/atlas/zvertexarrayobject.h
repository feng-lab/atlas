#ifndef ZVERTEXARRAYOBJECT_H
#define ZVERTEXARRAYOBJECT_H

#include <vector>
#include "z3dgl.h"

namespace nim {

class ZVertexArrayObject
{
public:
  ZVertexArrayObject(GLsizei n = 1);

  ~ZVertexArrayObject();

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

#endif // ZVERTEXARRAYOBJECT_H
