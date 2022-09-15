#pragma once

#include "zglmutils.h"
#include "zglobal.h"
#include "zmesh.h"
#include <glbinding/gl/gl.h>

DEFINE_bool(
  atlas_check_opengl_error_for_all_gl_calls,
  true,
  "Whether to check opengl error after all gl calls, default is true, can set to false for better performance");

namespace nim {

using namespace gl;

enum class Z3DEye
{
  Left = 0,
  Mono,
  Right
};

enum class Z3DScreenShotType
{
  MonoView,
  HalfSideBySideStereoView,
  FullSideBySideStereoView
};

bool GLVersionGE(int majorVersion, int minorVersion);

void CheckGLError_Impl(const char* file, int line);

#define CHECK_GL_ERROR CheckGLError_Impl(__FILE__, __LINE__);

bool checkGLState(GLenum pname, bool value);

bool checkGLState(GLenum pname, GLint value);

bool checkGLState(GLenum pname, GLenum value);

bool checkGLState(GLenum pname, GLfloat value);

bool checkGLState(GLenum pname, const glm::vec4& value);

GLenum toGLType(ZMesh::Type type);

} // namespace nim
