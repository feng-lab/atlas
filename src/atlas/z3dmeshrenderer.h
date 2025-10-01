#pragma once

#include "z3dprimitiverenderer.h"
#include "z3drendercommands.h"
#include "zmesh.h"
#include <QString>
#include <string>

class Z3DTexture;

namespace nim {

// NOTE: Color of each vertex can comes from many sources, call setColorSource
// "MeshColor" (default) : colors field of ZMesh is used.
// "Mesh1DTexture" : 1DTextureCoord field of ZMesh is used, setTexture must be called
// "Mesh2DTexture" : 2DTextureCoord field of ZMesh is used, setTexture must be called
// "Mesh3DTexture" : 3DTextureCoord field of ZMesh is used, setTexture must be called
// "CustomColor" : setDataColors must be called to set color of each mesh
enum class MeshColorSource
{
  MeshColor,
  Mesh1DTexture,
  Mesh2DTexture,
  Mesh3DTexture,
  CustomColor
};

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
  void setColorSource(MeshColorSource source);

  enum class WireframeMode
  {
    NoWireframe,
    WithWireframe,
    OnlyWireframe
  };

  void setWireframeMode(WireframeMode mode)
  {
    m_wireframeModeValue = mode;
  }

  void setWireframeColor(const glm::vec4& color)
  {
    m_wireframeColorValue = color;
  }

  // Command list integration helper used by backend implementations.
  void executeBatchGL(const RenderBatch& batch);

protected:
  void compile() override;

  [[nodiscard]] std::string generateHeader();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  void renderUsingOpengl() override;
  void renderPickingUsingOpengl() override;
#endif

  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye eye) override;

private:
  // Rebuilds the mesh pointer list, splitting oversized meshes while keeping
  // payload spans referencing renderer-owned storage.
  void prepareMesh();

  // Material/picking color helpers populate the lazily-evaluated span pointers
  // used by MeshPayload. They never allocate unless splitting requires it.
  void prepareMeshColor();

  void prepareMeshPickingColor();

  void renderImmediate(Z3DEye eye, bool appendBatch);

  // MeshPayload is a zero-copy view of the renderer's CPU-side buffers. Callers
  // must ensure the corresponding prepare* helpers ran before dereferencing the
  // spans they expose.
  [[nodiscard]] MeshPayload buildMeshPayload() const;
  [[nodiscard]] RenderBatch buildRenderBatch(Z3DEye eye) const;

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

  MeshColorSource m_colorSource;

  bool m_dataChanged;
  bool m_pickingDataChanged;
  // one VAO for each mesh
  Z3DVertexArrayObject m_VAOs;
  Z3DVertexArrayObject m_pickingVAOs;
  std::vector<Z3DVertexBufferObject> m_VBOs;
  std::vector<Z3DVertexBufferObject> m_pickingVBOs;

  WireframeMode m_wireframeModeValue = WireframeMode::NoWireframe;
  glm::vec4 m_wireframeColorValue{1.f};
};

} // namespace nim
