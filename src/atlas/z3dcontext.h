#pragma once

class QOpenGLContext;
class QOffscreenSurface;
class QOpenGLContextGroup;

namespace nim {

using ProcAddress = void (*)();

class Z3DContext
{
public:
  Z3DContext(QOffscreenSurface& offscreenSurface, QOpenGLContext* sharedContext = nullptr);

  ~Z3DContext();

  QOpenGLContext* context() const
  {
    return m_context;
  }

  static void logCurrentContext();

  ProcAddress getProcAddress(const char* name) const;

  void makeCurrent() const;

private:
  QOpenGLContext* m_context = nullptr;
  QOffscreenSurface& m_offscreenSurface;
};

class Z3DContextGroup
{
public:
  Z3DContextGroup();

  Z3DContextGroup(const Z3DContextGroup&) = default;

  Z3DContextGroup& operator=(const Z3DContextGroup&) = default;

  bool operator<(const Z3DContextGroup& rhs) const;

  bool operator==(const Z3DContextGroup& rhs) const;

  bool operator!=(const Z3DContextGroup& rhs) const;

private:
  QOpenGLContextGroup* m_contextGroup;
};

} // namespace nim
