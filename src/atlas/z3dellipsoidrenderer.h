#pragma once

#include "z3dprimitiverenderer.h"
#include <algorithm>
#include <string>

namespace nim {

class Z3DEllipsoidRenderer : public Z3DPrimitiveRenderer
{
public:
  // default use display list and lighting for opengl mode
  explicit Z3DEllipsoidRenderer(Z3DRendererBase& rendererBase);

  void setData(std::vector<glm::vec3>* centers,
               std::vector<glm::vec3>* axis1,
               std::vector<glm::vec3>* axis2,
               std::vector<glm::vec3>* axis3,
               std::vector<glm::vec4>* specularAndShininessInput = nullptr);

  void setDataColors(std::vector<glm::vec4>* ellipsoidColorsInput);

  void setDataPickingColors(std::vector<glm::vec4>* ellipsoidPickingColorsInput = nullptr);

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
  Z3DShaderGroup m_ellipsoidShaderGrp;

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  int m_sphereSlicesStacks = 36;
#endif

  bool m_useDynamicMaterial = true;

private:
  std::vector<glm::vec4> m_axis1;
  std::vector<glm::vec4> m_axis2;
  std::vector<glm::vec4> m_axis3;
  std::vector<glm::vec4> m_centers;

  std::vector<glm::vec4> m_specularAndShininess;
  std::vector<glm::vec4> m_ellipsoidColors;
  std::vector<glm::vec4> m_ellipsoidPickingColors;
  std::vector<GLfloat> m_allFlags;
  std::vector<GLuint> m_indexs;

  Z3DVertexArrayObject m_VAO;
  Z3DVertexArrayObject m_pickingVAO;
  Z3DVertexBufferObject m_VBOs;
  Z3DVertexBufferObject m_pickingVBOs;
  bool m_dataChanged;
  bool m_pickingDataChanged;
};

} // namespace nim
