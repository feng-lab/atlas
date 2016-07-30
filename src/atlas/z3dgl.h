#ifndef Z3DGL_H
#define Z3DGL_H

#ifdef _USE_GLEW_

#include <GL/glew.h>
#define GLVersionGE(a, b) GLEW_VERSION_ ## a ## _ ## b

#else

#include <glbinding/gl/gl.h>

using namespace gl;

#endif

#include "zglobal.h"
#include "zglmutils.h"

namespace nim {

enum class Z3DEye
{
  Left = 0, Mono, Right
};

enum class Z3DScreenShotType
{
  MonoView, HalfSideBySideStereoView, FullSideBySideStereoView
};

#ifndef _USE_GLEW_

bool GLVersionGE(int majorVersion, int minorVersion);

#endif

GLenum _CheckGLError(const char* file, int line, const char* function);

#define CHECK_GL_ERROR _CheckGLError(__FILE__, __LINE__, __PRETTY_FUNCTION__)

bool checkGLState(GLenum pname, bool value);

bool checkGLState(GLenum pname, GLint value);

bool checkGLState(GLenum pname, GLenum value);

bool checkGLState(GLenum pname, GLfloat value);

bool checkGLState(GLenum pname, const glm::vec4 value);

} // namespace nim

#endif  //Z3DGL_H
