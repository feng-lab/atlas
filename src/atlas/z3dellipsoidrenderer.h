#ifndef Z3DELLIPSOIDRENDERER_H
#define Z3DELLIPSOIDRENDERER_H

#include "z3dprimitiverenderer.h"

namespace nim {

class Z3DEllipsoidRenderer : public Z3DPrimitiveRenderer
{
  Q_OBJECT
public:
  // default use display list and lighting for opengl mode
  explicit Z3DEllipsoidRenderer(Z3DRendererBase &rendererBase);

  void setData(std::vector<glm::vec3> *centers, std::vector<glm::vec3> *axis1, std::vector<glm::vec3> *axis2,
               std::vector<glm::vec3> *axis3, std::vector<glm::vec4> *specularAndShininessInput = NULL);
  void setDataColors(std::vector<glm::vec4> *ellipsoidColorsInput);
  void setDataPickingColors(std::vector<glm::vec4> *ellipsoidPickingColorsInput = NULL);

  ZBoolParameter& useDynamicMaterialPara() { return m_useDynamicMaterial; }

protected:
  virtual void compile() override;
  QString generateHeader();

#ifndef _USE_CORE_PROFILE_
  virtual void renderUsingOpengl() override;
  virtual void renderPickingUsingOpengl() override;
#endif

  virtual void render(Z3DEye eye) override;
  virtual void renderPicking(Z3DEye eye) override;

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

  ZVertexArrayObject m_VAO;
  ZVertexArrayObject m_pickingVAO;
  ZVertexBufferObject m_VBOs;
  ZVertexBufferObject m_pickingVBOs;
  bool m_dataChanged;
  bool m_pickingDataChanged;
};

} // namespace nim

#endif // Z3DELLIPSOIDRENDERER_H
