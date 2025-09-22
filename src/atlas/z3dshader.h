#pragma once

#include "z3dcontext.h"
#include <string>

namespace nim {

// throw ZException if error
class Z3DShader
{
public:
  enum class Type
  {
    Vertex,
    Fragment,
    Geometry,
    TessellationControl,
    TessellationEvaluation,
    Compute
  };

  explicit Z3DShader(Z3DShader::Type type);

  ~Z3DShader();

  Z3DShader(const Z3DShader&) = delete;

  Z3DShader& operator=(const Z3DShader&) = delete;

  [[nodiscard]] Z3DShader::Type shaderType() const
  {
    return m_type;
  }

  void compileSourceCode(const char* source);

  void compileSourceCode(const std::string& source);

  [[nodiscard]] std::string sourceCode() const;

  [[nodiscard]] bool isCompiled() const
  {
    return m_compiled;
  }

  [[nodiscard]] unsigned int shaderId() const
  {
    return m_id;
  }

  [[nodiscard]] Z3DContextGroup context() const
  {
    return m_context;
  }

private:
  Type m_type;
  bool m_compiled;
  unsigned int m_id;
  Z3DContextGroup m_context;
};

} // namespace nim
