#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dshaderprogram.h"
#include "zcolormap.h"
#include "zmesh.h"
#include "z3drendertarget.h"
#include "z3dtexture.h"

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

  // Targets are owned internally; no external override needed

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

  // Ensure internal targets are sized; size is provided by filter
  void setOutputSize(const glm::uvec2& size)
  {
    // Store output size provided by the filter; pooled render targets use this on acquire
    m_outputSize = size;
  }

  double renderProgressively(Z3DEye eye);

  bool renderingStarted(Z3DEye eye)
  {
    return m_progress[eye] > 0;
  }

  // Public API minimal; progressive reset is internal and coordinated by filter

protected:
  void bindVolumes(Z3DShaderProgram& shader) const;

  void bindVolume(Z3DShaderProgram& shader, size_t idx) const;

  QString generateHeader();

  void render(Z3DEye eye) override;

private:
  friend class Z3DImgFilter; // allow filter to request a reset at a safe point

  void resetProgress(Z3DEye eye)
  {
    m_progress[eye] = 0;
  }
  double renderSlice(Z3DEye eye, bool progressive = false);

  void renderSliceFast(Z3DEye eye);

protected:
  // Z3DShaderProgram m_volumeSliceShader;
  Z3DShaderProgram m_scVolumeSliceShader;
  // Internal targets are obtained from the scratch pool
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

  // No per-renderer textures; all temporary images are pooled.

  // Output size provided via ensureInternalTargets()
  glm::uvec2 m_outputSize{32, 32};
};

} // namespace nim
