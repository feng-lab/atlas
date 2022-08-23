#pragma once

#include "z3dlinerenderer.h"

namespace nim {

class Z3DLineWithFixedWidthColorRenderer : public Z3DLineRenderer
{
  Q_OBJECT

public:
  // default use display list but not lighting in opengl mode
  explicit Z3DLineWithFixedWidthColorRenderer(Z3DRendererBase& base);

  void setData(std::vector<glm::vec3>* linesInput) override;

  ZFloatParameter& lineWidthPara()
  {
    return m_lineWidth;
  }

  ZVec4Parameter& lineColorPara()
  {
    return m_lineColor;
  }

protected:
  void setLineColors();

  float lineWidth() const override;

protected:
  ZFloatParameter m_lineWidth;
  ZVec4Parameter m_lineColor;
  std::vector<glm::vec4> m_lineColorsPrivate;
};

} // namespace nim
