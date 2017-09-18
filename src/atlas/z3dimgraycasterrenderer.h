#pragma once

#include "z3dprimitiverenderer.h"
#include "z3dtransferfunction.h"
#include "zmesh.h"
#include "z3dshaderprogram.h"
#include "z3drendertarget.h"
#include <boost/align/aligned_allocator.hpp>

namespace nim {

class Z3DImg;

// use raycaster to render volume or 2D Image (stack with depth==1) with color
// transfer functions
class Z3DImgRaycasterRenderer : public Z3DPrimitiveRenderer
{
Q_OBJECT
public:
  explicit Z3DImgRaycasterRenderer(Z3DRendererBase& rendererBase);

  void setData(Z3DImg& img);

  void setLayerTarget(Z3DRenderTarget& target)
  { m_layerTarget = &target; }

  void setBlockIDsRenderTarget(Z3DRenderTarget& target)
  { m_blockIDsRenderTarget = &target; }

  // quad or entryexit texture should be set before rendering

  // For 2D Image rendering, once set, entry exit textures will be cleared and
  // renderer switch to 2D mode
  // To render a 2D image, quad should contains corner vertex and 2d texture coordinates
  // To render a slice in 3D volume, quad should contains corner vertex and 3d texture coordinates
  // DO NOT call this function for 3d Raycaster
  // clear
  void clearQuads()
  { m_quads.clear(); }

  // add quad
  void addQuad(const ZMesh& quad);

  // For 3D Raycasting rendering, once called, 2d quads will be cleared and renderer
  // switch to 3D mode
  void setEntryExitInfo(const Z3DTexture* entryTexCoordTexture,
                        const Z3DTexture* entryEyeCoordTexture,
                        const Z3DTexture* exitTexCoordTexture,
                        const Z3DTexture* exitEyeCoordTexture);

  void setFastRendering(bool v)
  { m_fastRendering = v; }

  // return true if something is rendered by this renderer
  bool hasVisibleRendering() const;

  QString compositeMode() const;

  ZStringIntOptionParameter& compositingModePara()
  { return m_compositingMode; }

  ZFloatParameter& samplingRatePara()
  { return m_samplingRate; }

  ZFloatParameter& isoValuePara()
  { return m_isoValue; }

  ZFloatParameter& localMIPThresholdPara()
  { return m_localMIPThreshold; }

  const std::vector<std::unique_ptr<ZBoolParameter>>& channelVisibleParas() const
  { return m_channelVisibleParas; }

  const std::vector<std::unique_ptr<Z3DTransferFunctionParameter>>& transferFuncParas() const
  { return m_transferFuncParas; }

  const std::vector<std::unique_ptr<ZStringIntOptionParameter>>& texFilterModeParas() const
  { return m_texFilterModeParas; }

  void compile() override;

protected:
  void adjustWidgets();

  void bindVolumesAndTransferFuncs(Z3DShaderProgram& shader) const;

  void bindVolumeAndTransferFunc(Z3DShaderProgram& shader, size_t idx) const;

  QString generateHeader();

  void render(Z3DEye eye) override;

  void renderPicking(Z3DEye /*unused*/) override;

private:
  // this function is used to get proper default
  // transfer functions (grey or color depends on current number of channel)
  void resetTransferFunctions();

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

  Z3DRenderTarget* m_layerTarget = nullptr;
  Z3DRenderTarget* m_blockIDsRenderTarget = nullptr;

  ZFloatParameter m_samplingRate;  // Sampling rate of the raycasting, specified relative to the size of one voxel
  ZFloatParameter m_isoValue;  // The used isovalue, when isosurface raycasting is enabled
  ZFloatParameter m_localMIPThreshold;

  ZStringIntOptionParameter m_compositingMode;

  Z3DImg* m_img = nullptr;
  std::vector<QString> m_volumeUniformNames;
  std::vector<QString> m_volumeDimensionNames;
  std::vector<QString> m_transferFuncUniformNames;
  std::vector<std::unique_ptr<ZBoolParameter>> m_channelVisibleParas;
  std::vector<std::unique_ptr<Z3DTransferFunctionParameter>> m_transferFuncParas;
  std::vector<std::unique_ptr<ZStringIntOptionParameter>> m_texFilterModeParas;

private:
  std::vector<ZMesh> m_quads;
  const Z3DTexture* m_entryTexCoordTexture;
  const Z3DTexture* m_entryEyeCoordTexture;
  const Z3DTexture* m_exitTexCoordTexture;
  const Z3DTexture* m_exitEyeCoordTexture;

  bool m_opaque;
  double m_alpha; //only takes effect when m_opaque is true
  ZVertexArrayObject m_VAO;

  std::vector<uint32_t> m_blockIDs;
  bool m_fastRendering = false;
};

} // namespace nim

