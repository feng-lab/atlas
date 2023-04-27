#pragma once

#include "z3dprimitiverenderer.h"

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

  ZBoolParameter& useDynamicMaterialPara()
  {
    return m_useDynamicMaterial;
  }

protected:
  void compile() override;

  QString generateHeader();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  void renderUsingOpengl() override;
  void renderPickingUsingOpengl() override;
#endif

  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye eye) override;

  void appendDefaultColors();

protected:
  Z3DShaderGroup m_ellipsoidShaderGrp;

  ZIntParameter m_sphereSlicesStacks;
  ZBoolParameter m_useDynamicMaterial;

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
