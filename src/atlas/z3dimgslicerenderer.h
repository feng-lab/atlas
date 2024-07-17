#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dshaderprogram.h"
#include "zcolormap.h"
#include "zmesh.h"

namespace nim {

class Z3DVolume;

class Z3DImg;

// render 2d slices of volume with colormap
// use colormap of each volume to composite final image
class Z3DImgSliceRenderer : public Z3DPrimitiveRenderer
{
public:
  explicit Z3DImgSliceRenderer(Z3DRendererBase& rendererBase);

  void setData(Z3DImg& img, const std::vector<std::unique_ptr<ZColorMapParameter>>& colormaps);

  void setFastRendering(bool v)
  {
    m_fastRendering = v;
  }

  [[nodiscard]] bool isFastRendering() const
  {
    return m_fastRendering;
  }

  void setLayerTarget(Z3DRenderTarget& target)
  {
    m_layerTarget = &target;
  }

  void setBlockIDsRenderTarget(Z3DRenderTarget& target)
  {
    m_blockIDsRenderTarget = &target;
  }

  //  [[nodiscard]] bool lastRenderingIsFastRendering() const
  //  {
  //    return m_lastRenderingIsFastRendering;
  //  }

  // a slice in 3D volume contains plane triangles and 3d texture coordinates
  // clear
  void clearSlices()
  {
    m_slices.clear();
  }

  // add slice
  void addSlice(const ZMesh& slice);

  void compile() override;

  double renderProgressively(Z3DEye eye);

  bool renderingStarted(Z3DEye eye)
  {
    return m_progress[eye] > 0;
  }

protected:
  void bindVolumes(Z3DShaderProgram& shader) const;

  void bindVolume(Z3DShaderProgram& shader, size_t idx) const;

  QString generateHeader();

  void render(Z3DEye eye) override;

private:
  double renderSlice(Z3DEye eye, bool progressive = false);

  void renderSliceFast(Z3DEye eye);

  void resetProgress(Z3DEye eye)
  {
    m_progress[eye] = 0;
  }

protected:
  // Z3DShaderProgram m_volumeSliceShader;
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
  std::vector<ZMesh> m_slices;
  Z3DVertexArrayObject m_VAO;

  std::vector<uint32_t> m_blockIDs;
  bool m_fastRendering = true;
  // bool m_lastRenderingIsFastRendering = false;

  double m_progress[3] = {0, 0, 0};
};

} // namespace nim
