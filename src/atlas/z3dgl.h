#pragma once

#include "z3dtypes.h"
#include "zglmutils.h"
#include "zglobal.h"
#include "zmesh.h"
#include <glbinding/gl/gl.h>

#include <cstdint>
#include <span>
#include <vector>

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

// ---------------------------------------------------------------------------
// Span helpers for rendering payloads (GL vector storage → backend-neutral spans)
// ---------------------------------------------------------------------------
inline std::span<const float> spanFromGLfloats(const std::vector<GLfloat>& vec)
{
  if (vec.empty()) {
    return std::span<const float>();
  }
  static_assert(sizeof(GLfloat) == sizeof(float), "GLfloat expected to match float");
  return std::span<const float>(reinterpret_cast<const float*>(vec.data()), vec.size());
}

inline std::span<const uint32_t> spanFromGLuints(const std::vector<GLuint>& vec)
{
  if (vec.empty()) {
    return std::span<const uint32_t>();
  }
  static_assert(sizeof(GLuint) == sizeof(uint32_t), "GLuint expected to be 32-bit");
  return std::span<const uint32_t>(reinterpret_cast<const uint32_t*>(vec.data()), vec.size());
}

} // namespace nim
