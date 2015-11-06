#ifndef Z3DTEXTUREANDEYECOORDINATERENDERER_H
#define Z3DTEXTUREANDEYECOORDINATERENDERER_H


#include "z3dprimitiverenderer.h"
#include "z3dshaderprogram.h"

class ZMesh;

namespace nim {

// render 3d texture coordinates as color
class Z3DTextureAndEyeCoordinateRenderer : public Z3DPrimitiveRenderer
{
  Q_OBJECT
public:
  explicit Z3DTextureAndEyeCoordinateRenderer(Z3DRendererBase &rendererBase);

  // triangle list should contains vertexs and 3d texture coordinates
  void setTriangleList(const ZMesh *mesh) { m_mesh = mesh; m_dataChanged = true; }
  // todo: add function to set data (vertex, texture coordinate, triangle type, indexes) separately

signals:

public slots:

protected:
  virtual void compile() override;

  virtual void render(Z3DEye eye) override;
  virtual void renderPicking(Z3DEye) override;

  const ZMesh *m_mesh;

  Z3DShaderProgram m_renderTextureAndEyeCoordinateShader;

  ZVertexBufferObject m_VBOs;
  ZVertexArrayObject m_VAO;
  bool m_dataChanged;
};

} // namespace nim

#endif // Z3DTEXTUREANDEYECOORDINATERENDERER_H
