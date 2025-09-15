#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dtransferfunction.h"
#include "zmesh.h"
#include "z3dshaderprogram.h"
#include "z3drendertarget.h"
#include "z3dtexture.h"
#include <memory>
#include "z3dtextureandeyecoordinaterenderer.h"
#include "z3dscratchresourcepool.h"

namespace nim {

class Z3DImg;

// use raycaster to render volume or 2D Image (stack with depth==1) with color
// transfer functions
class Z3DImgRaycasterRenderer : public Z3DPrimitiveRenderer
{
public:
  explicit Z3DImgRaycasterRenderer(Z3DRendererBase& rendererBase);

  void setData(Z3DImg& img);
  // Targets are owned internally; no external override needed

  // quad or entry_exit texture should be set before rendering

  // For 2D Image rendering, once set, entry exit textures will be cleared and
  // renderer switch to 2D mode
  // To render a 2D image, quad should contains corner vertex and 2d texture coordinates
  // To render a slice in 3D volume, quad should contains corner vertex and 3d texture coordinates
  // DO NOT call this function for 3d Raycaster
  // clear
  void clearQuads()
  {
    m_quads.clear();
  }

  // add quad
  void addQuad(const ZMesh& quad);

  void setFastRendering(bool v)
  {
    m_fastRendering = v;
  }

  bool isFastRendering() const
  {
    return m_fastRendering;
  }

  //  [[nodiscard]] bool lastRenderingIsFastRendering() const
  //  {
  //    return m_lastRenderingIsFastRendering;
  //  }

  // return true if something is rendered by this renderer
  [[nodiscard]] bool hasVisibleRendering() const;

  [[nodiscard]] QString compositeMode() const;

  ZStringIntOptionParameter& compositingModePara()
  {
    return m_compositingMode;
  }

  ZFloatParameter& samplingRatePara()
  {
    return m_samplingRate;
  }

  ZFloatParameter& isoValuePara()
  {
    return m_isoValue;
  }

  ZFloatParameter& localMIPThresholdPara()
  {
    return m_localMIPThreshold;
  }

  [[nodiscard]] const std::vector<std::unique_ptr<ZBoolParameter>>& channelVisibleParas() const
  {
    return m_channelVisibleParas;
  }

  [[nodiscard]] const std::vector<std::unique_ptr<Z3DTransferFunctionParameter>>& transferFuncParas() const
  {
    return m_transferFuncParas;
  }

  //  [[nodiscard]] const std::vector<std::unique_ptr<ZStringIntOptionParameter>>& texFilterModeParas() const
  //  {
  //    return m_texFilterModeParas;
  //  }

  void compile() override;

  void updateDisplayRanges();

  double renderProgressively(Z3DEye eye);

  bool renderingStarted(Z3DEye eye)
  {
    return m_channelIdx[eye] > -1;
  }

  // Ensure internal targets are sized; size is provided by filter
  void setOutputSize(const glm::uvec2& size)
  {
    // Store output size and resize per-eye ping-pong targets
    m_outputSize = size;
    for (int e = 0; e < 3; ++e) {
      m_imageRenderTarget1s[e]->resize(size);
      m_imageRenderTarget2s[e]->resize(size);
    }
  }

  // Compute entry/exit texture for a clipped volume surface, For 3D Raycasting rendering, once called, 2d quads will be
  // cleared and renderer switch to 3D mode
  void prepareEntryExit(const ZMesh& clipped, bool flipped, Z3DEye eye, const glm::uvec2& size);

  // Expose progress reset so filters can explicitly restart progressive accumulation
  void resetProgress(Z3DEye eye)
  {
    m_channelIdx[eye] = -1;
    m_round[eye] = 0;
  }

protected:
  void adjustWidgets();

  void bindVolumesAndTransferFuncs(Z3DShaderProgram& shader) const;

  void bindVolumeAndTransferFunc(Z3DShaderProgram& shader, size_t idx) const;

  QString generateHeader();

  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye) override;

private:
  // this function is used to get proper default
  // transfer functions (grey or color depends on current number of channel)
  void resetTransferFunctions();

  void render2DImage(Z3DEye eye, const std::vector<size_t>& visibleIdxs);

