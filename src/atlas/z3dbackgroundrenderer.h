#pragma once

#include "z3dprimitiverenderer.h"
#include <string>

namespace nim {

enum class BackgroundMode
{
  Uniform,
  Gradient
};

enum class BackgroundGradientOrientation
{
  LeftToRight,
  RightToLeft,
  TopToBottom,
  BottomToTop
};

class Z3DBackgroundRenderer : public Z3DPrimitiveRenderer
{
public:
  explicit Z3DBackgroundRenderer(Z3DRendererBase& rendererBase);

  void setMode(BackgroundMode mode);

  void setFirstColor(const glm::vec4& color)
  {
    m_firstColorValue = color;
  }

  void setSecondColor(const glm::vec4& color)
  {
    m_secondColorValue = color;
  }

  void setGradientOrientation(BackgroundGradientOrientation orientation);

  void setRenderingRegion(double left = 0., double right = 1., double bottom = 0., double top = 1.);

  void compile() override;

  [[nodiscard]] std::string generateHeader();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  void renderUsingOpengl() override;
  void renderPickingUsingOpengl() override;
#endif

  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye) override;

protected:
  Z3DShaderGroup m_backgroundShaderGrp;

  glm::vec4 m_region{0.f, 1.f, 0.f, 1.f};

  BackgroundMode m_modeValue = BackgroundMode::Gradient;
  BackgroundGradientOrientation m_orientationValue = BackgroundGradientOrientation::BottomToTop;
  glm::vec4 m_firstColorValue{1.f, 1.f, 1.f, 1.f};
  glm::vec4 m_secondColorValue{0.2f, 0.2f, 0.2f, 1.f};

  Z3DVertexArrayObject m_VAO;
  Z3DVertexBufferObject m_VBO;
};

} // namespace nim
