#ifndef Z3DVOLUMERAYCASTERRENDERER_H
#define Z3DVOLUMERAYCASTERRENDERER_H

#include "z3dprimitiverenderer.h"
#include "z3dtransferfunction.h"
#include "zmesh.h"
#include "z3dshaderprogram.h"

namespace nim {

// use raycaster to render volume or 2D Image (stack with depth==1) with color
// transfer functions
// only support up to 20 channels now.
// use python script to generate new shader to support more channels
class Z3DVolumeRaycasterRenderer : public Z3DPrimitiveRenderer
{
  Q_OBJECT
public:
  explicit Z3DVolumeRaycasterRenderer(Z3DRendererBase &rendererBase);

  // input vols can not be nullptr
  void setChannels(const std::vector<std::unique_ptr<Z3DVolume> > &vols);

  // quad or entryexit texture should be set before rendering

  // For 2D Image rendering, once set, entry exit textures will be cleared and
  // renderer switch to 2D mode
  // To render a 2D image, quad should contains corner vertex and 2d texture coordinates
  // To render a slice in 3D volume, quad should contains corner vertex and 3d texture coordinates
  // DO NOT call this function for 3d Raycaster
  // clear
  void clearQuads() { m_quads.clear(); }
  // add quad
  void addQuad(const ZMesh &quad);
  // For 3D Raycasting rendering, once called, 2d quads will be cleared and renderer
  // switch to 3D mode
  void setEntryExitCoordTextures(const Z3DTexture *entryCoordTexture,
                                 const Z3DTexture *entryDepthTexture,
                                 const Z3DTexture *exitCoordTexture,
                                 const Z3DTexture *exitDepthTexture);

  void translate(double dx, double dy, double dz);

  // return true if something is rendered by this renderer
  bool hasVisibleRendering() const;

  QString compositeMode() const;

  ZStringIntOptionParameter& compositingModePara() { return m_compositingMode; }
  ZFloatParameter& samplingRatePara() { return m_samplingRate; }
  ZFloatParameter& isoValuePara() { return m_isoValue; }
  ZFloatParameter& localMIPThresholdPara() { return m_localMIPThreshold; }
  const std::vector<std::unique_ptr<ZBoolParameter> >& channelVisibleParas() const { return m_channelVisibleParas; }
  const std::vector<std::unique_ptr<Z3DTransferFunctionParameter> >& transferFuncParas() const { return m_transferFuncParas; }
  const std::vector<std::unique_ptr<ZStringIntOptionParameter> >& texFilterModeParas() const { return m_texFilterModeParas; }

signals:

protected slots:
  void adjustWidgets();

protected:
  void bindVolumesAndTransferFuncs(Z3DShaderProgram &shader);

  virtual void compile() override;
  QString generateHeader();

  virtual void render(Z3DEye eye) override;
  virtual void renderPicking(Z3DEye) override;

  Z3DShaderProgram m_raycasterShader;
  Z3DShaderProgram m_2dImageShader;
  Z3DShaderProgram m_volumeSliceWithTransferfunShader;

  ZFloatParameter m_samplingRate;  // Sampling rate of the raycasting, specified relative to the size of one voxel
  ZFloatParameter m_isoValue;  // The used isovalue, when isosurface raycasting is enabled
  ZFloatParameter m_localMIPThreshold;

  ZStringIntOptionParameter m_compositingMode;

  std::vector<Z3DVolume *> m_volumes;
  std::vector<QString> m_volumeUniformNames;
  std::vector<QString> m_transferFuncUniformNames;
  std::vector<std::unique_ptr<ZBoolParameter> > m_channelVisibleParas;
  std::vector<std::unique_ptr<Z3DTransferFunctionParameter> > m_transferFuncParas;
  std::vector<std::unique_ptr<ZStringIntOptionParameter> > m_texFilterModeParas;

private:
  // this function is used to get proper default
  // transfer functions (grey or color depends on current number of channel)
  void resetTransferFunctions();

  bool m_is2DImage;

  std::vector<ZMesh> m_quads;
  const Z3DTexture *m_entryCoordTexture;
  const Z3DTexture *m_entryDepthTexture;
  const Z3DTexture *m_exitCoordTexture;
  const Z3DTexture *m_exitDepthTexture;

  bool m_opaque;
  double m_alpha; //only takes effect when m_opaque is true
};

} // namespace nim

#endif // Z3DVOLUMERAYCASTERRENDERER_H
