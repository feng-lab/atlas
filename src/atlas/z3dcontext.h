#pragma once

class QOpenGLContext;
class QOffscreenSurface;

namespace nim {

using ProcAddress = void (*)();

class Z3DContext
{
public:
  explicit Z3DContext(QOffscreenSurface& offscreenSurface, QOpenGLContext* sharedContext = nullptr);

#if defined(__linux__)
  Z3DContext();
#endif

  ~Z3DContext();

  ProcAddress getProcAddress(const char* name) const;

  void makeCurrent() const;

private:
  QOpenGLContext* m_context = nullptr;
  QOffscreenSurface* m_offscreenSurface = nullptr;
#if defined(__linux__)
  void* m_eglContext = nullptr;
  void* m_eglDisplay = nullptr;
#endif
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
  void* m_contextGroup = nullptr;
};

} // namespace nim
