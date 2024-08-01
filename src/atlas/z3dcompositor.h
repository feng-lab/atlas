#pragma once

#include "z3dboundedfilter.h"
#include "z3dgeometryfilter.h"
#include "z3drenderport.h"
#include "z3dimgfilter.h"
#include "z3dbackgroundrenderer.h"
#include "z3dcameraparameter.h"
#include "z3dpickingmanager.h"
#include "z3dtextureblendrenderer.h"
#include "z3dtexturecopyrenderer.h"
#include "z3dfontrenderer.h"
#include "z3dinteractionhandler.h"
#include "zwidgetsgroup.h"
#include "z3dshaderprogram.h"

namespace nim {

class Z3DCompositor : public Z3DBoundedFilter
{
  Q_OBJECT

public:
  explicit Z3DCompositor(Z3DGlobalParameters& globalParas, QObject* parent = nullptr);

  bool isReady(Z3DEye eye) const override;

  std::shared_ptr<ZWidgetsGroup> backgroundWidgetsGroup();

  std::shared_ptr<ZWidgetsGroup> axisWidgetsGroup();

  void savePickingBufferToImage(const QString& filename);

  void setRenderingRegion(double left = 0., double right = 1., double bottom = 0., double top = 1.);

  void setOutputSize(const glm::uvec2& size);

  glm::uvec2 outputSize() const;

  Z3DRenderTarget* monoReadyTarget() const
  {
    return m_monoReadyTarget;
  }

  Z3DRenderTarget* leftReadyTarget() const
  {
    return m_leftReadyTarget;
  }

  Z3DRenderTarget* rightReadyTarget() const
  {
    return m_rightReadyTarget;
  }

  Z3DLocalColorBuffer* monoReadyLocalBuffer() const
  {
    return m_monoReadyLocalBuffer;
  }

  Z3DLocalColorBuffer* leftReadyLocalBuffer() const
  {
    return m_leftReadyLocalBuffer;
  }

  Z3DLocalColorBuffer* rightReadyLocalBuffer() const
  {
    return m_rightReadyLocalBuffer;
  }

  void invalidate(State inv) override;

  void setProgressiveRenderingMode(bool v) override;

Q_SIGNALS:
  void sceneParaUpdated();

  void renderingFinished();

protected:
  double process(Z3DEye eye) override;

  void updateSize() override;

private:
  // little helper function
  void renderGeometries(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                        const std::vector<Z3DBoundedFilter*>& transparentFilters,
                        Z3DRenderTarget& renderTarget,
                        Z3DEye eye,
                        const Z3DTexture* imageColorTex = nullptr,
                        const Z3DTexture* imageDepthTex = nullptr);

  void renderGeomsBlendDelayed(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                               const std::vector<Z3DBoundedFilter*>& transparentFilters,
                               Z3DRenderTarget& renderTarget,
                               Z3DEye eye);

  void renderGeomsBlendNoDepthMask(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                                   const std::vector<Z3DBoundedFilter*>& transparentFilters,
                                   Z3DRenderTarget& renderTarget,
                                   Z3DEye eye);

  void renderGeomsOIT(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                      const std::vector<Z3DBoundedFilter*>& transparentFilters,
                      Z3DRenderTarget& renderTarget,
                      Z3DEye eye,
                      const QString& method,
                      const Z3DTexture* imageColorTex = nullptr,
                      const Z3DTexture* imageDepthTex = nullptr);

  void renderOpaqueFilters(const std::vector<Z3DBoundedFilter*>& filters, Z3DRenderTarget& renderTarget, Z3DEye eye);

  void renderTransparentDDP(const std::vector<Z3DBoundedFilter*>& filters,
                            Z3DRenderTarget& renderTarget,
                            Z3DEye eye,
                            Z3DTexture* depthTexture = nullptr,
                            const Z3DTexture* imageColorTex = nullptr,
                            const Z3DTexture* imageDepthTex = nullptr);

  bool createDDPRenderTarget(const glm::uvec2& size);

  void renderTransparentWA(const std::vector<Z3DBoundedFilter*>& filters,
                           Z3DRenderTarget& renderTarget,
                           Z3DEye eye,
                           Z3DTexture* depthTexture = nullptr,
                           const Z3DTexture* imageColorTex = nullptr,
                           const Z3DTexture* imageDepthTex = nullptr);

  bool createWARenderTarget(const glm::uvec2& size);

  void renderTransparentWB(const std::vector<Z3DBoundedFilter*>& filters,
                           Z3DRenderTarget& renderTarget,
                           Z3DEye eye,
                           Z3DTexture* depthTexture = nullptr,
                           const Z3DTexture* imageColorTex = nullptr,
                           const Z3DTexture* imageDepthTex = nullptr);

  bool createWBRenderTarget(const glm::uvec2& size);

  // if image inport has more than 1 image, blend use tempport3 and tempport4,
  // send output to colorTex and depthTex
  void renderImages(Z3DRenderInputPort& currentInport,
                    Z3DRenderTarget& renderTarget,
                    Z3DEye eye,
                    const Z3DTexture*& colorTex,
                    const Z3DTexture*& depthTex);

