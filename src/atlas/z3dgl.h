#pragma once

#include "z3dtypes.h"
#include "zglmutils.h"
#include "zglobal.h"
#include "zmesh.h"
#include <glbinding/gl/gl.h>

namespace nim {

using namespace gl;

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
