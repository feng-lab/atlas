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

  // Ensure internal FBOs/textures are sized and configured
  void ensureInternalTargets(const glm::uvec2& size, size_t numChannels);

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
  // Internal targets
  Z3DRenderTarget m_layerTarget{glm::uvec2(32, 32)};
  Z3DRenderTarget m_blockIDsRenderTarget{glm::uvec2(32, 32)};
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

  // Textures moved from filter
  Z3DTexture m_layerColorTexture{GL_TEXTURE_2D_ARRAY, GLint(GL_RGBA16), glm::uvec3(32, 32, 3), GL_RGBA, GL_FLOAT};
  Z3DTexture m_layerDepthTexture{GL_TEXTURE_2D_ARRAY,
                                 GLint(GL_DEPTH_COMPONENT24),
                                 glm::uvec3(32, 32, 3),
                                 GL_DEPTH_COMPONENT,
                                 GL_FLOAT};
  Z3DTexture m_missBlocksTexture0{GL_TEXTURE_2D,
                                  GLint(GL_RGBA32UI),
                                  glm::uvec3(32, 32, 1),
                                  GL_RGBA_INTEGER,
                                  GL_UNSIGNED_INT,
                                  nullptr,
                                  GLint(GL_NEAREST),
                                  GLint(GL_NEAREST)};
  Z3DTexture m_missBlocksTexture1{GL_TEXTURE_2D,
                                  GLint(GL_RGBA32UI),
                                  glm::uvec3(32, 32, 1),
                                  GL_RGBA_INTEGER,
                                  GL_UNSIGNED_INT,
                                  nullptr,
                                  GLint(GL_NEAREST),
                                  GLint(GL_NEAREST)};
  Z3DTexture m_missBlocksTexture2{GL_TEXTURE_2D,
                                  GLint(GL_RGBA32UI),
                                  glm::uvec3(32, 32, 1),
                                  GL_RGBA_INTEGER,
                                  GL_UNSIGNED_INT,
                                  nullptr,
                                  GLint(GL_NEAREST),
                                  GLint(GL_NEAREST)};
  Z3DTexture m_missBlocksTexture3{GL_TEXTURE_2D,
                                  GLint(GL_RGBA32UI),
                                  glm::uvec3(32, 32, 1),
                                  GL_RGBA_INTEGER,
                                  GL_UNSIGNED_INT,
                                  nullptr,
                                  GLint(GL_NEAREST),
                                  GLint(GL_NEAREST)};
  Z3DTexture m_missBlocksTexture4{GL_TEXTURE_2D,
                                  GLint(GL_RGBA32UI),
                                  glm::uvec3(32, 32, 1),
                                  GL_RGBA_INTEGER,
                                  GL_UNSIGNED_INT,
                                  nullptr,
                                  GLint(GL_NEAREST),
                                  GLint(GL_NEAREST)};
  Z3DTexture m_missBlocksTexture5{GL_TEXTURE_2D,
                                  GLint(GL_RGBA32UI),
                                  glm::uvec3(32, 32, 1),
                                  GL_RGBA_INTEGER,
                                  GL_UNSIGNED_INT,
                                  nullptr,
                                  GLint(GL_NEAREST),
                                  GLint(GL_NEAREST)};
  Z3DTexture m_missBlocksTexture6{GL_TEXTURE_2D,
                                  GLint(GL_RGBA32UI),
                                  glm::uvec3(32, 32, 1),
                                  GL_RGBA_INTEGER,
                                  GL_UNSIGNED_INT,
                                  nullptr,
                                  GLint(GL_NEAREST),
                                  GLint(GL_NEAREST)};
  Z3DTexture m_missBlocksTexture7{GL_TEXTURE_2D,
                                  GLint(GL_RGBA32UI),
                                  glm::uvec3(32, 32, 1),
                                  GL_RGBA_INTEGER,
                                  GL_UNSIGNED_INT,
                                  nullptr,
                                  GLint(GL_NEAREST),
                                  GLint(GL_NEAREST)};
};

} // namespace nim
