#pragma once

#include "z3dprimitiverenderer.h"
#include "zmesh.h"

class Z3DTexture;

namespace nim {

// NOTE: Color of each vertex can comes from many sources, call setColorSource
// "MeshColor" (default) : colors field of ZMesh is used.
// "Mesh1DTexture" : 1DTextureCoord field of ZMesh is used, setTexture must be called
// "Mesh2DTexture" : 2DTextureCoord field of ZMesh is used, setTexture must be called
// "Mesh3DTexture" : 3DTextureCoord field of ZMesh is used, setTexture must be called
// "CustomColor" : setDataColors must be called to set color of each mesh
class Z3DMeshRenderer : public Z3DPrimitiveRenderer
{
public:
  explicit Z3DMeshRenderer(Z3DRendererBase& rendererBase);

  void setData(std::vector<ZMesh*>* meshInput);

  // if set, this color will used instead colors from ZMesh (if any)
  // the number should match the number of meshes
  void setDataColors(std::vector<glm::vec4>* meshColorsInput);

  // set texture, texture coordinates of each vertex should exist in
  // ZMesh
  void setTexture(Z3DTexture* tex);

  void setDataPickingColors(std::vector<glm::vec4>* meshPickingColorsInput = nullptr);

  // One of "MeshColor", "Mesh1DTexture", "Mesh2DTexture", "Mesh3DTexture", "CustomColor"
  void setColorSource(const QString& sc)
  {
    m_colorSource.select(sc);
  }

  ZStringIntOptionParameter& wireframeModePara()
  {
    return m_wireframeMode;
  }

  ZVec4Parameter& wireframeColorPara()
  {
    return m_wireframeColor;
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

  void adjustWidgets();

private:
  void prepareMesh();

  void prepareMeshColor();

  void prepareMeshPickingColor();

protected:
  Z3DShaderGroup m_meshShaderGrp;

  std::vector<ZMesh*>* m_meshPt;
  std::vector<glm::vec4>* m_meshColorsPt;
  std::vector<glm::vec4>* m_meshPickingColorsPt;
  std::vector<ZMesh*>* m_origMeshPt;
  std::vector<glm::vec4>* m_origMeshColorsPt;
  std::vector<glm::vec4>* m_origMeshPickingColorsPt;

  Z3DTexture* m_texture;

private:
  std::vector<ZMesh> m_splitMeshes;
  std::vector<ZMesh*> m_splitMeshesWrapper;
  std::vector<glm::vec4> m_splitMeshesColors;
  std::vector<glm::vec4> m_splitMeshesPickingColors;
  bool m_meshNeedSplit = false;
  std::vector<size_t> m_splitCount;
  bool m_meshColorReady;
  bool m_meshPickingColorReady;

  ZStringIntOptionParameter m_colorSource;

  bool m_dataChanged;
  bool m_pickingDataChanged;
  // one VAO for each mesh
  ZVertexArrayObject m_VAOs;
  ZVertexArrayObject m_pickingVAOs;
  std::vector<ZVertexBufferObject> m_VBOs;
  std::vector<ZVertexBufferObject> m_pickingVBOs;

  ZStringIntOptionParameter m_wireframeMode;
  ZVec4Parameter m_wireframeColor;
};

} // namespace nim
