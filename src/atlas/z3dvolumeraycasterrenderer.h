#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dtransferfunction.h"
#include "zmesh.h"
#include "z3dshaderprogram.h"
#include "z3drendertarget.h"
#include <QString>

namespace nim {

class Z3DImg;
enum class VolumeCompositingMode
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
class Z3DVolumeRaycasterRenderer : public Z3DPrimitiveRenderer
{
public:
  explicit Z3DVolumeRaycasterRenderer(Z3DRendererBase& rendererBase);

  void setChannels(const std::vector<std::unique_ptr<Z3DVolume>>& vols);

  void setChannels(const Z3DImg& img);

  void setLayerTarget(Z3DRenderTarget* layerTarget)
  {
    m_layerTarget = layerTarget;
  }

  // quad or entryexit texture should be set before rendering

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

  // For 3D Raycasting rendering, once called, 2d quads will be cleared and renderer
  // switch to 3D mode
  void setEntryExitInfo(const Z3DTexture* entryTexCoordTexture,
                        const Z3DTexture* entryEyeCoordTexture,
                        const Z3DTexture* exitTexCoordTexture,
                        const Z3DTexture* exitEyeCoordTexture);

  void translate(double dx, double dy, double dz);

  // return true if something is rendered by this renderer
  bool hasVisibleRendering() const;

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

  void setCompositingMode(VolumeCompositingMode mode)
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

  void setTexFilterMode(size_t index, GLint mode);

  void setTexFilterModes(const std::vector<GLint>& modes);

  [[nodiscard]] const std::vector<bool>& channelVisibilities() const
  {
    return m_channelVisibilities;
  }

protected:
  void bindVolumesAndTransferFuncs(Z3DShaderProgram& shader);

  void bindVolumeAndTransferFunc(Z3DShaderProgram& shader, size_t idx);

  void compile() override;

  QString generateHeader();

  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye) override;

  //  Z3DShaderProgram m_raycasterShader;
  //  Z3DShaderProgram m_2dImageShader;
  //  Z3DShaderProgram m_volumeSliceWithTransferfunShader;

  // single channel version
  Z3DShaderProgram m_scRaycasterShader;
  Z3DShaderProgram m_sc2dImageShader;
  Z3DShaderProgram m_scVolumeSliceWithTransferfunShader;
  Z3DRenderTarget* m_layerTarget = nullptr;
  Z3DShaderProgram m_mergeChannelShader;
  float m_samplingRateValue = 1.f; // Sampling rate of the raycasting, specified relative to the size of one voxel
  float m_isoValue = 0.5f; // The used isovalue, when isosurface raycasting is enabled
  float m_localMIPThreshold = 0.8f;

  VolumeCompositingMode m_compositingModeValue = VolumeCompositingMode::DirectVolumeRendering;

  std::vector<Z3DVolume*> m_volumes;
  std::vector<QString> m_volumeUniformNames;
  std::vector<QString> m_volumeDimensionNames;
  std::vector<QString> m_transferFuncUniformNames;
  std::vector<bool> m_channelVisibilities;
  std::vector<Z3DTransferFunction*> m_transferFunctions;
  std::vector<GLint> m_texFilterModes;

private:
  bool m_is2DImage;

  std::vector<ZMesh> m_quads;
  const Z3DTexture* m_entryTexCoordTexture;
  const Z3DTexture* m_entryEyeCoordTexture;
  const Z3DTexture* m_exitTexCoordTexture;
  const Z3DTexture* m_exitEyeCoordTexture;

  ZVertexArrayObject m_VAO;
};

} // namespace nim
