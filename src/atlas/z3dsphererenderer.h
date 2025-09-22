#pragma once

#include "z3dprimitiverenderer.h"
#include <algorithm>
#include <string>

namespace nim {

class Z3DSphereRenderer : public Z3DPrimitiveRenderer
{
public:
  // default use display list and lighting for opengl mode
  explicit Z3DSphereRenderer(Z3DRendererBase& rendererBase);

  void setData(std::vector<glm::vec4>* pointAndRadiusInput,
               std::vector<glm::vec4>* specularAndShininessInput = nullptr);

  void setDataColors(std::vector<glm::vec4>* pointColorsInput);

  void setDataPickingColors(std::vector<glm::vec4>* pointPickingColorsInput = nullptr);

  void setUseDynamicMaterial(bool enabled);

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  void setSphereSlicesStacks(int value)
  {
    int clamped = std::max(value, 3);
    if (m_sphereSlicesStacks == clamped) {
      return;
    }
    m_sphereSlicesStacks = clamped;
    invalidateOpenglRenderer();
  }
#endif

protected:
  void compile() override;

  [[nodiscard]] std::string generateHeader();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  void renderUsingOpengl() override;
  void renderPickingUsingOpengl() override;
#endif

  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye eye) override;

  void appendDefaultColors();

protected:
  Z3DShaderGroup m_sphereShaderGrp;

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  int m_sphereSlicesStacks = 36;
#endif

  bool m_useDynamicMaterial = true;

private:
  std::vector<glm::vec4> m_pointAndRadius;
  std::vector<glm::vec4> m_specularAndShininess;
  std::vector<glm::vec4> m_pointColors;
  std::vector<glm::vec4> m_pointPickingColors;
  std::vector<GLfloat> m_allFlags;
  std::vector<GLuint> m_indexs;

  // std::vector<GLuint> m_VBOs;
  // std::vector<GLuint> m_pickingVBOs;
  Z3DVertexArrayObject m_VAOs;
  Z3DVertexArrayObject m_pickingVAOs;
  std::vector<Z3DVertexBufferObject> m_VBOs;
  std::vector<Z3DVertexBufferObject> m_pickingVBOs;
  bool m_dataChanged;
  bool m_pickingDataChanged;
  size_t m_oneBatchNumber;
};

} // namespace nim
