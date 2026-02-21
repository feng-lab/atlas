#pragma once

#include "z3dboundedfilter.h"
#include "z3dgeometryfilter.h"
#include "z3dimgfilter.h"
#include "z3dbackgroundrenderer.h"
#include "z3dcameraparameter.h"
#include "z3dpickingmanager.h"
#include "z3dtextureblendrenderer.h"
#include "z3dtexturecopyrenderer.h"
#include "z3dtextureglowrenderer.h"
#include "z3dfontrenderer.h"
#include "zwidgetsgroup.h"
#include "z3dshaderprogram.h"
#include "zparameter.h"
#include "zoptionparameter.h"
#include "znumericparameter.h"
#include "z3dscratchresourcepool.h"
#include "z3dcompositorpass.h"

#include <array>
#include <cstdint>
#include <memory>

namespace nim {

class ZVulkanLinearScript;
struct RendererCPUState;

class Z3DCompositor : public Z3DBoundedFilter
{
  Q_OBJECT

public:
  explicit Z3DCompositor(Z3DGlobalParameters& globalParas, QObject* parent = nullptr);
  ~Z3DCompositor() override;

  bool isReady(Z3DEye eye) const override;

  std::shared_ptr<ZWidgetsGroup> backgroundWidgetsGroup();

  std::shared_ptr<ZWidgetsGroup> axisWidgetsGroup();

  void savePickingBufferToImage(const QString& filename);

  // Configure the current geometry and volume filters that participate in
  // compositing. The rendering engine owns the linear pipeline and keeps
  // these lists in sync when the scene changes.
  void setGeometryFilters(const std::vector<Z3DGeometryFilter*>& filters)
  {
    m_geometryFilters = filters;
    resetVulkanSceneBatchCaches();
  }

  void setVolumeFilters(const std::vector<Z3DImgFilter*>& filters)
  {
    m_volumeFilters = filters;
    resetVulkanSceneBatchCaches();
  }

  // Debug helpers: save the current output attachments directly to disk.
  // For Vulkan, downloads from the current ready lease attachments.
  // For OpenGL, uses the GL texture save helpers.
  void saveOutputColorToImage(const QString& filename, Z3DEye eye = MonoEye);
  void saveOutputDepthToImage(const QString& filename, Z3DEye eye = MonoEye);

  void setRenderingRegion(double left = 0., double right = 1., double bottom = 0., double top = 1.);

  Z3DRenderTarget* monoReadyTarget() const
  {
    return m_monoReadyTarget->renderTarget;
  }

  Z3DRenderTarget* leftReadyTarget() const
  {
    return m_leftReadyTarget->renderTarget;
  }

