#ifndef Z3DIMAGE2DRENDERER_H
#define Z3DIMAGE2DRENDERER_H

#include "z3dprimitiverenderer.h"
#include "zmesh.h"
#include "zcolormap.h"
#include "z3dshaderprogram.h"

namespace nim {

class Z3DVolume;

// render 2d image with colormap
// support up to 20 channels, use colormap of each volume to composite final image
// use python script to generate new shader to support more channels
class Z3DImage2DRenderer : public Z3DPrimitiveRenderer
{
  Q_OBJECT
public:
  explicit Z3DImage2DRenderer(Z3DRendererBase &rendererBase);

  // input vols can not be nullptr
  void setChannels(const std::vector<std::unique_ptr<Z3DVolume>> &vols,
                   const std::vector<std::unique_ptr<ZColorMapParameter>>& colormaps);

  // quad contains corner vertex and 2d texture coordinates
  // clear
  void clearQuads() { m_quads.clear(); }
  // add quad
  void addQuad(const ZMesh &quad);

signals:

protected slots:

protected:
  void bindVolumes(Z3DShaderProgram &shader);
  bool hasVolume() const;

  virtual void compile() override;
  QString generateHeader();

  virtual void render(Z3DEye eye) override;

  Z3DShaderProgram m_image2DShader;

  std::vector<Z3DVolume*> m_volumes;
  std::vector<ZColorMapParameter*> m_colormaps;
  std::vector<QString> m_volumeUniformNames;
  std::vector<QString> m_colormapUniformNames;

private:
  std::vector<ZMesh> m_quads;
  ZVertexArrayObject m_VAO;
};

} // namespace nim

#endif // Z3DIMAGE2DRENDERER_H