  void renderAxis(Z3DEye eye);

  void prepareAxisData(Z3DEye eye);

  void setupAxisCamera();

  void setClipPlanes() override {}

  static void downloadTextureToLocalColorBuffer(const Z3DTexture& tex, Z3DLocalColorBuffer& localColorBuffer);

private:
  Z3DTextureBlendRenderer m_alphaBlendRenderer;
  Z3DTextureBlendRenderer m_firstOnTopBlendRenderer;
  Z3DTextureBlendRenderer m_firstOnTopRenderer;
  Z3DTextureBlendRenderer m_MIPImageAlphaBlendRenderer;
  Z3DTextureCopyRenderer m_textureCopyRenderer;
  Z3DBackgroundRenderer m_backgroundRenderer;

  // ZBoolParameter m_renderGeometries;

  Z3DRenderInputPort m_inport;
  Z3DRenderInputPort m_leftEyeInport;
  Z3DRenderInputPort m_rightEyeInport;
  Z3DFilterInputPort<Z3DGeometryFilter> m_gPPort;
  Z3DFilterInputPort<Z3DImgFilter> m_vPPort;

  Z3DRenderTarget m_outRenderTarget1;
  Z3DRenderTarget m_leftEyeOutRenderTarget1;

  Z3DRenderTarget m_outRenderTarget2;
  Z3DRenderTarget m_leftEyeOutRenderTarget2;

  Z3DRenderTarget* m_monoReadyTarget = nullptr;
  Z3DRenderTarget* m_leftReadyTarget = nullptr;
  Z3DRenderTarget* m_rightReadyTarget = nullptr;
  Z3DRenderTarget* m_monoCurrentTarget = nullptr;
  Z3DRenderTarget* m_leftCurrentTarget = nullptr;
  Z3DRenderTarget* m_rightCurrentTarget = nullptr;

  Z3DRenderTarget m_tempRenderTarget1;
  Z3DRenderTarget m_tempRenderTarget2;
  Z3DRenderTarget m_tempRenderTarget3;
  Z3DRenderTarget m_tempRenderTarget4;
  Z3DRenderTarget m_tempRenderTarget5;
  Z3DRenderTarget m_imgTempRenderTarget1;
  Z3DRenderTarget m_imgTempRenderTarget2;
  Z3DRenderTarget m_pickingRenderTarget;

  std::unique_ptr<Z3DRenderTarget> m_ddpRT;
  Z3DShaderProgram m_ddpBlendShader;
  Z3DShaderProgram m_ddpFinalShader;

  std::unique_ptr<Z3DRenderTarget> m_waRT;
  Z3DShaderProgram m_waFinalShader;

  std::unique_ptr<Z3DRenderTarget> m_wbRT;
  Z3DShaderProgram m_wbFinalShader;

  ZBoolParameter m_showBackground;
  std::shared_ptr<ZWidgetsGroup> m_backgroundWidgetsGroup;

  Z3DLineRenderer m_lineRenderer;
  Z3DArrowRenderer m_arrowRenderer;
  Z3DFontRenderer m_fontRenderer;

  ZBoolParameter m_showAxis;
  ZVec4Parameter m_XAxisColor;
  ZVec4Parameter m_YAxisColor;
  ZVec4Parameter m_ZAxisColor;
  ZFloatParameter m_axisRegionRatio;
  ZStringIntOptionParameter m_axisMode;

  std::vector<glm::vec4> m_tailPosAndTailRadius;
  std::vector<glm::vec4> m_headPosAndHeadRadius;
  std::vector<glm::vec4> m_lineColors;
  std::vector<glm::vec3> m_lines;
  std::vector<glm::vec4> m_textColors;
  std::vector<glm::vec3> m_textPositions;

  glm::vec3 m_XEnd{};
  glm::vec3 m_YEnd{};
  glm::vec3 m_ZEnd{};

  std::shared_ptr<ZWidgetsGroup> m_axisWidgetsGroup;

  Z3DVertexArrayObject m_screenQuadVAO;

  glm::vec4 m_region;

  bool m_progressiveRendering = false;

  Z3DLocalColorBuffer m_localColorBuffer1 = {};
  Z3DLocalColorBuffer m_leftLocalColorBuffer1 = {};

  Z3DLocalColorBuffer m_localColorBuffer2 = {};
  Z3DLocalColorBuffer m_leftLocalColorBuffer2 = {};

  Z3DLocalColorBuffer* m_monoReadyLocalBuffer = nullptr;
  Z3DLocalColorBuffer* m_leftReadyLocalBuffer = nullptr;
  Z3DLocalColorBuffer* m_rightReadyLocalBuffer = nullptr;
  Z3DLocalColorBuffer* m_monoCurrentLocalBuffer = nullptr;
  Z3DLocalColorBuffer* m_leftCurrentLocalBuffer = nullptr;
  Z3DLocalColorBuffer* m_rightCurrentLocalBuffer = nullptr;

  // Z3DVertexBufferObject m_PBO;
};

} // namespace nim
