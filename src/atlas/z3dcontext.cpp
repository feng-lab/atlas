#include "z3dcontext.h"

#include "zlog.h"
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QOffscreenSurface>

DEFINE_uint32(use_gpu_device, 0, "choose which gpu to use for rendering, default is 0 (the first one)");

#if defined(__linux__)
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

DEFINE_bool(__use_EGL, false, "use EGL in linux console mode");

namespace {

static const EGLint configAttribs[] = {EGL_SURFACE_TYPE,
                                       EGL_PBUFFER_BIT,
                                       EGL_ALPHA_SIZE,
                                       8,
                                       EGL_BLUE_SIZE,
                                       8,
                                       EGL_GREEN_SIZE,
                                       8,
                                       EGL_RED_SIZE,
                                       8,
                                       EGL_DEPTH_SIZE,
                                       24,
                                       EGL_RENDERABLE_TYPE,
                                       EGL_OPENGL_BIT,
                                       EGL_NONE};

void checkEGLError()
{
  if (auto res = eglGetError(); res != EGL_SUCCESS) {
    switch (res) {
      case EGL_NOT_INITIALIZED:
        throw nim::ZException(
          "EGL is not initialized, or could not be initialized, for the specified EGL display connection.");
      case EGL_BAD_ACCESS:
        throw nim::ZException(
          "EGL cannot access a requested resource (for example a context is bound in another thread).");
      case EGL_BAD_ALLOC:
        throw nim::ZException("EGL failed to allocate resources for the requested operation.");
      case EGL_BAD_ATTRIBUTE:
        throw nim::ZException("An unrecognized attribute or attribute value was passed in the attribute list.");
      case EGL_BAD_CONTEXT:
        throw nim::ZException("An EGLContext argument does not name a valid EGL rendering context.");
      case EGL_BAD_CONFIG:
        throw nim::ZException("An EGLConfig argument does not name a valid EGL frame buffer configuration.");
      case EGL_BAD_CURRENT_SURFACE:
        throw nim::ZException(
          "The current surface of the calling thread is a window, pixel buffer or pixmap that is no longer valid.");
      case EGL_BAD_DISPLAY:
        throw nim::ZException("An EGLDisplay argument does not name a valid EGL display connection.");
      case EGL_BAD_SURFACE:
        throw nim::ZException(
          "An EGLSurface argument does not name a valid surface (window, pixel buffer or pixmap) configured for GL rendering.");
      case EGL_BAD_MATCH:
        throw nim::ZException(
          "Arguments are inconsistent (for example, a valid context requires buffers not supplied by a valid surface).");
      case EGL_BAD_PARAMETER:
        throw nim::ZException("One or more argument values are invalid.");
      case EGL_BAD_NATIVE_PIXMAP:
        throw nim::ZException("A NativePixmapType argument does not refer to a valid native pixmap.");
      case EGL_BAD_NATIVE_WINDOW:
        throw nim::ZException("A NativeWindowType argument does not refer to a valid native window.");
      case EGL_CONTEXT_LOST:
        throw nim::ZException(
          "A power management event has occurred. The application must destroy all contexts and reinitialise OpenGL ES state and objects to continue rendering.");
      default:
        throw nim::ZException("impossible egl error value");
    }
  }
}

} // namespace

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
  static const int MAX_DEVICES = 48;
  EGLDeviceEXT eglDevs[MAX_DEVICES];
  EGLint numDevices;

  PFNEGLQUERYDEVICESEXTPROC eglQueryDevicesEXT = (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
  eglQueryDevicesEXT(MAX_DEVICES, eglDevs, &numDevices);
  checkEGLError();
  LOG(INFO) << "Detected " << numDevices << " EGL devices";
  if (static_cast<EGLint>(FLAGS_use_gpu_device) >= numDevices) {
    throw ZException(fmt::format("Specified GPU device {} is not available", FLAGS_use_gpu_device));
    LOG(ERROR) << "Sepcified GPU device " << FLAGS_use_gpu_device << " is not available";
  }
  LOG(INFO) << "Use EGL Device " << FLAGS_use_gpu_device;

  // 1. Initialize EGL
  // EGLDisplay eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  m_eglDisplay = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, eglDevs[FLAGS_use_gpu_device], nullptr);
  if (m_eglDisplay == EGL_NO_DISPLAY) {
    checkEGLError();
    throw ZException("can not create EGL display");
  }
  EGLint major, minor;
  auto res = eglInitialize(m_eglDisplay, &major, &minor);
  if (res == EGL_FALSE) {
    checkEGLError();
    throw ZException("can not initialize EGL");
  }
  LOG(INFO) << "EGL " << major << "." << minor;

  // 2. Select an appropriate configuration
  EGLint numConfigs;
  EGLConfig eglCfg;
  res = eglChooseConfig(m_eglDisplay, configAttribs, &eglCfg, 1, &numConfigs);
  if (res == EGL_FALSE) {
    checkEGLError();
    throw ZException("EGL can not choose config");
  }

  // 4. Bind the API
  res = eglBindAPI(EGL_OPENGL_API);
  if (res == EGL_FALSE) {
    checkEGLError();
    throw ZException("EGL can not bind API");
  }

  // 5. Create a context
  m_eglContext = eglCreateContext(m_eglDisplay, eglCfg, EGL_NO_CONTEXT, NULL);
  if (m_eglContext == EGL_NO_CONTEXT) {
    checkEGLError();
    throw ZException("can not create EGL context");
  }
}
#else
Z3DContext::Z3DContext() {
  if (FLAGS_use_gpu_device != 0) {
    throw ZException("Flag --use_gpu_device is Linux only");
  }
  LOG(FATAL) << "This initialization method is Linux only";
}
#endif

Z3DContext::~Z3DContext()
{
  if (m_context) {
    // Ensure no context is current before destruction
    m_context->doneCurrent();
    delete m_context;
    m_context = nullptr;
  }
#if defined(__linux__)
  if (m_eglDisplay && m_eglContext) {
    eglDestroyContext(m_eglDisplay, (EGLContext)m_eglContext);
    m_eglContext = nullptr;
  }
  if (m_eglDisplay) {
    eglTerminate(m_eglDisplay);
    m_eglDisplay = nullptr;
  }
#endif
}

ProcAddress Z3DContext::getProcAddress(const char* name) const
{
  if (!name) {
    return nullptr;
  }

#if defined(__linux__)
  if (FLAGS___use_EGL) {
    return eglGetProcAddress(name);
  } else {
    return m_context->getProcAddress(name);
  }
#else
  return m_context->getProcAddress(name);
#endif
}

void Z3DContext::makeCurrent() const
{
#if defined(__linux__)
  if (FLAGS___use_EGL) {
    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, m_eglContext);
  } else {
    m_context->makeCurrent(m_offscreenSurface);
  }
#else
  m_context->makeCurrent(m_offscreenSurface);
#endif
}

#if defined(__linux__)
Z3DContextGroup::Z3DContextGroup()
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

} // namespace nim
