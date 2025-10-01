#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dshaderprogram.h"
#include <memory>

namespace nim {

// render 3d texture coordinates as color
class Z3DTextureAndEyeCoordinateRenderer : public Z3DPrimitiveRenderer
{
public:
  explicit Z3DTextureAndEyeCoordinateRenderer(Z3DRendererBase& rendererBase);

  // triangle list should contains vertexs and 3d texture coordinates
  void setTriangleList(const ZMesh* mesh)
  {
    m_mesh = mesh;
    m_dataChanged = true;
  }
  // todo: add function to set data (vertex, texture coordinate, triangle type, indexes) separately

protected:
  void compile() override;

  void render(Z3DEye eye) override;

protected:
  const ZMesh* m_mesh;

  void createResources(RenderBackend backend) override;

  void destroyResources() override;

  std::unique_ptr<Z3DShaderProgram> m_renderTextureAndEyeCoordinateShader;

  std::unique_ptr<Z3DVertexBufferObject> m_VBOs;
  std::unique_ptr<Z3DVertexArrayObject> m_VAO;
  bool m_dataChanged;
};

} // namespace nim
