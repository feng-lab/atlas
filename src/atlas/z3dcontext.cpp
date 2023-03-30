#include "z3dcontext.h"

#include "zlog.h"
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QOffscreenSurface>

#if defined(__linux__)
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

DEFINE_bool(__use_EGL, false, "use EGL in linux console mode");

static const EGLint configAttribs[] = {EGL_SURFACE_TYPE,
                                       EGL_PBUFFER_BIT,
                                       EGL_BLUE_SIZE,
                                       8,
                                       EGL_GREEN_SIZE,
                                       8,
                                       EGL_RED_SIZE,
                                       8,
                                       EGL_DEPTH_SIZE,
                                       8,
                                       EGL_RENDERABLE_TYPE,
                                       EGL_OPENGL_BIT,
                                       EGL_NONE};
#endif

namespace nim {

Z3DContext::Z3DContext(QOffscreenSurface& offscreenSurface, QOpenGLContext* sharedContext)
  : m_offscreenSurface(&offscreenSurface)
{
  m_context = new QOpenGLContext();
  m_context->setFormat(QSurfaceFormat::defaultFormat());
  if (sharedContext) {
    m_context->setShareContext(sharedContext);
  }
  m_context->create();
  if (!m_context->isValid()) {
    LOG(ERROR) << "Can not create OpenGL context";
  }
}

#if defined(__linux__)
Z3DContext::Z3DContext()
{
  CHECK(FLAGS___use_EGL);
  static const int MAX_DEVICES = 4;
  EGLDeviceEXT eglDevs[MAX_DEVICES];
  EGLint numDevices;

  PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");

  eglQueryDevicesEXT(MAX_DEVICES, eglDevs, &numDevices);

  LOG(INFO) << "Detected " << numDevices << " EGL devices";

  // 1. Initialize EGL
  // EGLDisplay eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  m_eglDisplay = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, eglDevs[0], 0);
  EGLint major, minor;
  eglInitialize(m_eglDisplay, &major, &minor);
  LOG(INFO) << "EGL " << major << "." << minor;

  // 2. Select an appropriate configuration
  EGLint numConfigs;
  EGLConfig eglCfg;
  eglChooseConfig(m_eglDisplay, configAttribs, &eglCfg, 1, &numConfigs);

  // 4. Bind the API
  eglBindAPI(EGL_OPENGL_API);

  // 5. Create a context
  m_eglContext = eglCreateContext(m_eglDisplay, eglCfg, EGL_NO_CONTEXT, NULL);
}
#endif

Z3DContext::~Z3DContext()
{
  delete m_context;
#if defined(__linux__)
  if (m_eglDisplay) {
    eglTerminate(m_eglDisplay);
  }
#endif
}

ProcAddress Z3DContext::getProcAddress(const char* name) const
{
  if (!name) {
    return nullptr;
  }

#if defined(__linux__)
  if (m_context) {
    return m_context->getProcAddress(name);
  } else {
    return eglGetProcAddress(name);
  }
#else
  return m_context->getProcAddress(name);
#endif
}

void Z3DContext::makeCurrent() const
{
#if defined(__linux__)
  if (m_context) {
    m_context->makeCurrent(m_offscreenSurface);
  } else {
    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, m_eglContext);
  }
#else
  m_context->makeCurrent(m_offscreenSurface);
#endif
}

#if defined(__linux__)
Z3DContextGroup::Z3DContextGroup()
  : m_contextGroup(QOpenGLContext::currentContext()->shareGroup())
{
  if (FLAGS___use_EGL) {
    m_contextGroup = eglGetCurrentContext();
  } else {
    m_contextGroup = QOpenGLContext::currentContext()->shareGroup();
  }
  CHECK(m_contextGroup);
}
#else
Z3DContextGroup::Z3DContextGroup()
  : m_contextGroup(QOpenGLContext::currentContext()->shareGroup())
{
  CHECK(m_contextGroup);
}
#endif

bool Z3DContextGroup::operator<(const Z3DContextGroup& rhs) const
{
  return m_contextGroup < rhs.m_contextGroup;
}

bool Z3DContextGroup::operator==(const Z3DContextGroup& rhs) const
{
  return m_contextGroup == rhs.m_contextGroup;
}

bool Z3DContextGroup::operator!=(const Z3DContextGroup& rhs) const
{
  return m_contextGroup != rhs.m_contextGroup;
}

} // namespace nim
