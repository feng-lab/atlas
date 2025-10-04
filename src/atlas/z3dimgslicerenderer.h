#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dshaderprogram.h"
#include "zcolormap.h"
#include "zmesh.h"
#include <memory>
#include <string>

namespace nim {

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

  void enqueueRenderBatches(Z3DEye eye, RenderBackend backend, bool picking) override;

  [[nodiscard]] const std::vector<ZMesh>& slices() const
  {
    return m_slices;
  }

  [[nodiscard]] Z3DImg* image() const
  {
    return m_img;
  }

  [[nodiscard]] const std::vector<std::unique_ptr<ZColorMapParameter>>* colormaps() const
  {
    return m_colormaps;
  }

  [[nodiscard]] glm::uvec2 outputSize() const
  {
    return m_outputSize;
  }

  double renderProgressively(Z3DEye eye);

  bool renderingStarted(Z3DEye eye)
  {
    return m_progress[eye] > 0;
  }

  void resetProgress(Z3DEye eye)
  {
    m_progress[eye] = 0;
  }

protected:
  void bindVolumes(Z3DShaderProgram& shader) const;

  void bindVolume(Z3DShaderProgram& shader, size_t idx) const;

  [[nodiscard]] std::string generateHeader();

  void render(Z3DEye eye) override;

private:
  double renderSlice(Z3DEye eye, bool progressive = false);

  void renderSliceFast(Z3DEye eye);

protected:
  // Z3DShaderProgram m_volumeSliceShader;
  void createResources(RenderBackend backend) override;

  void destroyResources() override;

  std::unique_ptr<Z3DShaderProgram> m_scVolumeSliceShader;
  // Internal targets are obtained from the scratch pool
  std::unique_ptr<Z3DShaderProgram> m_mergeChannelShader;
  std::unique_ptr<Z3DShaderProgram> m_image3DSliceWithColorMapBlockIDsShader;
  std::unique_ptr<Z3DShaderProgram> m_image3DSliceWithColorMapShader;

  Z3DImg* m_img = nullptr;
  const std::vector<std::unique_ptr<ZColorMapParameter>>* m_colormaps = nullptr;
  std::vector<std::string> m_volumeUniformNames;
  std::vector<std::string> m_colormapUniformNames;

private:
  std::vector<ZMesh> m_slices;
  std::unique_ptr<Z3DVertexArrayObject> m_VAO;

  std::vector<uint32_t> m_blockIDs;
  bool m_fastRendering = true;
  // bool m_lastRenderingIsFastRendering = false;

  double m_progress[3] = {0, 0, 0};

  // No per-renderer textures; all temporary images are pooled.

  // Output size provided via ensureInternalTargets()
  glm::uvec2 m_outputSize{32, 32};
};

} // namespace nim