  double render2DSliceOf3DImage(Z3DEye eye, const std::vector<size_t>& visibleIdxs, bool progressive = false);

  void render2DSliceOf3DImageFast(Z3DEye eye, const std::vector<size_t>& visibleIdxs);

  double render3DImage(Z3DEye eye, const std::vector<size_t>& visibleIdxs, bool progressive = false);

  // return ture if is last round
  bool render3DImageForOneRound(Z3DEye eye,
                                size_t c,
                                uint32_t round,
                                float ze_to_zw_a,
                                float ze_to_zw_b,
                                float ze_to_screen_pixel_voxel_size);

  void render3DImageFast(Z3DEye eye, const std::vector<size_t>& visibleIdxs);

  // (resetProgress declared public above)

protected:
  //  Z3DShaderProgram m_raycasterShader;
  //  Z3DShaderProgram m_2dImageShader;
  //  Z3DShaderProgram m_volumeSliceWithTransferfunShader;

  // single channel version
  Z3DShaderProgram m_scRaycasterShader;
  Z3DShaderProgram m_sc2dImageShader;
  Z3DShaderProgram m_scVolumeSliceWithTransferfunShader;
  Z3DShaderProgram m_image3DSliceWithTransferfunBlockIDsShader;
  Z3DShaderProgram m_image3DSliceWithTransferfunShader;
  Z3DShaderProgram m_image3DRaycasterBlockIDsShader;
  Z3DShaderProgram m_image3DRaycasterShader;
  Z3DShaderProgram m_mergeChannelShader;
  Z3DShaderProgram m_copyTextureShader;

  // Internal targets
  // Internal targets are acquired from the scratch pool
  Z3DRenderTarget* m_lastImageRenderTargets[3] = {nullptr, nullptr, nullptr};
  Z3DRenderTarget* m_currentImageRenderTargets[3] = {nullptr, nullptr, nullptr};

  ZFloatParameter m_samplingRate; // Sampling rate of the raycasting, specified relative to the size of one voxel
  ZFloatParameter m_isoValue; // The used isovalue, when isosurface raycasting is enabled
  ZFloatParameter m_localMIPThreshold;

  ZStringIntOptionParameter m_compositingMode;

  Z3DImg* m_img = nullptr;
  std::vector<QString> m_volumeUniformNames;
  std::vector<QString> m_volumeDimensionNames;
  std::vector<QString> m_transferFuncUniformNames;
  std::vector<std::unique_ptr<ZBoolParameter>> m_channelVisibleParas;
  std::vector<std::unique_ptr<Z3DTransferFunctionParameter>> m_transferFuncParas;
  // std::vector<std::unique_ptr<ZStringIntOptionParameter>> m_texFilterModeParas;

private:
  std::vector<ZMesh> m_quads;
  const Z3DTexture* m_entryExitTexCoordAndZeTexture = nullptr;

  bool m_opaque;
  // double m_alpha; // only takes effect when m_opaque is true
  Z3DVertexArrayObject m_VAO;

  std::vector<uint32_t> m_blockIDs;
  bool m_fastRendering = true;
  // bool m_lastRenderingIsFastRendering = false;

  int m_channelIdx[3] = {-1, -1, -1};
  int m_round[3] = {0, 0, 0};

  // Output size provided via ensureInternalTargets()
  glm::uvec2 m_outputSize{32, 32};

  // Owned GL resources (moved from filter)
  // Layer textures
  // No per-renderer textures; entry/exit obtained from scratch pool
  Z3DTextureAndEyeCoordinateRenderer m_textureAndEyeCoordinateRenderer;
  Z3DScratchResourcePool::RenderTargetLease
    m_entryExitLease; // holds lifetime of entry/exit render target during a frame
  Z3DScratchResourcePool::RenderTargetLease m_progressiveLayerLease; // persistent across progressive rounds

  std::unique_ptr<Z3DRenderTarget> m_imageRenderTarget1s[3];
  std::unique_ptr<Z3DRenderTarget> m_imageRenderTarget2s[3];

  // (No internal camera state tracking; filter triggers resetProgress on invalidate.)
};

} // namespace nim