  Z3DRenderTarget* rightReadyTarget() const
  {
    return m_rightReadyTarget->renderTarget;
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

  void switchBackend(RenderBackend backend);

Q_SIGNALS:
  void sceneParaUpdated();

  void renderingFinished();

protected:
  double process(Z3DEye eye) override;

  void updateSize(const glm::uvec2& targetSize) override;

private:
  // Stage 3 (Vulkan): record a linear sequence of background + geometry segments
  // into the provided linear script. This helper is intentionally "record-only":
  // it does not decide Vulkan submission boundaries (the script owns that).
  //
  // - When includeGeometry is false, records only background into sceneOutLease.
  // - When true, records background then enqueues opaque + transparent filters.
  void recordSceneSegmentsVulkan(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                                 const std::vector<Z3DBoundedFilter*>& transparentFilters,
                                 Z3DScratchResourcePool::RenderTargetLease& sceneOutLease,
                                 Z3DEye eye,
                                 bool includeGeometry,
                                 bool clearAtStart,
                                 bool drawBackground,
                                 ZVulkanLinearScript& script);
  // little helper function
  void renderGeometries(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                        const std::vector<Z3DBoundedFilter*>& transparentFilters,
                        Z3DScratchResourcePool::RenderTargetLease& targetLease,
                        Z3DEye eye);

  void ensureOutputTargets(const glm::uvec2& size);

  void renderGeomsBlendDelayed(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                               const std::vector<Z3DBoundedFilter*>& transparentFilters,
                               Z3DScratchResourcePool::RenderTargetLease& targetLease,
                               Z3DEye eye);

  void renderGeomsBlendNoDepthMask(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                                   const std::vector<Z3DBoundedFilter*>& transparentFilters,
                                   Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                   Z3DEye eye);

  void renderGeomsOIT(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                      const std::vector<Z3DBoundedFilter*>& transparentFilters,
                      Z3DScratchResourcePool::RenderTargetLease& targetLease,
                      Z3DEye eye,
                      TransparencyMode mode);

  void renderOpaqueFilters(const std::vector<Z3DBoundedFilter*>& filters,
                           Z3DScratchResourcePool::RenderTargetLease& targetLease,
                           Z3DEye eye);

  // DDP with multiple image layers
  void renderTransparentDDP(const std::vector<Z3DBoundedFilter*>& filters,
                            Z3DScratchResourcePool::RenderTargetLease& targetLease,
                            Z3DEye eye,
                            /*nullable*/ Z3DTexture* depthTexture,
                            const std::vector<const Z3DTexture*>& imageColorTexList,
                            const std::vector<const Z3DTexture*>& imageDepthTexList);

  // Weighted Average with multiple image layers
  void renderTransparentWA(const std::vector<Z3DBoundedFilter*>& filters,
                           Z3DScratchResourcePool::RenderTargetLease& targetLease,
                           Z3DEye eye,
                           /*nullable*/ Z3DTexture* depthTexture,
                           const std::vector<const Z3DTexture*>& imageColorTexList,
                           const std::vector<const Z3DTexture*>& imageDepthTexList);

  // Weighted Blended with multiple image layers
  void renderTransparentWB(const std::vector<Z3DBoundedFilter*>& filters,
                           Z3DScratchResourcePool::RenderTargetLease& targetLease,
                           Z3DEye eye,
                           /*nullable*/ Z3DTexture* depthTexture,
                           const std::vector<const Z3DTexture*>& imageColorTexList,
                           const std::vector<const Z3DTexture*>& imageDepthTexList);

  void renderTransparentDDPVulkan(const std::vector<Z3DBoundedFilter*>& filters,
                                  Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                  Z3DEye eye,
                                  AttachmentHandle depthAttachmentHandle,
                                  const std::vector<Z3DCompositorImageLayer>& imageLayers,
                                  bool clearResolveTarget,
                                  ZVulkanLinearScript& script);

  void renderTransparentPPLLVulkan(const std::vector<Z3DBoundedFilter*>& filters,
                                   Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                   Z3DEye eye,
                                   AttachmentHandle depthAttachmentHandle,
                                   const std::vector<Z3DCompositorImageLayer>& imageLayers,
                                   bool clearResolveTarget,
                                   ZVulkanLinearScript& script);

  void renderTransparentWAVulkan(const std::vector<Z3DBoundedFilter*>& filters,
                                 Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                 Z3DEye eye,
                                 AttachmentHandle depthAttachmentHandle,
                                 const std::vector<Z3DCompositorImageLayer>& imageLayers,
                                 bool clearResolveTarget,
                                 ZVulkanLinearScript& script);

  void renderTransparentWBVulkan(const std::vector<Z3DBoundedFilter*>& filters,
                                 Z3DScratchResourcePool::RenderTargetLease& targetLease,
                                 Z3DEye eye,
                                 AttachmentHandle depthAttachmentHandle,
                                 const std::vector<Z3DCompositorImageLayer>& imageLayers,
                                 bool clearResolveTarget,
                                 ZVulkanLinearScript& script);

  // Build a list of non-opaque image layers (color/depth) from connected image filters
  std::vector<Z3DCompositorImageLayer> collectNonOpaqueImageLayers(Z3DEye eye);

  // Merge a list of image layers into a single pair using the same blending as renderImages
  bool mergeImageLayers(const std::vector<Z3DCompositorImageLayer>& layers,
                        Z3DEye eye,
                        Z3DScratchResourcePool::RenderTargetLease& targetLease,
                        const Z3DTexture*& colorTex,
                        const Z3DTexture*& depthTex);

  void renderAxis(Z3DEye eye);

  void prepareAxisData(Z3DEye eye);

  void setupAxisCamera();

  void
  renderAxisVulkan(Z3DEye eye, Z3DScratchResourcePool::RenderTargetLease& sceneOutLease, ZVulkanLinearScript& script);

  [[nodiscard]] glm::uvec4 axisViewportFor(const glm::uvec4& baseViewport) const;

  void ensureAxisCameraBackend(RenderBackend backend);

  void setClipPlanes() override {}

  static void downloadTextureToLocalColorBuffer(const Z3DTexture& tex, Z3DLocalColorBuffer& localColorBuffer);

  void updateBackgroundMode();
  void updateBackgroundFirstColor();
  void updateBackgroundSecondColor();
  void updateBackgroundOrientation();
  void ensurePickingTarget(const glm::uvec2& size);
  void ensurePickingTargetVulkan(const glm::uvec2& size);
  Z3DScratchResourcePool::RenderTargetLease& ensureDDPRenderTarget(const glm::uvec2& size);
  Z3DScratchResourcePool::RenderTargetLease& ensureWARenderTarget(const glm::uvec2& size);
  Z3DScratchResourcePool::RenderTargetLease& ensureWBRenderTarget(const glm::uvec2& size);

  // Internal helper: hooked transparent rendering with optional glow overlay
  void
  renderTransparentFilter(Z3DBoundedFilter* filter, Z3DScratchResourcePool::RenderTargetLease& targetLease, Z3DEye eye);

  double processGL(Z3DEye eye);
  double processVulkan(Z3DEye eye);

private:
  Z3DTextureBlendRenderer m_alphaBlendRenderer;
  Z3DTextureBlendRenderer m_firstOnTopBlendRenderer;
  Z3DTextureBlendRenderer m_firstOnTopRenderer;
  Z3DTextureBlendRenderer m_MIPImageAlphaBlendRenderer;
  Z3DTextureCopyRenderer m_textureCopyRenderer;
  Z3DTextureGlowRenderer m_glowRenderer;
  Z3DBackgroundRenderer m_backgroundRenderer;
  ZStringIntOptionParameter m_backgroundMode;
  ZVec4Parameter m_backgroundFirstColor;
  ZVec4Parameter m_backgroundSecondColor;
  ZStringIntOptionParameter m_backgroundGradientOrientation;

  // ZBoolParameter m_renderGeometries;

  // Current geometry and volume filters in the engine pipeline.
  std::vector<Z3DGeometryFilter*> m_geometryFilters;
  std::vector<Z3DImgFilter*> m_volumeFilters;

  Z3DScratchResourcePool::RenderTargetLease m_outRenderTarget1;
  Z3DScratchResourcePool::RenderTargetLease m_leftEyeOutRenderTarget1;

  Z3DScratchResourcePool::RenderTargetLease m_outRenderTarget2;
  Z3DScratchResourcePool::RenderTargetLease m_leftEyeOutRenderTarget2;

  Z3DScratchResourcePool::RenderTargetLease* m_monoReadyTarget = nullptr;
  Z3DScratchResourcePool::RenderTargetLease* m_leftReadyTarget = nullptr;
  Z3DScratchResourcePool::RenderTargetLease* m_rightReadyTarget = nullptr;
  Z3DScratchResourcePool::RenderTargetLease* m_monoCurrentTarget = nullptr;
  Z3DScratchResourcePool::RenderTargetLease* m_leftCurrentTarget = nullptr;
  Z3DScratchResourcePool::RenderTargetLease* m_rightCurrentTarget = nullptr;

  Z3DScratchResourcePool::RenderTargetLease m_pickingTargetLease;

  Z3DScratchResourcePool::RenderTargetLease m_ddpRTLease;
  std::unique_ptr<Z3DShaderProgram> m_ddpBlendShader;
  std::unique_ptr<Z3DShaderProgram> m_ddpFinalShader;

  Z3DScratchResourcePool::RenderTargetLease m_waRTLease;
  std::unique_ptr<Z3DShaderProgram> m_waFinalShader;

  Z3DScratchResourcePool::RenderTargetLease m_wbRTLease;
  std::unique_ptr<Z3DShaderProgram> m_wbFinalShader;

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
  ZStringIntOptionParameter m_axisFontName;
  ZFloatParameter m_axisFontSize;
  ZFloatParameter m_axisFontSoftEdgeScale;
  ZBoolParameter m_axisShowFontOutline;
  ZStringIntOptionParameter m_axisFontOutlineMode;
  ZVec4Parameter m_axisFontOutlineColor;
  ZBoolParameter m_axisShowFontShadow;
  ZVec4Parameter m_axisFontShadowColor;

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

  Z3DCamera m_axisCamera;

  // OpenGL-only: screen-quad VAO. Vulkan paths never touch this, and Vulkan
  // startup intentionally avoids creating a GL context, so this must be created
  // lazily when OpenGL is active.
  std::unique_ptr<Z3DVertexArrayObject> m_screenQuadVAO;

  glm::uvec2 m_outputSize{32u, 32u};

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

  // Vulkan async readback can complete later than newer frames; keep presentation
  // monotonic by dropping stale publishes (older perf-frame tokens) per eye.
  std::array<uint64_t, 3> m_lastPublishedPerfFrameToken{};

  struct VulkanBatchesCacheKey
  {
    glm::uvec2 targetSize{0u, 0u};
    bool clearAtStart = false;
    bool drawBackground = false;
  };

  VulkanBatchesCacheKey m_vkSceneBgGeomCacheKey{};
  std::array<std::shared_ptr<RendererCPUState>, 3> m_vkSceneBgGeomCache{};

  void resetVulkanSceneBatchCaches();

  // Z3DVertexBufferObject m_PBO;
};

} // namespace nim
