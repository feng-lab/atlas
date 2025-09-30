#pragma once

#include "z3dprimitiverenderer.h"
#include "z3drendercommands.h"
#include <string>

namespace nim {

enum class ConeCapStyle
{
  FlatCaps,
  NoCaps,
  RoundCaps,
  RoundBaseFlatTop,
  FlatBaseRoundTop
};

class Z3DConeRenderer : public Z3DPrimitiveRenderer
{
public:
  // default use display list and lighting in opengl mode
  // Round cap style might have bug. It only works when we are dealing with cylinder with slightly different radius.
  explicit Z3DConeRenderer(Z3DRendererBase& rendererBase);

  // base radius should be smaller than top radius
  void setData(std::vector<glm::vec4>* baseAndBaseRadius, std::vector<glm::vec4>* axisAndTopRadius);

  void setDataColors(std::vector<glm::vec4>* coneColors);

  void setDataColors(std::vector<glm::vec4>* coneBaseColors, std::vector<glm::vec4>* coneTopColors);

  void setDataPickingColors(std::vector<glm::vec4>* conePickingColors = nullptr);

  void setConeCapStyle(ConeCapStyle style);

  void setCylinderSubdivisionAroundZ(int subdivisions);

  void setCylinderSubdivisionAlongZ(int subdivisions);

protected:
  void compile() override;

  [[nodiscard]] std::string generateHeader();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  void renderUsingOpengl() override;
  void renderPickingUsingOpengl() override;
#endif

  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye eye) override;

  void executeBatchGL(const RenderBatch& batch);

  void appendDefaultColors();

  [[nodiscard]] ConePayload buildConePayload() const;
  [[nodiscard]] RenderBatch buildRenderBatch(Z3DEye eye) const;

  void renderImmediate(Z3DEye eye, bool appendBatch);

protected:
  Z3DShaderGroup m_coneShaderGrp;

  ConeCapStyle m_coneCapStyle = ConeCapStyle::FlatCaps;
  int m_cylinderSubdivisionAroundZ = 36;
  int m_cylinderSubdivisionAlongZ = 1;

private:
  std::vector<glm::vec4> m_baseAndBaseRadius;
  std::vector<glm::vec4> m_axisAndTopRadius;
  std::vector<glm::vec4> m_coneBaseColors;
  std::vector<glm::vec4> m_coneTopColors;
  std::vector<glm::vec4> m_conePickingColors;
  std::vector<GLfloat> m_allFlags;
  std::vector<GLuint> m_indexs;

  bool m_sameColorForBaseAndTop = false;

  bool m_useConeShader2 = true;

  Z3DVertexArrayObject m_VAO;
  Z3DVertexArrayObject m_pickingVAO;
  Z3DVertexBufferObject m_VBOs;
  Z3DVertexBufferObject m_pickingVBOs;
  bool m_dataChanged;
  bool m_pickingDataChanged;
};

} // namespace nim
