#pragma once

#include "z3dtypes.h"

#include <memory>
#include <string>

namespace nim {

class Z3DRendererBase;
class Z3DPrimitiveRenderer;
class Z3DShaderProgram;

class Z3DRendererBackend
{
public:
  virtual ~Z3DRendererBackend() = default;

  virtual void setGlobalShaderParameters(Z3DRendererBase& renderer, Z3DShaderProgram& shader, Z3DEye eye) = 0;

  [[nodiscard]] virtual std::string generateHeader(const Z3DRendererBase& renderer) const = 0;

  [[nodiscard]] virtual std::string generateGeomHeader(const Z3DRendererBase& renderer) const = 0;

  virtual void beginRender(Z3DRendererBase& renderer) = 0;

  virtual void endRender(Z3DRendererBase& renderer) = 0;
};

std::unique_ptr<Z3DRendererBackend> createGLRendererBackend();

} // namespace nim
