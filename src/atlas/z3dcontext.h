#ifndef Z3DCONTEXT_H
#define Z3DCONTEXT_H

#ifndef _QT4_
class QOpenGLContextGroup;
#else
class QGLContext;
#endif

namespace nim {

class Z3DContext
{
public:
  Z3DContext();
  ~Z3DContext();

  Z3DContext(const Z3DContext&) = default;
  Z3DContext& operator=(const Z3DContext&) = default;

  bool operator<(const Z3DContext& rhs) const;
  bool operator==(const Z3DContext& rhs) const;
  bool operator!=(const Z3DContext& rhs) const;

private:
#ifndef _QT4_
  QOpenGLContextGroup *m_context;
#else
  QGLContext *m_context;
#endif
};

} // namespace nim

#endif // Z3DCONTEXT_H
