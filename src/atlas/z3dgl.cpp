#include "z3dgl.h"
#include "zlog.h"

#ifndef _USE_GLEW_

#include <glbinding/ContextInfo.h>
#include <glbinding/Version.h>

#endif

namespace nim {

#ifndef _USE_GLEW_
bool GLVersionGE(int majorVersion, int minorVersion)
{
  return glbinding::ContextInfo::version() >= glbinding::Version(majorVersion, minorVersion);
}
#endif

GLenum _CheckGLError(const char* file, int line, const char* function)
{
  GLenum err = glGetError();

  if (err != GL_NO_ERROR) {
    QString errStr;
    switch (err) {
    case GL_INVALID_ENUM:
      errStr = "openGL invalid enumerant";
      break;
    case GL_INVALID_VALUE:
      errStr = "openGL invalid value";
      break;
    case GL_INVALID_OPERATION:
      errStr = "openGL invalid operation";
      break;
    case GL_INVALID_FRAMEBUFFER_OPERATION:
      errStr = "openGL invalid framebuffer operation";
      break;
    case GL_STACK_OVERFLOW:
      errStr = "openGL stack overflow";
      break;
    case GL_STACK_UNDERFLOW:
      errStr = "openGL stack underflow";
      break;
    case GL_OUT_OF_MEMORY:
      errStr = "openGL out of memory";
      break;
    default:
      errStr = "openGL unknown error";
      break;
    }

    LERRORF(file, line, function) << errStr;
  }

  return err;
}

bool checkGLState(GLenum pname, bool value)
{
  GLboolean b;
  glGetBooleanv(pname, &b);
  return (static_cast<bool>(b) == value);
}

bool checkGLState(GLenum pname, GLint value)
{
  GLint i;
  glGetIntegerv(pname, &i);
  return (i == value);
}

bool checkGLState(GLenum pname, GLenum value)
{
  GLint i;
  glGetIntegerv(pname, &i);
  return (GLenum(i) == value);
}

bool checkGLState(GLenum pname, GLfloat value)
{
  GLfloat f;
  glGetFloatv(pname, &f);
  return (f == value);
}

bool checkGLState(GLenum pname, const glm::vec4 value)
{
  glm::vec4 v;
  glGetFloatv(pname, &v[0]);
  return (v == value);
}

} // namespace nim
