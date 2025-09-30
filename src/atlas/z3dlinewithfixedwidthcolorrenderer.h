#pragma once

#include "z3dlinerenderer.h"

#include <span>

namespace nim {

class Z3DLineWithFixedWidthColorRenderer : public Z3DLineRenderer
{
public:
  // default use display list but not lighting in opengl mode
  explicit Z3DLineWithFixedWidthColorRenderer(Z3DRendererBase& base);

  using Z3DLineRenderer::setData;

  void setData(std::span<const glm::vec3> lines);
  void setData(std::vector<glm::vec3> lines);

  void setFixedLineWidth(float width);

  void setLineColor(const glm::vec4& color);

protected:
  void setLineColors();

  float lineWidth() const override;

protected:
  float m_fixedLineWidth = 2.f;
  glm::vec4 m_lineColor = glm::vec4(1.f, 1.f, 0.f, 1.f);
  std::vector<glm::vec4> m_lineColorsPrivate;
};

} // namespace nim
