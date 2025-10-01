#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dtransferfunction.h"
#include "zmesh.h"
#include "z3dshaderprogram.h"
#include "z3dtexture.h"
#include "z3dtextureandeyecoordinaterenderer.h"
#include "z3dscratchresourcepool.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace nim {

class Z3DImg;
enum class ImgCompositingMode
{
  DirectVolumeRendering,
  MaximumIntensityProjection,
  MIPOpaque,
  LocalMIP,
  LocalMIPOpaque,
  IsoSurface,
  XRay
};

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

  void compile() override;

  double renderProgressively(Z3DEye eye);

  bool renderingStarted(Z3DEye eye)
  {
    return m_channelIdx[eye] > -1;
  }

  void setSamplingRate(float value)
  {
    m_samplingRateValue = value;
  }

  void setIsoValue(float value)
  {
    m_isoValue = value;
  }

  void setLocalMIPThreshold(float value)
  {
    m_localMIPThreshold = value;
  }

  void setCompositingMode(ImgCompositingMode mode)
  {
    if (m_compositingModeValue == mode) {
      return;
    }
    m_compositingModeValue = mode;
    compile();
  }

  void setChannelCount(size_t count);

  void setChannelVisibility(size_t index, bool visible);

  void setChannelVisibilities(const std::vector<bool>& visibilities);

  void setTransferFunction(size_t index, Z3DTransferFunction* transferFunction);

  void setTransferFunctions(const std::vector<Z3DTransferFunction*>& transferFunctions);

  [[nodiscard]] const std::vector<bool>& channelVisibilities() const
  {
    return m_channelVisibilities;
  }

  // Ensure internal targets are sized; size is provided by filter
  void setOutputSize(const glm::uvec2& size)
  {
    if (m_outputSize == size) {
      return;
    }
    m_outputSize = size;
    releaseAllRaycastAccumulators();
  }

  // Compute entry/exit texture for a clipped volume surface, For 3D Raycasting rendering, once called, 2d quads will be
  // cleared and renderer switch to 3D mode
  void prepareEntryExit(const ZMesh& clipped, bool flipped, Z3DEye eye, const glm::uvec2& size);

  // Reset progressive accumulation state for an eye
  void resetProgress(Z3DEye eye)
  {
    m_channelIdx[eye] = -1;
    m_round[eye] = 0;
  }

  // Release any scratch-pool backed targets retained across frames.
  void releaseScratchResources();

protected:
  void bindVolumesAndTransferFuncs(Z3DShaderProgram& shader) const;

  void bindVolumeAndTransferFunc(Z3DShaderProgram& shader, size_t idx) const;

  [[nodiscard]] std::string generateHeader();

  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye) override;

private:
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

  void ensureRaycastAccumulators(Z3DEye eye);
  void releaseRaycastAccumulators(Z3DEye eye);
  void releaseAllRaycastAccumulators();

protected:
  //  Z3DShaderProgram m_raycasterShader;
  //  Z3DShaderProgram m_2dImageShader;
  //  Z3DShaderProgram m_volumeSliceWithTransferfunShader;

  // single channel version
  void createResources(RenderBackend backend) override;

  void destroyResources() override;

  std::unique_ptr<Z3DShaderProgram> m_scRaycasterShader;
  std::unique_ptr<Z3DShaderProgram> m_sc2dImageShader;
  std::unique_ptr<Z3DShaderProgram> m_scVolumeSliceWithTransferfunShader;
  std::unique_ptr<Z3DShaderProgram> m_image3DSliceWithTransferfunBlockIDsShader;
  std::unique_ptr<Z3DShaderProgram> m_image3DSliceWithTransferfunShader;
  std::unique_ptr<Z3DShaderProgram> m_image3DRaycasterBlockIDsShader;
  std::unique_ptr<Z3DShaderProgram> m_image3DRaycasterShader;
  std::unique_ptr<Z3DShaderProgram> m_mergeChannelShader;
  std::unique_ptr<Z3DShaderProgram> m_copyTextureShader;

  // Internal targets
  // Raycast accumulators are acquired from the scratch pool on demand
  std::array<Z3DScratchResourcePool::RenderTargetLease, 3> m_lastRaycastAccum;
  std::array<Z3DScratchResourcePool::RenderTargetLease, 3> m_currentRaycastAccum;

  float m_samplingRateValue = 1.f; // Sampling rate of the raycasting, specified relative to the size of one voxel
  float m_isoValue = 0.5f; // The used isovalue, when isosurface raycasting is enabled
  float m_localMIPThreshold = 0.8f;

  ImgCompositingMode m_compositingModeValue = ImgCompositingMode::DirectVolumeRendering;

  Z3DImg* m_img = nullptr;
  std::vector<std::string> m_volumeUniformNames;
  std::vector<std::string> m_volumeDimensionNames;
  std::vector<std::string> m_transferFuncUniformNames;
  std::vector<bool> m_channelVisibilities;
  std::vector<Z3DTransferFunction*> m_transferFunctions;

private:
  std::vector<ZMesh> m_quads;
  const Z3DTexture* m_entryExitTexCoordAndZeTexture = nullptr;

  std::unique_ptr<Z3DVertexArrayObject> m_VAO;

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
};

} // namespace nim
