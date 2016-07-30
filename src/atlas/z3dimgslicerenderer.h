#ifndef Z3DIMGSLICERENDERER_H
#define Z3DIMGSLICERENDERER_H

#include "z3dprimitiverenderer.h"
#include "zmesh.h"
#include "zcolormap.h"
#include "z3dshaderprogram.h"

namespace nim {

class Z3DVolume;

class Z3DImg;

// render 2d slices of volume with colormap
// use colormap of each volume to composite final image
class Z3DImgSliceRenderer : public Z3DPrimitiveRenderer
{
Q_OBJECT
public:
  explicit Z3DImgSliceRenderer(Z3DRendererBase& rendererBase);

  void setData(Z3DImg& img,
               const std::vector<std::unique_ptr<ZColorMapParameter>>& colormaps);

  void setFastRendering(bool v)
  { m_fastRendering = v; }

  void setLayerTarget(Z3DRenderTarget& target)
  { m_layerTarget = &target; }

  void setBlockIDsRenderTarget(Z3DRenderTarget& target)
  { m_blockIDsRenderTarget = &target; }

  // a slice (quad) in 3D volume contains corner vertex and 3d texture coordinates
  // clear
  void clearQuads()
  { m_quads.clear(); }

  // add quad
  void addQuad(const ZMesh& quad);

  virtual void compile() override;

protected:
  void bindVolumes(Z3DShaderProgram& shader);

  void bindVolume(Z3DShaderProgram& shader, size_t idx);

  QString generateHeader();

  virtual void render(Z3DEye eye) override;

protected:
  //Z3DShaderProgram m_volumeSliceShader;
  Z3DShaderProgram m_scVolumeSliceShader;
  Z3DRenderTarget* m_layerTarget = nullptr;
  Z3DRenderTarget* m_blockIDsRenderTarget = nullptr;
  Z3DShaderProgram m_mergeChannelShader;
  Z3DShaderProgram m_image3DSliceWithColorMapBlockIDsShader;
  Z3DShaderProgram m_image3DSliceWithColorMapShader;

  Z3DImg* m_img = nullptr;
  const std::vector<std::unique_ptr<ZColorMapParameter>>* m_colormaps = nullptr;
  std::vector<QString> m_volumeUniformNames;
  std::vector<QString> m_colormapUniformNames;

private:
  std::vector<ZMesh> m_quads;
  ZVertexArrayObject m_VAO;

  std::vector<uint32_t> m_blockIDs;
  bool m_fastRendering = false;
};

} // namespace nim

#endif // Z3DIMGSLICERENDERER_H
