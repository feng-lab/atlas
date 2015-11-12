#ifndef Z3DCOMPOSITOR_H
#define Z3DCOMPOSITOR_H

#include "z3drenderport.h"
#include "z3dboundedfilter.h"
#include "z3dgeometryfilter.h"
#include "z3dvolumefilter.h"
#include "z3dpickingmanager.h"
#include "z3dcameraparameter.h"
#include "z3dbackgroundrenderer.h"
#include "z3dtexturecopyrenderer.h"
#include "z3dtextureblendrenderer.h"
#include "zwidgetsgroup.h"
#include "z3dinteractionhandler.h"
#include "z3dfontrenderer.h"
#include "z3dshaderprogram.h"

namespace nim {

class Z3DCompositor : public Z3DBoundedFilter
{
  Q_OBJECT
public:
  Z3DCompositor(Z3DGlobalParameters &globalParas, QObject *parent = nullptr);

  virtual bool isReady(Z3DEye eye) const override;

  std::shared_ptr<ZWidgetsGroup> backgroundWidgetsGroup();
  std::shared_ptr<ZWidgetsGroup> axisWidgetsGroup();

  void savePickingBufferToImage(const QString &filename);

protected:
  virtual void process(Z3DEye eye) override;

private:
  // little helper function
  void renderGeometries(const std::vector<Z3DBoundedFilter*> &opaqueFilters,
                        const std::vector<Z3DBoundedFilter*> &transparentFilters,
                        Z3DRenderOutputPort &port, Z3DEye eye);

  void renderGeomsBlendDelayed(const std::vector<Z3DBoundedFilter*> &opaqueFilters,
                               const std::vector<Z3DBoundedFilter*> &transparentFilters,
                               Z3DRenderOutputPort &port, Z3DEye eye);
  void renderGeomsBlendNoDepthMask(const std::vector<Z3DBoundedFilter*> &opaqueFilters,
                                   const std::vector<Z3DBoundedFilter*> &transparentFilters,
                                   Z3DRenderOutputPort &port, Z3DEye eye);
  void renderGeomsOIT(const std::vector<Z3DBoundedFilter*> &opaqueFilters,
                      const std::vector<Z3DBoundedFilter*> &transparentFilters,
                      Z3DRenderOutputPort &port, Z3DEye eye, const QString &method);

  void renderOpaqueFilters(const std::vector<Z3DBoundedFilter*> &filters, Z3DRenderOutputPort &port, Z3DEye eye);

  void renderTransparentDDP(const std::vector<Z3DBoundedFilter*> &filters,
                            Z3DRenderOutputPort &port, Z3DEye eye, Z3DTexture *depthTexture = nullptr);
  bool createDDPRenderTarget(glm::ivec2 size);

  void renderTransparentWA(const std::vector<Z3DBoundedFilter*> &filters,
                           Z3DRenderOutputPort &port, Z3DEye eye, Z3DTexture *depthTexture = nullptr);
  bool createWARenderTarget(glm::ivec2 size);

  // if image inport has more than 1 image, blend use tempport3 and tempport4,
  // send output to colorTex and depthTex
  void renderImages(Z3DRenderInputPort &currentInport, Z3DRenderOutputPort &currentOutport,
                    Z3DEye eye, const Z3DTexture *&colorTex, const Z3DTexture *&depthTex);

  void renderAxis(Z3DEye eye);
  void prepareAxisData(Z3DEye eye);
  void setupAxisCamera();

private:
  Z3DTextureBlendRenderer m_alphaBlendRenderer;
  Z3DTextureBlendRenderer m_firstOnTopBlendRenderer;
  Z3DTextureBlendRenderer m_firstOnTopRenderer;
  Z3DTextureCopyRenderer m_textureCopyRenderer;
  Z3DBackgroundRenderer m_backgroundRenderer;

  //ZBoolParameter m_renderGeometries;

  Z3DRenderInputPort m_inport;
  Z3DRenderInputPort m_leftEyeInport;
  Z3DRenderInputPort m_rightEyeInport;
  Z3DRenderOutputPort m_outport;
  Z3DRenderOutputPort m_leftEyeOutport;
  Z3DRenderOutputPort m_rightEyeOutport;
  Z3DRenderOutputPort m_tempPort;
  Z3DRenderOutputPort m_tempPort2;
  Z3DRenderOutputPort m_tempPort3;
  Z3DRenderOutputPort m_tempPort4;
  Z3DRenderOutputPort m_tempPort5;
  Z3DRenderOutputPort m_pickingPort;
  Z3DFilterInputPort<Z3DGeometryFilter> m_gPPort;
  Z3DFilterInputPort<Z3DVolumeFilter> m_vPPort;

  std::unique_ptr<Z3DRenderTarget> m_ddpRT;
  Z3DShaderProgram m_ddpBlendShader;
  Z3DShaderProgram m_ddpFinalShader;

  std::unique_ptr<Z3DRenderTarget> m_waRT;
  Z3DShaderProgram m_waFinalShader;

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

  glm::vec3 m_XEnd;
  glm::vec3 m_YEnd;
  glm::vec3 m_ZEnd;

  std::shared_ptr<ZWidgetsGroup> m_axisWidgetsGroup;

  ZVertexArrayObject m_screenQuadVAO;
};

} // namespace nim

#endif // Z3DCOMPOSITOR_H
