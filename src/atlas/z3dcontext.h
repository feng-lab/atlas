#pragma once

#include <compare>

class QOpenGLContext;
class QOffscreenSurface;

namespace nim {

using ProcAddress = void (*)();

class Z3DContext
{
public:
  explicit Z3DContext(QOffscreenSurface& offscreenSurface, QOpenGLContext* sharedContext = nullptr);

  // note: Linux only
  Z3DContext();

  ~Z3DContext();

  // name may be null; returns nullptr in that case
  ProcAddress getProcAddress(/*nullable*/ const char* name) const;

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

  auto operator<=>(const Z3DContextGroup& rhs) const = default;

private:
  void* m_contextGroup = nullptr;
};

} // namespace nim
