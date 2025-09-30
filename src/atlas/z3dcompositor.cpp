#include "z3dcompositor.h"

#include "z3dgl.h"
#include "z3drendertarget.h"
#include "z3dtexture.h"
#include "z3drenderglobalstate.h"
#include "zbenchtimer.h"
#include "z3dscratchresourcepool.h"
#include "zlog.h"
#include <algorithm>

namespace nim {

namespace {

inline void prepareFilterForTarget(Z3DBoundedFilter& filter, Z3DRenderTarget& target)
{
  filter.setViewport(target.size());
  filter.setActiveSurfaceFromRenderTarget(target);
}

inline void prepareFilterForLease(Z3DBoundedFilter& filter, Z3DScratchResourcePool::RenderTargetLease& lease)
{
  if (lease.hasGLRenderTarget()) {
    prepareFilterForTarget(filter, lease.glRenderTarget());
  } else {
    filter.setViewport(lease.descriptor.size);
    filter.setActiveSurfaceFromLease(lease);
  }
}

} // namespace

Z3DCompositor::Z3DCompositor(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DBoundedFilter(globalParas, parent)
  , m_alphaBlendRenderer(m_rendererBase, TextureBlendMode::DepthTestBlending)
  , m_firstOnTopBlendRenderer(m_rendererBase, TextureBlendMode::FirstOnTopBlending)
  , m_firstOnTopRenderer(m_rendererBase, TextureBlendMode::FirstOnTop)
  , m_MIPImageAlphaBlendRenderer(m_rendererBase, TextureBlendMode::MIPImageDepthTestBlending)
  , m_textureCopyRenderer(m_rendererBase)
  , m_glowRenderer(m_rendererBase)
  , m_backgroundRenderer(m_rendererBase)
  , m_backgroundMode("Background Mode")
  , m_backgroundFirstColor("First Color", glm::vec4(1.0f, 1.0f, 1.0f, 1.0f))
  , m_backgroundSecondColor("Second Color", glm::vec4(0.2f, 0.2f, 0.2f, 1.0f))
  , m_backgroundGradientOrientation("Gradient Orientation")
  //, m_renderGeometries("Render Geometries", true)
  , m_gPPort("GeometryFilters", true, this)
  , m_vPPort("VolumeFilters", true, this)
  , m_ddpBlendShader()
  , m_ddpFinalShader()
  , m_waFinalShader()
  , m_wbFinalShader()
  , m_showBackground("Show Background", true)
  , m_lineRenderer(m_rendererBase)
  , m_arrowRenderer(m_rendererBase)
  , m_fontRenderer(m_rendererBase)
  , m_showAxis("Show Axis", true)
  , m_XAxisColor("X Axis Color", glm::vec4(1.f, 0.f, 0.f, 1.0f))
  , m_YAxisColor("Y Axis Color", glm::vec4(0.f, 1.f, 0.f, 1.0f))
  , m_ZAxisColor("Z Axis Color", glm::vec4(0.f, 0.f, 1.f, 1.0f))
  , m_axisRegionRatio("Axis Region Ratio", .25f, .1f, 1.f)
  , m_axisMode("Mode")
  , m_axisFontName("Font")
  , m_axisFontSize("Font Size", 32.f, .1f, 5000.f)
  , m_axisFontSoftEdgeScale("Font Softedge Scale", 80.f, 0.f, 200.f)
  , m_axisShowFontOutline("Show Font Outline", false)
  , m_axisFontOutlineMode("Font Outline Mode")
  , m_axisFontOutlineColor("Font Outline Color", glm::vec4(1.f))
  , m_axisShowFontShadow("Show Font Shadow", false)
  , m_axisFontShadowColor("Font Shadow Color", glm::vec4(0.f, 0.f, 0.f, 1.f))
  , m_screenQuadVAO(1)
  , m_region(0, 1, 0, 1)
{
  m_monoCurrentTarget = &m_outRenderTarget1;
  m_leftCurrentTarget = &m_leftEyeOutRenderTarget1;
  m_rightCurrentTarget = &m_outRenderTarget1;

  m_monoCurrentLocalBuffer = &m_localColorBuffer1;
  m_leftCurrentLocalBuffer = &m_leftLocalColorBuffer1;
  m_rightCurrentLocalBuffer = &m_localColorBuffer1;

  addParameter(m_showBackground);

  addPort(m_gPPort);
  addPort(m_vPPort);

  m_textureCopyRenderer.setDiscardTransparent(true);
  m_backgroundFirstColor.setStyle("COLOR");
  m_backgroundSecondColor.setStyle("COLOR");
  m_backgroundMode.clearOptions();
  m_backgroundMode.addOptionsWithData(
    std::make_pair(enumToQString(BackgroundMode::Uniform), static_cast<int>(BackgroundMode::Uniform)),
    std::make_pair(enumToQString(BackgroundMode::Gradient), static_cast<int>(BackgroundMode::Gradient)));
  m_backgroundMode.select(enumToQString(BackgroundMode::Gradient));
  m_backgroundGradientOrientation.clearOptions();
  m_backgroundGradientOrientation.addOptionsWithData(
    std::make_pair(enumToQString(BackgroundGradientOrientation::LeftToRight),
                   static_cast<int>(BackgroundGradientOrientation::LeftToRight)),
    std::make_pair(enumToQString(BackgroundGradientOrientation::RightToLeft),
                   static_cast<int>(BackgroundGradientOrientation::RightToLeft)),
    std::make_pair(enumToQString(BackgroundGradientOrientation::TopToBottom),
                   static_cast<int>(BackgroundGradientOrientation::TopToBottom)),
    std::make_pair(enumToQString(BackgroundGradientOrientation::BottomToTop),
                   static_cast<int>(BackgroundGradientOrientation::BottomToTop)));
  m_backgroundGradientOrientation.select(enumToQString(BackgroundGradientOrientation::BottomToTop));
  addParameter(m_backgroundMode);
  addParameter(m_backgroundFirstColor);
  addParameter(m_backgroundSecondColor);
  addParameter(m_backgroundGradientOrientation);

  updateBackgroundFirstColor();
  updateBackgroundSecondColor();
  updateBackgroundOrientation();
  updateBackgroundMode();

  connect(&m_backgroundMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DCompositor::updateBackgroundMode);
  connect(&m_backgroundFirstColor, &ZVec4Parameter::valueChanged, this, &Z3DCompositor::updateBackgroundFirstColor);
  connect(&m_backgroundSecondColor, &ZVec4Parameter::valueChanged, this, &Z3DCompositor::updateBackgroundSecondColor);
  connect(&m_backgroundGradientOrientation,
          &ZStringIntOptionParameter::valueChanged,
          this,
          &Z3DCompositor::updateBackgroundOrientation);

  // Glow renderer is compositor-owned; parameters are synced from filters per-draw

  m_waFinalShader.loadFromSourceFile("pass.vert", "wavg_final.frag", m_rendererBase.generateHeader());

  m_wbFinalShader.loadFromSourceFile("pass.vert", "wblended_final.frag", m_rendererBase.generateHeader());

  m_ddpBlendShader.loadFromSourceFile("pass.vert", "dual_peeling_blend.frag", m_rendererBase.generateHeader());

  m_ddpFinalShader.loadFromSourceFile("pass.vert", "dual_peeling_final.frag", m_rendererBase.generateHeader());

  ensurePickingTarget(glm::uvec2(32u, 32u));
  addInteractionHandler(globalParas.interactionHandler);

  m_XAxisColor.setStyle("COLOR");
  m_YAxisColor.setStyle("COLOR");
  m_ZAxisColor.setStyle("COLOR");
  m_axisMode.addOptions("Arrow", "Line");
  m_axisMode.select("Arrow");
  m_axisFontSize.setSingleStep(0.1);
  m_axisFontSize.setDecimal(1);
  m_axisFontSoftEdgeScale.setSingleStep(1.0);
  m_axisFontOutlineMode.clearOptions();
  m_axisFontOutlineMode.addOptionsWithData(
    std::make_pair(enumToQString(FontOutlineMode::Glow), static_cast<int>(FontOutlineMode::Glow)),
    std::make_pair(enumToQString(FontOutlineMode::Outline), static_cast<int>(FontOutlineMode::Outline)));
  m_axisFontOutlineMode.select(enumToQString(FontOutlineMode::Glow));
  m_axisFontOutlineColor.setStyle("COLOR");
  m_axisFontShadowColor.setStyle("COLOR");
  addParameter(m_showAxis);
  addParameter(m_XAxisColor);
  addParameter(m_YAxisColor);
  addParameter(m_ZAxisColor);
  addParameter(m_axisRegionRatio);
  addParameter(m_axisMode);
  addParameter(m_axisFontName);
  addParameter(m_axisFontSize);
  addParameter(m_axisFontSoftEdgeScale);
  addParameter(m_axisShowFontOutline);
  addParameter(m_axisFontOutlineMode);
  addParameter(m_axisFontOutlineColor);
  addParameter(m_axisShowFontShadow);
  addParameter(m_axisFontShadowColor);

  if (!m_fontRenderer.fontNames().isEmpty()) {
    int idx = 0;
    for (const auto& name : m_fontRenderer.fontNames()) {
      m_axisFontName.addOptionWithData(std::make_pair(name, idx++));
    }
    m_axisFontName.select(m_fontRenderer.selectedFontName());
  } else {
    m_axisFontName.setVisible(false);
  }

  auto updateAxisFontWidgets = [this]() {
    const bool outlineVisible = m_axisShowFontOutline.get();
    m_axisFontOutlineMode.setVisible(outlineVisible);
    m_axisFontOutlineColor.setVisible(outlineVisible);
    m_axisFontShadowColor.setVisible(m_axisShowFontShadow.get());
  };

  connect(&m_axisFontName, &ZStringIntOptionParameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontName(m_axisFontName.get());
  });
  connect(&m_axisFontSize, &ZFloatParameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontSize(m_axisFontSize.get());
  });
  connect(&m_axisFontSoftEdgeScale, &ZFloatParameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontSoftEdgeScale(m_axisFontSoftEdgeScale.get());
  });
  connect(&m_axisShowFontOutline, &ZBoolParameter::valueChanged, this, [this, updateAxisFontWidgets]() mutable {
    m_fontRenderer.setShowFontOutline(m_axisShowFontOutline.get());
    updateAxisFontWidgets();
  });
  connect(&m_axisFontOutlineMode, &ZStringIntOptionParameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontOutlineMode(static_cast<FontOutlineMode>(m_axisFontOutlineMode.associatedData()));
  });
  connect(&m_axisFontOutlineColor, &ZVec4Parameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontOutlineColor(m_axisFontOutlineColor.get());
  });
  connect(&m_axisShowFontShadow, &ZBoolParameter::valueChanged, this, [this, updateAxisFontWidgets]() mutable {
    m_fontRenderer.setShowFontShadow(m_axisShowFontShadow.get());
    updateAxisFontWidgets();
  });
  connect(&m_axisFontShadowColor, &ZVec4Parameter::valueChanged, this, [this]() {
    m_fontRenderer.setFontShadowColor(m_axisFontShadowColor.get());
  });

  m_fontRenderer.setFontName(m_axisFontName.get());
  m_fontRenderer.setFontSize(m_axisFontSize.get());
  m_fontRenderer.setFontSoftEdgeScale(m_axisFontSoftEdgeScale.get());
  m_fontRenderer.setShowFontOutline(m_axisShowFontOutline.get());
  m_fontRenderer.setFontOutlineMode(static_cast<FontOutlineMode>(m_axisFontOutlineMode.associatedData()));
  m_fontRenderer.setFontOutlineColor(m_axisFontOutlineColor.get());
  m_fontRenderer.setShowFontShadow(m_axisShowFontShadow.get());
  m_fontRenderer.setFontShadowColor(m_axisFontShadowColor.get());
  updateAxisFontWidgets();
#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  m_arrowRenderer.setUseDisplayList(false);
  m_lineRenderer.setUseDisplayList(false);
#endif
  m_fontRenderer.setFollowCoordTransform(false);
  setupAxisCamera();

  for (auto* para : m_globalParameters.parameters()) {
    connect(para, &ZParameter::valueChanged, this, &Z3DBoundedFilter::invalidateResult);
  }
}

void Z3DCompositor::updateBackgroundMode()
{
  const auto mode = static_cast<BackgroundMode>(m_backgroundMode.associatedData());
  m_backgroundRenderer.setMode(mode);

  m_backgroundFirstColor.setVisible(true);
  const bool useGradient = mode == BackgroundMode::Gradient;
  m_backgroundSecondColor.setVisible(useGradient);
  m_backgroundGradientOrientation.setVisible(useGradient);
}

void Z3DCompositor::updateBackgroundFirstColor()
{
  m_backgroundRenderer.setFirstColor(m_backgroundFirstColor.get());
}

void Z3DCompositor::updateBackgroundSecondColor()
{
  m_backgroundRenderer.setSecondColor(m_backgroundSecondColor.get());
}

void Z3DCompositor::updateBackgroundOrientation()
{
  m_backgroundRenderer.setGradientOrientation(
    static_cast<BackgroundGradientOrientation>(m_backgroundGradientOrientation.associatedData()));
}

bool Z3DCompositor::isReady(Z3DEye) const
{
  return true;
}

std::shared_ptr<ZWidgetsGroup> Z3DCompositor::backgroundWidgetsGroup()
{
  if (!m_backgroundWidgetsGroup) {
    m_backgroundWidgetsGroup = std::make_shared<ZWidgetsGroup>("Background", 1);
    m_backgroundWidgetsGroup->addChild(m_showBackground, 1);
    m_backgroundWidgetsGroup->addChild(m_backgroundMode, 1);
    m_backgroundWidgetsGroup->addChild(m_backgroundFirstColor, 1);
    m_backgroundWidgetsGroup->addChild(m_backgroundSecondColor, 1);
    m_backgroundWidgetsGroup->addChild(m_backgroundGradientOrientation, 1);
    m_backgroundWidgetsGroup->setBasicAdvancedCutoff(4);
  }
  return m_backgroundWidgetsGroup;
}

std::shared_ptr<ZWidgetsGroup> Z3DCompositor::axisWidgetsGroup()
{
  if (!m_axisWidgetsGroup) {
    m_axisWidgetsGroup = std::make_shared<ZWidgetsGroup>("Axis", 1);
    m_axisWidgetsGroup->addChild(m_showAxis, 1);
    m_axisWidgetsGroup->addChild(m_axisMode, 1);
    m_axisWidgetsGroup->addChild(m_axisRegionRatio, 1);
    m_axisWidgetsGroup->addChild(m_XAxisColor, 1);
    m_axisWidgetsGroup->addChild(m_YAxisColor, 1);
    m_axisWidgetsGroup->addChild(m_ZAxisColor, 1);
    auto& rendererParas = m_rendererParameters;
    m_axisWidgetsGroup->addChild(rendererParas.sizeScale, 1);
    m_axisWidgetsGroup->addChild(rendererParas.opacity, 3);
    m_axisWidgetsGroup->addChild(m_axisFontName, 4);
    m_axisWidgetsGroup->addChild(m_axisFontSize, 4);
    m_axisWidgetsGroup->addChild(m_axisFontSoftEdgeScale, 4);
    m_axisWidgetsGroup->addChild(m_axisShowFontOutline, 4);
    m_axisWidgetsGroup->addChild(m_axisFontOutlineMode, 4);
    m_axisWidgetsGroup->addChild(m_axisFontOutlineColor, 4);
    m_axisWidgetsGroup->addChild(m_axisShowFontShadow, 4);
    m_axisWidgetsGroup->addChild(m_axisFontShadowColor, 4);
    m_axisWidgetsGroup->setBasicAdvancedCutoff(5);
  }
  return m_axisWidgetsGroup;
}

void Z3DCompositor::savePickingBufferToImage(const QString& filename)
{
  const Z3DTexture* tex = pickingManager().renderTarget().attachment(GL_COLOR_ATTACHMENT0);
  tex->saveAsColorImage(filename);
}

void Z3DCompositor::setRenderingRegion(double left, double right, double bottom, double top)
{
  m_backgroundRenderer.setRenderingRegion(left, right, bottom, top);
  m_region = glm::vec4(left, right - left, bottom, top - bottom);
}

void Z3DCompositor::setOutputSize(const glm::uvec2& size)
{
  if (size == m_monoCurrentTarget->size()) {
    return;
  }

  m_monoCurrentTarget->resize(size);
  m_leftCurrentTarget->resize(size);
  m_rightCurrentTarget->resize(size);

  if (size != m_vPPort.expectedSize()) {
    m_vPPort.setExpectedSize(size);
    m_globalParameters.camera.viewportChanged(size);
    Q_EMIT requestUpstreamSizeChange(this);
  }
}

glm::uvec2 Z3DCompositor::outputSize() const
{
  return m_outRenderTarget1.size();
}

void Z3DCompositor::invalidate(State inv)
{
  // VLOG(1) << "1";
  CHECK(inv != State::Valid);
  if (isFlagSet(m_state, inv)) {
    return;
  }
  // VLOG(1) << std::to_underlying(m_state) << " " << to_underlying(inv);
  setFlag(m_state, inv);

  Q_EMIT sceneParaUpdated();
}

void Z3DCompositor::setProgressiveRenderingMode(bool v)
{
  m_progressiveRendering = v;
}

double Z3DCompositor::process(Z3DEye eye)
{
  syncRendererState();

  std::vector<Z3DGeometryFilter*> filters = m_gPPort.connectedFilters();
  std::vector<Z3DImgFilter*> vFilters = m_vPPort.connectedFilters();
  // VLOG(1) << filters.size() << " " << vFilters.size();
  std::vector<Z3DBoundedFilter*> onTopOpaqueFilters;
  std::vector<Z3DBoundedFilter*> onTopTransparentFilters;
  std::vector<Z3DBoundedFilter*> normalOpaqueFilters;
  std::vector<Z3DBoundedFilter*> normalTransparentFilters;
  std::vector<Z3DBoundedFilter*> selectedFilters;
  std::vector<Z3DBoundedFilter*> showHandleFilters;

  const auto transparencyMode = static_cast<TransparencyMode>(m_globalParameters.transparencyMethod.associatedData());
  const bool multisample2x2 =
    static_cast<GeometryMSAAMode>(m_globalParameters.geometriesMultisampleMode.associatedData()) ==
    GeometryMSAAMode::MSAA2x2;
  for (auto vFilter : vFilters) {
    if (vFilter->isReady(eye) && vFilter->hasOpaque(eye)) {
      normalOpaqueFilters.push_back(vFilter);
    }
    if (vFilter->isSelected()) {
      selectedFilters.push_back(vFilter);
      if (vFilter->isTransformEnabled()) {
        showHandleFilters.push_back(vFilter);
      }
    }
  }
  // if (m_renderGeometries.get()) {
  for (auto geomFilter : filters) {
    if (geomFilter->isReady(eye) && (geomFilter->opacity() > 0.0)) {
      if (geomFilter->hasOpaque(eye)) {
        if (geomFilter->isStayOnTop()) {
          onTopOpaqueFilters.push_back(geomFilter);
        } else {
          normalOpaqueFilters.push_back(geomFilter);
        }
      }
      if (geomFilter->hasTransparent(eye)) {
        if (geomFilter->isStayOnTop()) {
          onTopTransparentFilters.push_back(geomFilter);
        } else {
          normalTransparentFilters.push_back(geomFilter);
        }
      }
    }
    if (geomFilter->isSelected()) {
      selectedFilters.push_back(geomFilter);
      if (geomFilter->isTransformEnabled()) {
        showHandleFilters.push_back(geomFilter);
      }
    }
  }
  //}
  size_t numNormalFilters = normalOpaqueFilters.size() + normalTransparentFilters.size();
  size_t numOnTopFilters = onTopOpaqueFilters.size() + onTopTransparentFilters.size();

  // If we need to overlay handles, render the scene into a pooled temp first
  Z3DScratchResourcePool::RenderTargetLease overlayLease;
  Z3DRenderTarget* currentOutPtr = nullptr;
  if (!showHandleFilters.empty()) {
    overlayLease =
      Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(m_monoCurrentTarget->size());
    // VLOG(1) << "lease acquired";
    currentOutPtr = overlayLease.renderTarget;
  } else {
    currentOutPtr = (eye == MonoEye)   ? m_monoCurrentTarget
                    : (eye == LeftEye) ? m_leftCurrentTarget
                                       : m_rightCurrentTarget;
  }
  auto& currentOutRenderTarget = *currentOutPtr;

  const bool anyVolumeReady = std::any_of(vFilters.begin(), vFilters.end(), [eye](const Z3DImgFilter* filter) {
    return filter && filter->isReady(eye) && filter->hasTransparent(eye);
  });

  glEnable(GL_DEPTH_TEST);

  if (transparencyMode == TransparencyMode::BlendNoDepthMask || transparencyMode == TransparencyMode::BlendDelayed) {
    if (!anyVolumeReady) { // no volume, only geometrys to render
      if (numNormalFilters == 0 || numOnTopFilters == 0) {
        // Acquire temp for geometry-only path (optionally twice the size)
        Z3DScratchResourcePool::RenderTargetLease temp1Lease =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(
            multisample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size());
        // VLOG(1) << "lease acquired";

        if (numOnTopFilters == 0) {
          renderGeometries(normalOpaqueFilters, normalTransparentFilters, *temp1Lease.renderTarget, eye);
        } else {
          renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, *temp1Lease.renderTarget, eye);
        }

        // copy to outport
        currentOutRenderTarget.bind();
        currentOutRenderTarget.clear();
        prepareFilterForTarget(*this, currentOutRenderTarget);

        if (m_showBackground.get()) {
          m_rendererBase.render(eye, m_backgroundRenderer);
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        glDepthFunc(GL_ALWAYS);
        m_textureCopyRenderer.setColorTexture(temp1Lease.renderTarget->colorTexture());
        m_textureCopyRenderer.setDepthTexture(temp1Lease.renderTarget->depthTexture());
        m_rendererBase.render(eye, m_textureCopyRenderer);
        glDepthFunc(GL_LESS);
        if (m_showAxis.get()) {
          if (!m_showBackground.get()) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
          }
          renderAxis(eye);
        }
        if (m_showBackground.get() || m_showAxis.get()) {
          glBlendFunc(GL_ONE, GL_ZERO);
          glDisable(GL_BLEND);
        }

        currentOutRenderTarget.release();
      } else {
        auto tempSize = multisample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size();
        Z3DScratchResourcePool::RenderTargetLease temp1Lease =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize);
        // VLOG(1) << "lease acquired";
        Z3DScratchResourcePool::RenderTargetLease temp2Lease =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize);
        // VLOG(1) << "lease acquired";

        // render normal geometries to tempport
        renderGeometries(normalOpaqueFilters, normalTransparentFilters, *temp1Lease.renderTarget, eye);

        // render on top geometries to tempport2
        renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, *temp2Lease.renderTarget, eye);

        // blend to output
        currentOutRenderTarget.bind();
        currentOutRenderTarget.clear();
        prepareFilterForTarget(*this, currentOutRenderTarget);

        if (m_showBackground.get()) {
          m_rendererBase.render(eye, m_backgroundRenderer);
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        m_firstOnTopBlendRenderer.setColorTexture1(temp2Lease.renderTarget->colorTexture());
        m_firstOnTopBlendRenderer.setDepthTexture1(temp2Lease.renderTarget->depthTexture());
        m_firstOnTopBlendRenderer.setColorTexture2(temp1Lease.renderTarget->colorTexture());
        m_firstOnTopBlendRenderer.setDepthTexture2(temp1Lease.renderTarget->depthTexture());
        m_rendererBase.render(eye, m_firstOnTopBlendRenderer);
        if (m_showAxis.get()) {
          if (!m_showBackground.get()) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
          }
          renderAxis(eye);
        }
        if (m_showBackground.get() || m_showAxis.get()) {
          glBlendFunc(GL_ONE, GL_ZERO);
          glDisable(GL_BLEND);
        }

        currentOutRenderTarget.release();
      }
    } else { // with volume
      // Only collect non-opaque image layers; opaque ones were rendered via the opaque pass
      auto nonOpaqueLayers = collectNonOpaqueImageLayers(eye);
      CHECK(!nonOpaqueLayers.empty()) << "should have images";
      if (numNormalFilters == 0 && numOnTopFilters == 0) { // directly copy inport image to outport
        const Z3DTexture* colorTex = nullptr;
        const Z3DTexture* depthTex = nullptr;
        mergeImageLayers(nonOpaqueLayers, eye, currentOutRenderTarget, colorTex, depthTex);

        currentOutRenderTarget.bind();
        currentOutRenderTarget.clear();
        setViewport(currentOutRenderTarget.size());

        if (m_showBackground.get()) {
          m_rendererBase.render(eye, m_backgroundRenderer);
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        glDepthFunc(GL_ALWAYS);
        m_textureCopyRenderer.setColorTexture(colorTex);
        m_textureCopyRenderer.setDepthTexture(depthTex);
        m_rendererBase.render(eye, m_textureCopyRenderer);
        glDepthFunc(GL_LESS);
        if (m_showAxis.get()) {
          if (!m_showBackground.get()) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
          }
          renderAxis(eye);
        }
        if (m_showBackground.get() || m_showAxis.get()) {
          glBlendFunc(GL_ONE, GL_ZERO);
          glDisable(GL_BLEND);
        }

        currentOutRenderTarget.release();
      } else if (numNormalFilters == 0 ||
                 numOnTopFilters == 0) { // render geometries into one temp port then blend with volume
        Z3DScratchResourcePool::RenderTargetLease tempGeoLease =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(
            multisample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size());
        // VLOG(1) << "lease acquired";

        // render geometries into one temp port
        if (numOnTopFilters == 0) {
          renderGeometries(normalOpaqueFilters, normalTransparentFilters, *tempGeoLease.renderTarget, eye);
        } else {
          renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, *tempGeoLease.renderTarget, eye);
        }

        const Z3DTexture* colorTex = nullptr;
        const Z3DTexture* depthTex = nullptr;
        mergeImageLayers(nonOpaqueLayers, eye, currentOutRenderTarget, colorTex, depthTex);

        // blend tempPort with volume
        currentOutRenderTarget.bind();
        currentOutRenderTarget.clear();
        setViewport(currentOutRenderTarget.size());

        if (m_showBackground.get()) {
          m_rendererBase.render(eye, m_backgroundRenderer);
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }

        if (numOnTopFilters == 0) {
          m_alphaBlendRenderer.setColorTexture1(tempGeoLease.renderTarget->colorTexture());
          m_alphaBlendRenderer.setDepthTexture1(tempGeoLease.renderTarget->depthTexture());
          m_alphaBlendRenderer.setColorTexture2(colorTex);
          m_alphaBlendRenderer.setDepthTexture2(depthTex);
          m_rendererBase.render(eye, m_alphaBlendRenderer);
        } else {
          m_firstOnTopBlendRenderer.setColorTexture1(tempGeoLease.renderTarget->colorTexture());
          m_firstOnTopBlendRenderer.setDepthTexture1(tempGeoLease.renderTarget->depthTexture());
          m_firstOnTopBlendRenderer.setColorTexture2(colorTex);
          m_firstOnTopBlendRenderer.setDepthTexture2(depthTex);
          m_rendererBase.render(eye, m_firstOnTopBlendRenderer);
        }

        if (m_showAxis.get()) {
          if (!m_showBackground.get()) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
          }
          renderAxis(eye);
        }
        if (m_showBackground.get() || m_showAxis.get()) {
          glBlendFunc(GL_ONE, GL_ZERO);
          glDisable(GL_BLEND);
        }

        currentOutRenderTarget.release();
      } else { // render normal geometries into tempport, then blend inport and tempport into tempport2, then render on
               // top geometries into tempport, then
        // blend temport and temport2 into outport
        auto tempSize2 = multisample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size();
        Z3DScratchResourcePool::RenderTargetLease temp1LeaseA =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize2);
        // VLOG(1) << "lease acquired";
        Z3DScratchResourcePool::RenderTargetLease temp2LeaseA =
          Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize2);
        // VLOG(1) << "lease acquired";

        // render normal geometries into tempport
        renderGeometries(normalOpaqueFilters, normalTransparentFilters, *temp1LeaseA.renderTarget, eye);

        const Z3DTexture* colorTex = nullptr;
        const Z3DTexture* depthTex = nullptr;
        mergeImageLayers(nonOpaqueLayers, eye, currentOutRenderTarget, colorTex, depthTex);

        // blend inport and tempport into tempport2
        temp2LeaseA.renderTarget->bind();
        prepareFilterForLease(*this, temp2LeaseA);
        m_alphaBlendRenderer.setColorTexture1(temp1LeaseA.renderTarget->colorTexture());
        m_alphaBlendRenderer.setDepthTexture1(temp1LeaseA.renderTarget->depthTexture());
        m_alphaBlendRenderer.setColorTexture2(colorTex);
        m_alphaBlendRenderer.setDepthTexture2(depthTex);
        m_rendererBase.render(eye, m_alphaBlendRenderer);
        temp2LeaseA.renderTarget->release();

        // render on top geometries into tempport
        renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, *temp1LeaseA.renderTarget, eye);

        // blend temport and temport2 into outport
        currentOutRenderTarget.bind();
        currentOutRenderTarget.clear();
        prepareFilterForTarget(*this, currentOutRenderTarget);

        if (m_showBackground.get()) {
          m_rendererBase.render(eye, m_backgroundRenderer);
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        m_firstOnTopBlendRenderer.setColorTexture1(temp1LeaseA.renderTarget->colorTexture());
        m_firstOnTopBlendRenderer.setDepthTexture1(temp1LeaseA.renderTarget->depthTexture());
        m_firstOnTopBlendRenderer.setColorTexture2(temp2LeaseA.renderTarget->colorTexture());
        m_firstOnTopBlendRenderer.setDepthTexture2(temp2LeaseA.renderTarget->depthTexture());
        m_rendererBase.render(eye, m_firstOnTopBlendRenderer);

        if (m_showAxis.get()) {
          if (!m_showBackground.get()) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
          }
          renderAxis(eye);
        }
        if (m_showBackground.get() || m_showAxis.get()) {
          glBlendFunc(GL_ONE, GL_ZERO);
          glDisable(GL_BLEND);
        }

        currentOutRenderTarget.release();
      }
    }
  } else { // OIT
    //    for (auto vFilter : vFilters) {
    //      if (vFilter->isReady(eye) && vFilter->hasTransparent(eye)) {
    //        normalTransparentFilters.push_back(vFilter);
    //      }
    //    }

    // Do not pre-merge image layers here; OIT path collects each non-opaque layer
    // individually in renderGeomsOIT for correct per-layer composition.
    numNormalFilters = normalOpaqueFilters.size() + normalTransparentFilters.size() + vFilters.size();
    if (numNormalFilters == 0 || numOnTopFilters == 0) {
      Z3DScratchResourcePool::RenderTargetLease temp1Lease =
        Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(
          multisample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size());
      // VLOG(1) << "lease acquired";

      if (numOnTopFilters == 0) {
        renderGeometries(normalOpaqueFilters, normalTransparentFilters, *temp1Lease.renderTarget, eye);
      } else {
        renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, *temp1Lease.renderTarget, eye);
      }

      // copy to outport
      currentOutRenderTarget.bind();
      currentOutRenderTarget.clear();
      setViewport(currentOutRenderTarget.size());

      if (m_showBackground.get()) {
        m_rendererBase.render(eye, m_backgroundRenderer);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      }
      glDepthFunc(GL_ALWAYS);
      m_textureCopyRenderer.setColorTexture(temp1Lease.renderTarget->colorTexture());
      m_textureCopyRenderer.setDepthTexture(temp1Lease.renderTarget->depthTexture());
      m_rendererBase.render(eye, m_textureCopyRenderer);
      glDepthFunc(GL_LESS);
      if (m_showAxis.get()) {
        if (!m_showBackground.get()) {
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        renderAxis(eye);
      }
      if (m_showBackground.get() || m_showAxis.get()) {
        glBlendFunc(GL_ONE, GL_ZERO);
        glDisable(GL_BLEND);
      }

      currentOutRenderTarget.release();
    } else {
      auto tempSize = multisample2x2 ? (currentOutRenderTarget.size() * 2_u32) : currentOutRenderTarget.size();
      Z3DScratchResourcePool::RenderTargetLease temp1Lease2 =
        Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize);
      // VLOG(1) << "lease acquired";
      Z3DScratchResourcePool::RenderTargetLease temp2Lease2 =
        Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(tempSize);
      // VLOG(1) << "lease acquired";

      // render normal geometries to tempport
      renderGeometries(normalOpaqueFilters, normalTransparentFilters, *temp1Lease2.renderTarget, eye);

      // render on top geometries to tempport2
      renderGeometries(onTopOpaqueFilters, onTopTransparentFilters, *temp2Lease2.renderTarget, eye);

      // blend to output
      currentOutRenderTarget.bind();
      currentOutRenderTarget.clear();
      setViewport(currentOutRenderTarget.size());

      if (m_showBackground.get()) {
        m_rendererBase.render(eye, m_backgroundRenderer);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      }
      m_firstOnTopBlendRenderer.setColorTexture1(temp2Lease2.renderTarget->colorTexture());
      m_firstOnTopBlendRenderer.setDepthTexture1(temp2Lease2.renderTarget->depthTexture());
      m_firstOnTopBlendRenderer.setColorTexture2(temp1Lease2.renderTarget->colorTexture());
      m_firstOnTopBlendRenderer.setDepthTexture2(temp1Lease2.renderTarget->depthTexture());
      m_rendererBase.render(eye, m_firstOnTopBlendRenderer);
      if (m_showAxis.get()) {
        if (!m_showBackground.get()) {
          glEnable(GL_BLEND);
          glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        renderAxis(eye);
      }
      if (m_showBackground.get() || m_showAxis.get()) {
        glBlendFunc(GL_ONE, GL_ZERO);
        glDisable(GL_BLEND);
      }

      currentOutRenderTarget.release();
    }
  }

  if (!showHandleFilters.empty()) {
    auto& finalOutRenderTarget = (eye == MonoEye)   ? *m_monoCurrentTarget
                                 : (eye == LeftEye) ? *m_leftCurrentTarget
                                                    : *m_rightCurrentTarget;
    Z3DScratchResourcePool::RenderTargetLease handleLease =
      Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(finalOutRenderTarget.size());
    // VLOG(1) << "lease acquired";

    handleLease.renderTarget->bind();
    handleLease.renderTarget->clear();
    for (auto& showHandleFilter : showHandleFilters) {
      showHandleFilter->setViewport(handleLease.renderTarget->size());
      showHandleFilter->renderHandle(eye);
    }
    handleLease.renderTarget->release();
    CHECK_GL_ERROR
    finalOutRenderTarget.bind();
    finalOutRenderTarget.clear();
    setViewport(finalOutRenderTarget.size());
    m_firstOnTopBlendRenderer.setColorTexture1(handleLease.renderTarget->colorTexture());
    m_firstOnTopBlendRenderer.setDepthTexture1(handleLease.renderTarget->depthTexture());
    m_firstOnTopBlendRenderer.setColorTexture2(currentOutRenderTarget.colorTexture());
    m_firstOnTopBlendRenderer.setDepthTexture2(currentOutRenderTarget.depthTexture());
    m_rendererBase.render(eye, m_firstOnTopBlendRenderer);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    for (auto& selectedFilter : selectedFilters) {
      selectedFilter->setViewport(finalOutRenderTarget.size());
      selectedFilter->renderSelectionBox(eye);
    }
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_BLEND);
    finalOutRenderTarget.release();
    CHECK_GL_ERROR
  } else if (!selectedFilters.empty()) {
    currentOutRenderTarget.bind();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    for (auto& selectedFilter : selectedFilters) {
      selectedFilter->setViewport(currentOutRenderTarget.size());
      selectedFilter->renderSelectionBox(eye);
    }
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_BLEND);
    currentOutRenderTarget.release();
    CHECK_GL_ERROR
  }

  // render picking objects
  ensurePickingTarget(m_monoCurrentTarget->size());
  if (filters.empty() && !showHandleFilters.empty()) {
    pickingManager().bindTarget();
    pickingManager().clearTarget();
    for (auto& showHandleFilter : showHandleFilters) {
      showHandleFilter->setViewport(pickingManager().renderTarget().size());
      showHandleFilter->renderHandlePicking(eye);
    }
    pickingManager().releaseTarget();
  } else if (showHandleFilters.empty() && !filters.empty()) {
    pickingManager().bindTarget();
    pickingManager().clearTarget();
    for (auto geomFilter : filters) {
      if (geomFilter->isReady(eye)) {
        geomFilter->setViewport(pickingManager().renderTarget().size());
        geomFilter->renderPicking(eye);
      }
    }
    pickingManager().releaseTarget();
  } else if (!filters.empty() && !showHandleFilters.empty()) {
    auto pickSize = pickingManager().renderTarget().size();
    auto leaseHandles = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(pickSize);
    // VLOG(1) << "lease acquired";
    auto leaseGeoms = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(pickSize);
    // VLOG(1) << "lease acquired";

    leaseHandles.renderTarget->bind();
    leaseHandles.renderTarget->clear();
    for (auto& showHandleFilter : showHandleFilters) {
      showHandleFilter->setViewport(leaseHandles.renderTarget->size());
      showHandleFilter->renderHandlePicking(eye);
    }
    leaseHandles.renderTarget->release();

    leaseGeoms.renderTarget->bind();
    leaseGeoms.renderTarget->clear();
    for (auto geomFilter : filters) {
      if (geomFilter->isReady(eye)) {
        geomFilter->setViewport(leaseGeoms.renderTarget->size());
        geomFilter->renderPicking(eye);
        CHECK_GL_ERROR
      }
    }
    leaseGeoms.renderTarget->release();

    pickingManager().bindTarget();
    pickingManager().clearTarget();
    setViewport(pickingManager().renderTarget().size());
    m_firstOnTopRenderer.setColorTexture1(leaseHandles.renderTarget->colorTexture());
    m_firstOnTopRenderer.setDepthTexture1(leaseHandles.renderTarget->depthTexture());
    m_firstOnTopRenderer.setColorTexture2(leaseGeoms.renderTarget->colorTexture());
    m_firstOnTopRenderer.setDepthTexture2(leaseGeoms.renderTarget->depthTexture());
    m_rendererBase.render(eye, m_firstOnTopRenderer);
    pickingManager().releaseTarget();
  }

  glDisable(GL_DEPTH_TEST);

#if defined(ATLAS_USE_OPENGLWIDGET)
  glFinish();
#else
  // glFinish();
  // VLOG(1) << "start downloading texture";
  downloadTextureToLocalColorBuffer((eye == MonoEye)   ? *m_monoCurrentTarget->colorTexture()
                                    : (eye == LeftEye) ? *m_leftCurrentTarget->colorTexture()
                                                       : *m_rightCurrentTarget->colorTexture(),
                                    (eye == MonoEye)   ? *m_monoCurrentLocalBuffer
                                    : (eye == LeftEye) ? *m_leftCurrentLocalBuffer
                                                       : *m_rightCurrentLocalBuffer);
#endif

  if (eye == MonoEye) {
    {
      const std::scoped_lock lock(m_globalParameters.targetSwitchMutex);
      if (!m_monoReadyTarget) {
        m_monoReadyTarget = &m_outRenderTarget2 != m_monoCurrentTarget ? &m_outRenderTarget2 : &m_outRenderTarget1;
        m_monoReadyLocalBuffer =
          &m_localColorBuffer2 != m_monoCurrentLocalBuffer ? &m_localColorBuffer2 : &m_localColorBuffer1;
      }
      std::swap(m_monoReadyTarget, m_monoCurrentTarget);
      std::swap(m_monoReadyLocalBuffer, m_monoCurrentLocalBuffer);
    }

    m_globalParameters.hasNewRendering = true;
    VLOG(1) << fmt::format("{} finished to {}",
                           m_progressiveRendering ? "progressive rendering" : "rendering",
                           (void*)m_monoReadyLocalBuffer);
    // Log scratch pool memory usage after the mono render completes
    const auto& pool = Z3DRenderGlobalState::instance().scratchPool();
    static uint64_t s_lastCreate = 0;
    static uint64_t s_lastChange = 0;
    const uint64_t curCreate = pool.creationCounter();
    const uint64_t curChange = pool.changeCounter();
    if (curCreate != s_lastCreate || curChange != s_lastChange) {
      VLOG(1) << pool.describeMemoryUsage(true);
      s_lastCreate = curCreate;
      s_lastChange = curChange;
    }
    Q_EMIT renderingFinished();

    if (m_monoCurrentTarget->size() != m_monoReadyTarget->size()) {
      m_monoCurrentTarget->resize(m_monoReadyTarget->size());
    }

  } else if (eye == LeftEye) {
    {
      const std::scoped_lock lock(m_globalParameters.targetSwitchMutex);
      if (!m_leftReadyTarget) {
        m_leftReadyTarget =
          &m_leftEyeOutRenderTarget2 != m_leftCurrentTarget ? &m_leftEyeOutRenderTarget2 : &m_leftEyeOutRenderTarget1;
        m_leftReadyLocalBuffer =
          &m_leftLocalColorBuffer2 != m_leftCurrentLocalBuffer ? &m_leftLocalColorBuffer2 : &m_leftLocalColorBuffer1;
      }
      std::swap(m_leftReadyTarget, m_leftCurrentTarget);
      std::swap(m_leftReadyLocalBuffer, m_leftCurrentLocalBuffer);
    }

    if (m_leftCurrentTarget->size() != m_leftReadyTarget->size()) {
      m_leftCurrentTarget->resize(m_leftReadyTarget->size());
    }
  } else {
    {
      const std::scoped_lock lock(m_globalParameters.targetSwitchMutex);
      if (!m_rightReadyTarget) {
        m_rightReadyTarget = &m_outRenderTarget2 != m_rightCurrentTarget ? &m_outRenderTarget2 : &m_outRenderTarget1;
        m_rightReadyLocalBuffer =
          &m_localColorBuffer2 != m_rightCurrentLocalBuffer ? &m_localColorBuffer2 : &m_localColorBuffer1;
      }
      std::swap(m_rightReadyTarget, m_rightCurrentTarget);
      std::swap(m_rightReadyLocalBuffer, m_rightCurrentLocalBuffer);
    }

    m_globalParameters.hasNewRendering = true;
    VLOG(1) << fmt::format("{} finished", m_progressiveRendering ? "progressive rendering" : "rendering");
    Q_EMIT renderingFinished();

    if (m_rightCurrentTarget->size() != m_rightReadyTarget->size()) {
      m_rightCurrentTarget->resize(m_rightReadyTarget->size());
    }
  }

  return 1.0;
}

void Z3DCompositor::updateSize()
{
  for (auto port : m_inputPorts) {
    port->setExpectedSize(outputSize());
  }

  invalidate(State::AllResultInvalid);
}

void Z3DCompositor::renderGeometries(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                                     const std::vector<Z3DBoundedFilter*>& transparentFilters,
                                     Z3DRenderTarget& renderTarget,
                                     Z3DEye eye)
{
  const auto transparencyMode = static_cast<TransparencyMode>(m_globalParameters.transparencyMethod.associatedData());
  if (transparencyMode == TransparencyMode::BlendNoDepthMask) {
    renderGeomsBlendNoDepthMask(opaqueFilters, transparentFilters, renderTarget, eye);
  } else if (transparencyMode == TransparencyMode::BlendDelayed) {
    renderGeomsBlendDelayed(opaqueFilters, transparentFilters, renderTarget, eye);
  } else {
    renderGeomsOIT(opaqueFilters, transparentFilters, renderTarget, eye, transparencyMode);
  }
}

void Z3DCompositor::renderTransparentFilter(Z3DBoundedFilter* filter, Z3DRenderTarget& renderTarget, Z3DEye eye)
{
  if (!filter) {
    return;
  }
  auto glowPara = filter->parameter("Glow");
  auto* glowBool = glowPara ? dynamic_cast<ZBoolParameter*>(glowPara) : nullptr;
  if (glowBool && glowBool->get()) {
    GLboolean prevDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
    if (prevDepthMask == GL_FALSE) {
      glDepthMask(GL_TRUE);
    }
    // 1) render filter geometry to pooled glow temp target 1
    auto glowLease1 = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(renderTarget.size());
    glowLease1.renderTarget->bind();
    glowLease1.renderTarget->clear();
    prepareFilterForLease(*filter, glowLease1);
    filter->renderOpaque(eye);
    glowLease1.renderTarget->release();

    // 2) sync glow params and render glow into temp2
    if (auto* mode = dynamic_cast<ZStringIntOptionParameter*>(filter->parameter("Glow Mode"))) {
      m_glowRenderer.setGlowMode(static_cast<GlowMode>(mode->associatedData()));
    }
    if (auto* radius = dynamic_cast<ZIntParameter*>(filter->parameter("Glow Blur Radius"))) {
      m_glowRenderer.setBlurRadius(radius->get());
    }
    if (auto* scale = dynamic_cast<ZFloatParameter*>(filter->parameter("Glow Blur Scale"))) {
      m_glowRenderer.setBlurScale(scale->get());
    }
    if (auto* strength = dynamic_cast<ZFloatParameter*>(filter->parameter("Glow Blur Strength"))) {
      m_glowRenderer.setBlurStrength(strength->get());
    }

    auto glowLease2 = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(renderTarget.size());
    glowLease2.renderTarget->bind();
    glowLease2.renderTarget->clear();
    prepareFilterForLease(*this, glowLease2);
    m_glowRenderer.setColorTexture(glowLease1.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_glowRenderer.setDepthTexture(glowLease1.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
    m_rendererBase.render(eye, m_glowRenderer);
    glowLease2.renderTarget->release();

    // Restore previous depth state
    if (prevDepthMask == GL_FALSE) {
      glDepthMask(GL_FALSE);
    }

    // 3) copy glow result directly into renderTarget (skip alpha blend)
    prepareFilterForTarget(*this, renderTarget);
    m_textureCopyRenderer.setColorTexture(glowLease2.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
    m_textureCopyRenderer.setDepthTexture(glowLease2.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
    m_rendererBase.render(eye, m_textureCopyRenderer);
  } else {
    // default path
    prepareFilterForTarget(*filter, renderTarget);
    filter->renderTransparent(eye);
  }
}

void Z3DCompositor::renderGeomsBlendDelayed(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                                            const std::vector<Z3DBoundedFilter*>& transparentFilters,
                                            Z3DRenderTarget& renderTarget,
                                            Z3DEye eye)
{
  renderTarget.bind();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  for (auto filter : opaqueFilters) {
    prepareFilterForTarget(*filter, renderTarget);
    filter->renderOpaque(eye);
  }

  for (auto filter : transparentFilters) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    renderTransparentFilter(filter, renderTarget, eye);
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_BLEND);
  }

  renderTarget.release();
}

void Z3DCompositor::renderGeomsBlendNoDepthMask(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                                                const std::vector<Z3DBoundedFilter*>& transparentFilters,
                                                Z3DRenderTarget& renderTarget,
                                                Z3DEye eye)
{
  renderTarget.bind();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  for (auto filter : opaqueFilters) {
    prepareFilterForTarget(*filter, renderTarget);
    filter->renderOpaque(eye);
  }

  for (auto filter : transparentFilters) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    renderTransparentFilter(filter, renderTarget, eye);
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
  }

  renderTarget.release();
}

void Z3DCompositor::renderGeomsOIT(const std::vector<Z3DBoundedFilter*>& opaqueFilters,
                                   const std::vector<Z3DBoundedFilter*>& transparentFilters,
                                   Z3DRenderTarget& renderTarget,
                                   Z3DEye eye,
                                   TransparencyMode mode)
{
  // Build per-layer lists for OIT: start with the base image pair if present
  std::vector<const Z3DTexture*> imageColorTexList;
  std::vector<const Z3DTexture*> imageDepthTexList;

  // Append each non-opaque image layer from connected image filters so they
  // participate individually in OIT, instead of a pre-merged single layer.
  auto nonOpaqueLayers = collectNonOpaqueImageLayers(eye);
  for (const auto& p : nonOpaqueLayers) {
    imageColorTexList.push_back(p.first);
    imageDepthTexList.push_back(p.second);
  }

  // Precompute per-glow color/depth layers (one pair per glow-enabled filter)
  std::vector<Z3DBoundedFilter*> glowFilters;
  // Hold leases for glow results so textures remain valid through OIT blending
  std::vector<Z3DScratchResourcePool::RenderTargetLease> glowLayerLeases;
  glowFilters.reserve(transparentFilters.size());
  for (auto f : transparentFilters) {
    if (auto p = f->parameter("Glow")) {
      if (auto* b = dynamic_cast<ZBoolParameter*>(p); b && b->get()) {
        glowFilters.push_back(f);
      }
    }
  }
  if (!glowFilters.empty()) {
    glowLayerLeases.reserve(glowFilters.size());

    for (auto gf : glowFilters) {
      // Compute glow result (color+depth) for this filter into m_glowTempRenderTarget2
      // Ensure depth test/writes for geometry prepass
      GLboolean prevDepthMask = GL_TRUE;
      glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
      GLboolean prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
      if (prevDepthTest == GL_FALSE) {
        glEnable(GL_DEPTH_TEST);
      }
      if (prevDepthMask == GL_FALSE) {
        glDepthMask(GL_TRUE);
      }

      // Geometry prepass (use pooled temp)
      auto glowGeomLease =
        Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(renderTarget.size());
      // VLOG(1) << "lease acquired";
      glowGeomLease.renderTarget->bind();
      glowGeomLease.renderTarget->clear();
      gf->setViewport(glowGeomLease.renderTarget->size());
      gf->renderOpaque(eye);
      glowGeomLease.renderTarget->release();

      // Glow blur/composition for this object directly into a pooled layer RT
      glowLayerLeases.emplace_back(
        Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(renderTarget.size()));
      // VLOG(1) << "lease acquired";
      auto* layerRT = glowLayerLeases.back().renderTarget;
      layerRT->bind();
      layerRT->clear();
      setViewport(layerRT->size());
      if (auto* glowModeParam = dynamic_cast<ZStringIntOptionParameter*>(gf->parameter("Glow Mode"))) {
        m_glowRenderer.setGlowMode(static_cast<GlowMode>(glowModeParam->associatedData()));
      }
      if (auto* radius = dynamic_cast<ZIntParameter*>(gf->parameter("Glow Blur Radius"))) {
        m_glowRenderer.setBlurRadius(radius->get());
      }
      if (auto* scale = dynamic_cast<ZFloatParameter*>(gf->parameter("Glow Blur Scale"))) {
        m_glowRenderer.setBlurScale(scale->get());
      }
      if (auto* strength = dynamic_cast<ZFloatParameter*>(gf->parameter("Glow Blur Strength"))) {
        m_glowRenderer.setBlurStrength(strength->get());
      }
      m_glowRenderer.setColorTexture(glowGeomLease.renderTarget->attachment(GL_COLOR_ATTACHMENT0));
      m_glowRenderer.setDepthTexture(glowGeomLease.renderTarget->attachment(GL_DEPTH_ATTACHMENT));
      m_rendererBase.render(eye, m_glowRenderer);
      layerRT->release();

      // Restore depth state
      if (prevDepthMask == GL_FALSE) {
        glDepthMask(GL_FALSE);
      }
      if (prevDepthTest == GL_FALSE) {
        glDisable(GL_DEPTH_TEST);
      }

      imageColorTexList.push_back(layerRT->colorTexture());
      imageDepthTexList.push_back(layerRT->depthTexture());
    }
  }

  auto dispatchTransparent = [&](Z3DRenderTarget& target, Z3DTexture* depthTexture) {
    switch (mode) {
      case TransparencyMode::DualDepthPeeling:
        renderTransparentDDP(transparentFilters, target, eye, depthTexture, imageColorTexList, imageDepthTexList);
        break;
      case TransparencyMode::WeightedAverage:
        renderTransparentWA(transparentFilters, target, eye, depthTexture, imageColorTexList, imageDepthTexList);
        break;
      case TransparencyMode::WeightedBlended:
        renderTransparentWB(transparentFilters, target, eye, depthTexture, imageColorTexList, imageDepthTexList);
        break;
      default:
        LOG(FATAL) << "renderGeomsOIT called with unsupported transparency mode: " << static_cast<int>(mode);
    }
  };

  //  std::vector<Z3DBoundedFilter*> allFilters;
  //  allFilters.insert(allFilters.end(), opaqueFilters.begin(), opaqueFilters.end());
  //  allFilters.insert(allFilters.end(), transparentFilters.begin(), transparentFilters.end());
  //  if (mode == "Dual Depth Peeling") {
  //    renderTransparentDDP(allFilters, port, eye);
  //  } else if (mode == "Weighted Average") {
  //    renderTransparentWA(allFilters, port, eye);
  //  }
  //  return;
  if (transparentFilters.empty() && imageColorTexList.empty()) {
    renderOpaqueFilters(opaqueFilters, renderTarget, eye);
  }
  //  else {
  //    if (mode == "Dual Depth Peeling") {
  //      renderTransparentDDP(renderers, port, eye);
  //    }
  //  }
  else if (opaqueFilters.empty()) {
    dispatchTransparent(renderTarget, nullptr);
  } else {
    auto leaseOpaque = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(renderTarget.size());
    // VLOG(1) << "lease acquired";
    renderOpaqueFilters(opaqueFilters, *leaseOpaque.renderTarget, eye);

    auto leaseTrans = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(renderTarget.size());
    // VLOG(1) << "lease acquired";
    dispatchTransparent(*leaseTrans.renderTarget, leaseOpaque.renderTarget->depthTexture());

    // blend temport3 and temport4 into outport
    renderTarget.bind();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    prepareFilterForTarget(*this, renderTarget);
    m_alphaBlendRenderer.setColorTexture1(leaseOpaque.renderTarget->colorTexture());
    m_alphaBlendRenderer.setDepthTexture1(leaseOpaque.renderTarget->depthTexture());
    m_alphaBlendRenderer.setColorTexture2(leaseTrans.renderTarget->colorTexture());
    m_alphaBlendRenderer.setDepthTexture2(leaseTrans.renderTarget->depthTexture());
    m_rendererBase.render(eye, m_alphaBlendRenderer);

    renderTarget.release();
  }
}

void Z3DCompositor::renderOpaqueFilters(const std::vector<Z3DBoundedFilter*>& filters,
                                        Z3DRenderTarget& renderTarget,
                                        Z3DEye eye)
{
  renderTarget.bind();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  for (auto filter : filters) {
    prepareFilterForTarget(*filter, renderTarget);
    filter->renderOpaque(eye);
  }
  renderTarget.release();
}

// Vector-list overload: feeds multiple image pairs through DDP
void Z3DCompositor::renderTransparentDDP(const std::vector<Z3DBoundedFilter*>& filters,
                                         Z3DRenderTarget& renderTarget,
                                         Z3DEye eye,
                                         Z3DTexture* depthTexture,
                                         const std::vector<const Z3DTexture*>& imageColorTexList,
                                         const std::vector<const Z3DTexture*>& imageDepthTexList)
{
  Z3DRenderTarget& ddpRT = ensureDDPRenderTarget(renderTarget.size());
  if (depthTexture) {
    ddpRT.attachTextureToFBO(depthTexture, GL_DEPTH_ATTACHMENT, false);
    ddpRT.isFBOComplete();
  }

  const Z3DTexture* g_dualDepthTexId[2];
  g_dualDepthTexId[0] = ddpRT.attachment(GL_COLOR_ATTACHMENT0);
  g_dualDepthTexId[1] = ddpRT.attachment(GL_COLOR_ATTACHMENT3);
  const Z3DTexture* g_dualFrontBlenderTexId[2];
  g_dualFrontBlenderTexId[0] = ddpRT.attachment(GL_COLOR_ATTACHMENT1);
  g_dualFrontBlenderTexId[1] = ddpRT.attachment(GL_COLOR_ATTACHMENT4);
  const Z3DTexture* g_dualBackTempTexId[2];
  g_dualBackTempTexId[0] = ddpRT.attachment(GL_COLOR_ATTACHMENT2);
  g_dualBackTempTexId[1] = ddpRT.attachment(GL_COLOR_ATTACHMENT5);
  const Z3DTexture* g_dualBackBlenderTexId = ddpRT.attachment(GL_COLOR_ATTACHMENT6);
  const Z3DTexture* g_depthTex = ddpRT.attachment(GL_COLOR_ATTACHMENT7);
  const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0,
                                  GL_COLOR_ATTACHMENT1,
                                  GL_COLOR_ATTACHMENT2,
                                  GL_COLOR_ATTACHMENT3,
                                  GL_COLOR_ATTACHMENT4,
                                  GL_COLOR_ATTACHMENT5,
                                  GL_COLOR_ATTACHMENT6};

  const GLenum g_db[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT7};

  bool g_useOQ = true;
  size_t g_numPasses = 100;

#define MAX_DEPTH 1.0

  if (depthTexture) {
    glDepthMask(GL_FALSE);
  } else {
    glDisable(GL_DEPTH_TEST);
  }
  glEnable(GL_BLEND);

  // ---------------------------------------------------------------------
  // 1. Initialize Min-Max Depth Buffer
  // ---------------------------------------------------------------------

  ddpRT.bind();

  // Render targets 1 and 2 store the front and back colors
  // Clear to 0.0 and use MAX blending to filter written color
  // At most one front color and one back color can be written every pass
  glDrawBuffers(2, &g_drawBuffers[1]);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);

  // Render target 0 stores (-minDepth, maxDepth, alphaMultiplier)
  glDrawBuffers(2, g_db);
  glClearColor(-MAX_DEPTH, -MAX_DEPTH, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  glBlendEquation(GL_MAX);

  for (auto filter : filters) {
    prepareFilterForTarget(*filter, ddpRT);
    filter->setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingInit);
    filter->renderTransparent(eye);
  }
  if (!imageColorTexList.empty()) {
    prepareFilterForTarget(*this, ddpRT);
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingInit);
    size_t n = std::min(imageColorTexList.size(), imageDepthTexList.size());
    for (size_t i = 0; i < n; ++i) {
      m_textureCopyRenderer.setColorTexture(imageColorTexList[i]);
      m_textureCopyRenderer.setDepthTexture(imageDepthTexList[i]);
      m_rendererBase.render(eye, m_textureCopyRenderer);
    }
  }

  // ---------------------------------------------------------------------
  // 2. Dual Depth Peeling + Blending
  // ---------------------------------------------------------------------

  // Since we cannot blend the back colors in the geometry passes,
  // we use another render target to do the alpha blending
  glDrawBuffer(g_drawBuffers[6]);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);

  size_t currId = 0;

  for (size_t pass = 1; g_useOQ && pass < g_numPasses; pass++) {
    currId = pass % 2;
    auto prevId = 1 - currId;
    auto bufId = currId * 3;

    glDrawBuffers(2, &g_drawBuffers[bufId + 1]);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    glDrawBuffer(g_drawBuffers[bufId + 0]);
    glClearColor(-MAX_DEPTH, -MAX_DEPTH, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    // Render target 0: RG32F MAX blending
    // Render target 1: RGBA MAX blending
    // Render target 2: RGBA MAX blending
    glDrawBuffers(3, &g_drawBuffers[bufId + 0]);
    glBlendEquation(GL_MAX);

    for (auto filter : filters) {
      prepareFilterForTarget(*filter, ddpRT);
      filter->setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
      filter->setShaderHookParaDDPDepthBlenderTexture(g_dualDepthTexId[prevId]);
      filter->setShaderHookParaDDPFrontBlenderTexture(g_dualFrontBlenderTexId[prevId]);
      filter->renderTransparent(eye);
    }
    if (!imageColorTexList.empty()) {
      prepareFilterForTarget(*this, ddpRT);
      m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::DualDepthPeelingPeel);
      size_t n = std::min(imageColorTexList.size(), imageDepthTexList.size());
      for (size_t i = 0; i < n; ++i) {
        m_textureCopyRenderer.setColorTexture(imageColorTexList[i]);
        m_textureCopyRenderer.setDepthTexture(imageDepthTexList[i]);
        m_rendererBase.render(eye, m_textureCopyRenderer);
      }
    }

    // Full screen pass to alpha-blend the back color
    glDrawBuffer(g_drawBuffers[6]);

    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    GLuint queryId;
    if (g_useOQ) {
      glGenQueries(1, &queryId);
      glBeginQuery(GL_SAMPLES_PASSED, queryId);
    }

    m_ddpBlendShader.bind();
    m_ddpBlendShader.bindTexture("TempTex", g_dualBackTempTexId[currId]);
    const glm::uvec2 ddpBlendSize = ddpRT.size();
    const glm::vec2 ddpBlendScreenDimRcp(ddpBlendSize.x > 0u ? 1.f / static_cast<float>(ddpBlendSize.x) : 0.f,
                                         ddpBlendSize.y > 0u ? 1.f / static_cast<float>(ddpBlendSize.y) : 0.f);
    m_ddpBlendShader.setScreenDimRCPUniform(ddpBlendScreenDimRcp);

    Z3DPrimitiveRenderer::renderScreenQuad(m_screenQuadVAO, m_ddpBlendShader);
    m_ddpBlendShader.release();

    if (g_useOQ) {
      glEndQuery(GL_SAMPLES_PASSED);
      GLuint sample_count;
      glGetQueryObjectuiv(queryId, GL_QUERY_RESULT, &sample_count);
      glDeleteQueries(1, &queryId);
      if (sample_count == 0) {
        break;
      }
    }
  }

  if (depthTexture) {
    ddpRT.detach(GL_DEPTH_ATTACHMENT);
    ddpRT.isFBOComplete();
  }
  ddpRT.release();

  if (depthTexture) {
    glDepthMask(GL_TRUE);
  }
  glClearColor(0, 0, 0, 0);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_ONE, GL_ZERO);
  glDisable(GL_BLEND);

  for (auto filter : filters) {
    filter->setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  }
  if (!imageColorTexList.empty()) {
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  }

  // ---------------------------------------------------------------------
  // 3. Final Pass
  // ---------------------------------------------------------------------

  renderTarget.bind();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  m_ddpFinalShader.bind();
  m_ddpFinalShader.bindTexture("DepthTex", g_depthTex);
  m_ddpFinalShader.bindTexture("FrontBlenderTex", g_dualFrontBlenderTexId[currId]);
  m_ddpFinalShader.bindTexture("BackBlenderTex", g_dualBackBlenderTexId);

  const glm::uvec2 ddpSize = ddpRT.size();
  const glm::vec2 ddpScreenDimRcp(ddpSize.x > 0u ? 1.f / static_cast<float>(ddpSize.x) : 0.f,
                                  ddpSize.y > 0u ? 1.f / static_cast<float>(ddpSize.y) : 0.f);
  m_ddpFinalShader.setScreenDimRCPUniform(ddpScreenDimRcp);

  Z3DPrimitiveRenderer::renderScreenQuad(m_screenQuadVAO, m_ddpFinalShader);
  m_ddpFinalShader.release();
  renderTarget.release();

  glEnable(GL_DEPTH_TEST);
}

// Removed single-image overload: use list-based API

void Z3DCompositor::renderTransparentWA(const std::vector<Z3DBoundedFilter*>& filters,
                                        Z3DRenderTarget& renderTarget,
                                        Z3DEye eye,
                                        Z3DTexture* depthTexture,
                                        const std::vector<const Z3DTexture*>& imageColorTexList,
                                        const std::vector<const Z3DTexture*>& imageDepthTexList)
{
  Z3DRenderTarget& waRT = ensureWARenderTarget(renderTarget.size());
  if (depthTexture) {
    waRT.attachTextureToFBO(depthTexture, GL_DEPTH_ATTACHMENT, false);
    waRT.isFBOComplete();
  }

  const Z3DTexture* g_accumulationTexId[2];
  g_accumulationTexId[0] = waRT.attachment(GL_COLOR_ATTACHMENT0);
  g_accumulationTexId[1] = waRT.attachment(GL_COLOR_ATTACHMENT1);
  const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};

  if (depthTexture) {
    glDepthMask(GL_FALSE);
  } else {
    glDisable(GL_DEPTH_TEST);
  }

  waRT.bind();
  glDrawBuffers(2, g_drawBuffers);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);

  glBlendFunc(GL_ONE, GL_ONE);
  glEnable(GL_BLEND);

  for (auto filter : filters) {
    filter->setViewport(waRT.size());
    filter->setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedAverageInit);
    filter->renderTransparent(eye);
  }
  if (!imageColorTexList.empty()) {
    setViewport(waRT.size());
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedAverageInit);
    size_t n = std::min(imageColorTexList.size(), imageDepthTexList.size());
    for (size_t i = 0; i < n; ++i) {
      m_textureCopyRenderer.setColorTexture(imageColorTexList[i]);
      m_textureCopyRenderer.setDepthTexture(imageDepthTexList[i]);
      m_rendererBase.render(eye, m_textureCopyRenderer);
    }
  }

  if (depthTexture) {
    waRT.detach(GL_DEPTH_ATTACHMENT);
    waRT.isFBOComplete();
  }
  waRT.release();

  if (depthTexture) {
    glDepthMask(GL_TRUE);
  }
  glBlendFunc(GL_ONE, GL_ZERO);
  glDisable(GL_BLEND);

  for (auto filter : filters) {
    filter->setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  }
  if (!imageColorTexList.empty()) {
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  }

  renderTarget.bind();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  m_waFinalShader.bind();
  m_waFinalShader.bindTexture("ColorTex0", g_accumulationTexId[0]);
  m_waFinalShader.bindTexture("ColorTex1", g_accumulationTexId[1]);

  const glm::uvec2 waSize = waRT.size();
  const glm::vec2 waScreenDimRcp(waSize.x > 0u ? 1.f / static_cast<float>(waSize.x) : 0.f,
                                 waSize.y > 0u ? 1.f / static_cast<float>(waSize.y) : 0.f);
  m_waFinalShader.setScreenDimRCPUniform(waScreenDimRcp);

  Z3DPrimitiveRenderer::renderScreenQuad(m_screenQuadVAO, m_waFinalShader);
  m_waFinalShader.release();
  renderTarget.release();

  glEnable(GL_DEPTH_TEST);
}

void Z3DCompositor::renderTransparentWB(const std::vector<Z3DBoundedFilter*>& filters,
                                        Z3DRenderTarget& renderTarget,
                                        Z3DEye eye,
                                        Z3DTexture* depthTexture,
                                        const std::vector<const Z3DTexture*>& imageColorTexList,
                                        const std::vector<const Z3DTexture*>& imageDepthTexList)
{
  Z3DRenderTarget& wbRT = ensureWBRenderTarget(renderTarget.size());
  if (depthTexture) {
    wbRT.attachTextureToFBO(depthTexture, GL_DEPTH_ATTACHMENT, false);
    wbRT.isFBOComplete();
  }

  const Z3DTexture* g_accumulationTexId[2];
  g_accumulationTexId[0] = wbRT.attachment(GL_COLOR_ATTACHMENT0);
  g_accumulationTexId[1] = wbRT.attachment(GL_COLOR_ATTACHMENT1);
  const GLenum g_drawBuffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};

  if (depthTexture) {
    glDepthMask(GL_FALSE);
  } else {
    glDisable(GL_DEPTH_TEST);
  }

  wbRT.bind();
  glDrawBuffers(2, g_drawBuffers);

  float clearColorZero[4] = {0.f, 0.f, 0.f, 0.f};
  float clearColorOne[4] = {1.f, 1.f, 1.f, 1.f};
  glClearBufferfv(GL_COLOR, 0, clearColorZero);
  glClearBufferfv(GL_COLOR, 1, clearColorOne);

  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunci(0, GL_ONE, GL_ONE);
  glBlendFunci(1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR);

  for (auto filter : filters) {
    filter->setViewport(wbRT.size());
    filter->setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedBlendedInit);
    filter->renderTransparent(eye);
  }
  if (!imageColorTexList.empty()) {
    setViewport(wbRT.size());
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::WeightedBlendedInit);
    size_t n = std::min(imageColorTexList.size(), imageDepthTexList.size());
    for (size_t i = 0; i < n; ++i) {
      m_textureCopyRenderer.setColorTexture(imageColorTexList[i]);
      m_textureCopyRenderer.setDepthTexture(imageDepthTexList[i]);
      m_rendererBase.render(eye, m_textureCopyRenderer);
    }
  }

  if (depthTexture) {
    wbRT.detach(GL_DEPTH_ATTACHMENT);
    wbRT.isFBOComplete();
  }
  wbRT.release();

  glBlendFunc(GL_ONE, GL_ZERO);
  glDisable(GL_BLEND);

  if (depthTexture) {
    glDepthMask(GL_TRUE);
  }

  for (auto filter : filters) {
    filter->setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  }
  if (!imageColorTexList.empty()) {
    m_rendererBase.setShaderHookType(Z3DRendererBase::ShaderHookType::Normal);
  }

  renderTarget.bind();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  m_wbFinalShader.bind();
  m_wbFinalShader.bindTexture("ColorTex0", g_accumulationTexId[0]);
  m_wbFinalShader.bindTexture("ColorTex1", g_accumulationTexId[1]);

  const glm::uvec2 wbSize = wbRT.size();
  const glm::vec2 wbScreenDimRcp(wbSize.x > 0u ? 1.f / static_cast<float>(wbSize.x) : 0.f,
                                 wbSize.y > 0u ? 1.f / static_cast<float>(wbSize.y) : 0.f);
  m_wbFinalShader.setScreenDimRCPUniform(wbScreenDimRcp);

  Z3DPrimitiveRenderer::renderScreenQuad(m_screenQuadVAO, m_wbFinalShader);
  m_wbFinalShader.release();
  renderTarget.release();

  glEnable(GL_DEPTH_TEST);
}

void Z3DCompositor::ensurePickingTarget(const glm::uvec2& size)
{
  if (size.x == 0u || size.y == 0u) {
    return;
  }
  if (!m_pickingTargetLease.renderTarget || m_pickingTargetLease.renderTarget->size() != size) {
    m_pickingTargetLease.release();
    m_pickingTargetLease =
      Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(size,
                                                                               ScratchFormat::RGBA8,
                                                                               ScratchFormat::Depth24);
  }

  CHECK(m_pickingTargetLease.renderTarget != nullptr);
  m_globalParameters.setPickingTarget(*m_pickingTargetLease.renderTarget);
}

Z3DRenderTarget& Z3DCompositor::ensureDDPRenderTarget(const glm::uvec2& size)
{
  CHECK_GT(size.x, 0u);
  CHECK_GT(size.y, 0u);
  if (!m_ddpRTLease.renderTarget || m_ddpRTLease.renderTarget->size() != size) {
    m_ddpRTLease.release();
    m_ddpRTLease = Z3DRenderGlobalState::instance().scratchPool().acquireDualDepthPeelRenderTarget(size);
  }
  CHECK(m_ddpRTLease.renderTarget != nullptr);
  return *m_ddpRTLease.renderTarget;
}

Z3DRenderTarget& Z3DCompositor::ensureWARenderTarget(const glm::uvec2& size)
{
  CHECK_GT(size.x, 0u);
  CHECK_GT(size.y, 0u);
  if (!m_waRTLease.renderTarget || m_waRTLease.renderTarget->size() != size) {
    m_waRTLease.release();
    m_waRTLease = Z3DRenderGlobalState::instance().scratchPool().acquireWeightedAverageRenderTarget(size);
  }
  CHECK(m_waRTLease.renderTarget != nullptr);
  return *m_waRTLease.renderTarget;
}

Z3DRenderTarget& Z3DCompositor::ensureWBRenderTarget(const glm::uvec2& size)
{
  CHECK_GT(size.x, 0u);
  CHECK_GT(size.y, 0u);
  if (!m_wbRTLease.renderTarget || m_wbRTLease.renderTarget->size() != size) {
    m_wbRTLease.release();
    m_wbRTLease = Z3DRenderGlobalState::instance().scratchPool().acquireWeightedBlendedRenderTarget(size);
  }
  CHECK(m_wbRTLease.renderTarget != nullptr);
  return *m_wbRTLease.renderTarget;
}

void Z3DCompositor::renderAxis(Z3DEye eye)
{
  prepareAxisData(eye);
  {
    const glm::mat4 axisTransform = glm::mat4(m_globalParameters.camera.get().rotateMatrix(eye));
    const glm::uvec4& viewport = currentViewport();

    if (m_region[0] <= 0.f && m_region[2] <= 0.f) {
      double startX = viewport.x + viewport.z / m_region[1] * m_region[0];
      double startY = viewport.y + viewport.w / m_region[3] * m_region[2];

      GLsizei size = std::min(viewport.z, viewport.w) * m_axisRegionRatio.get();
      glViewport(viewport.x - std::floor(startX), viewport.y - std::floor(startY), size, size);
      glScissor(viewport.x - std::floor(startX), viewport.y - std::floor(startY), size, size);
      glEnable(GL_SCISSOR_TEST);
      glClear(GL_DEPTH_BUFFER_BIT);

      if (m_axisMode.get() == "Arrow") {
        renderWithStateAndCameraAndCoordTransform(eye, m_axisCamera, axisTransform, m_arrowRenderer, m_fontRenderer);
      } else {
        renderWithStateAndCameraAndCoordTransform(eye, m_axisCamera, axisTransform, m_lineRenderer, m_fontRenderer);
      }

      glViewport(viewport.x, viewport.y, viewport.z, viewport.w);
      glScissor(viewport.x, viewport.y, viewport.z, viewport.w);
      glDisable(GL_SCISSOR_TEST);
    }
  }
}

void Z3DCompositor::prepareAxisData(Z3DEye eye)
{
  m_textPositions.clear();
  glm::mat3 rotMatrix = m_globalParameters.camera.get().rotateMatrix(eye);
  m_XEnd = rotMatrix * glm::vec3(256.f, 0.f, 0.f);
  m_YEnd = rotMatrix * glm::vec3(0.f, 256.f, 0.f);
  m_ZEnd = rotMatrix * glm::vec3(0.f, 0.f, 256.f);

  m_textPositions.push_back(m_XEnd * glm::vec3(0.93));
  m_textPositions.push_back(m_YEnd * glm::vec3(0.93));
  m_textPositions.push_back(m_ZEnd * glm::vec3(0.93));
  QStringList texts;
  texts.push_back("X");
  texts.push_back("Y");
  texts.push_back("Z");

  m_fontRenderer.setData(&m_textPositions, texts);
}

void Z3DCompositor::setupAxisCamera()
{
  Z3DCamera camera;
  glm::vec3 center(0.f);
  camera.setFieldOfView(glm::radians(10.f));

  float radius = 300.f;

  float distance = radius / std::sin(camera.fieldOfView() * 0.5f);
  glm::vec3 vn(0, 0, 1); // plane normal
  glm::vec3 position = center + vn * distance;
  camera.setCamera(position, center, glm::vec3(0.0, 1.0, 0.0));
  camera.setNearDist(distance - radius - 1);
  camera.setFarDist(distance + radius);

  m_axisCamera = camera;

  m_tailPosAndTailRadius.clear();
  m_headPosAndHeadRadius.clear();
  m_lineColors.clear();
  m_lines.clear();
  m_textColors.clear();
  m_textPositions.clear();
  m_XEnd = glm::vec3(256.f, 0.f, 0.f);
  m_YEnd = glm::vec3(0.f, 256.f, 0.f);
  m_ZEnd = glm::vec3(0.f, 0.f, 256.f);
  glm::vec3 origin(0.f);
  m_lines.push_back(origin);
  m_lineColors.push_back(m_XAxisColor.get());
  m_lines.push_back(m_XEnd * glm::vec3(0.88));
  m_lineColors.push_back(m_XAxisColor.get());
  m_lines.push_back(origin);
  m_lineColors.push_back(m_YAxisColor.get());
  m_lines.push_back(m_YEnd * glm::vec3(0.88));
  m_lineColors.push_back(m_YAxisColor.get());
  m_lines.push_back(origin);
  m_lineColors.push_back(m_ZAxisColor.get());
  m_lines.push_back(m_ZEnd * glm::vec3(0.88));
  m_lineColors.push_back(m_ZAxisColor.get());

  m_textPositions.push_back(m_XEnd * glm::vec3(0.93));
  m_textPositions.push_back(m_YEnd * glm::vec3(0.93));
  m_textPositions.push_back(m_ZEnd * glm::vec3(0.93));
  m_textColors.push_back(m_XAxisColor.get());
  m_textColors.push_back(m_YAxisColor.get());
  m_textColors.push_back(m_ZAxisColor.get());

  float tailRadius = 5.f;
  float headRadius = 10.f;

  m_tailPosAndTailRadius.emplace_back(origin, tailRadius);
  m_headPosAndHeadRadius.emplace_back(m_XEnd * glm::vec3(0.88), headRadius);

  m_tailPosAndTailRadius.emplace_back(origin, tailRadius);
  m_headPosAndHeadRadius.emplace_back(m_YEnd * glm::vec3(0.88), headRadius);

  m_tailPosAndTailRadius.emplace_back(origin, tailRadius);
  m_headPosAndHeadRadius.emplace_back(m_ZEnd * glm::vec3(0.88), headRadius);

  m_lineRenderer.setData(std::move(m_lines));
  m_lineRenderer.setDataColors(std::move(m_lineColors));
  m_arrowRenderer.setArrowData(&m_tailPosAndTailRadius, &m_headPosAndHeadRadius, .1f);
  m_arrowRenderer.setArrowColors(&m_textColors);
  m_fontRenderer.setDataColors(&m_textColors);
}

// Collect non-opaque image layers (color/depth) from connected image filters
std::vector<std::pair<const Z3DTexture*, const Z3DTexture*>>
Z3DCompositor::collectNonOpaqueImageLayers(Z3DEye eye) const
{
  std::vector<std::pair<const Z3DTexture*, const Z3DTexture*>> layers;
  auto vFilters = m_vPPort.connectedFilters();
  for (auto* vf : vFilters) {
    if (!vf || !vf->isReady(eye) || !vf->hasTransparent(eye)) {
      continue;
    }
    const auto& target = vf->transparentTarget(eye);
    layers.emplace_back(target.attachment(GL_COLOR_ATTACHMENT0), target.attachment(GL_DEPTH_ATTACHMENT));
  }
  return layers;
}

// Merge a list of image layers using the same shader/path as renderImages
bool Z3DCompositor::mergeImageLayers(const std::vector<std::pair<const Z3DTexture*, const Z3DTexture*>>& layers,
                                     Z3DEye eye,
                                     Z3DRenderTarget& renderTarget,
                                     const Z3DTexture*& colorTex,
                                     const Z3DTexture*& depthTex)
{
  colorTex = nullptr;
  depthTex = nullptr;
  if (layers.empty()) {
    return false;
  }
  if (layers.size() == 1) {
    colorTex = layers[0].first;
    depthTex = layers[0].second;
    return true;
  }

  auto imgLease1 = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(renderTarget.size());
  auto imgLease2 = Z3DRenderGlobalState::instance().scratchPool().acquireTempRenderTarget2D(renderTarget.size());

  // Blend first two layers into imgLease1
  imgLease1.renderTarget->bind();
  imgLease1.renderTarget->clear();
  setViewport(imgLease1.renderTarget->size());
  m_MIPImageAlphaBlendRenderer.setColorTexture1(layers[0].first);
  m_MIPImageAlphaBlendRenderer.setDepthTexture1(layers[0].second);
  m_MIPImageAlphaBlendRenderer.setColorTexture2(layers[1].first);
  m_MIPImageAlphaBlendRenderer.setDepthTexture2(layers[1].second);
  m_rendererBase.render(eye, m_MIPImageAlphaBlendRenderer);
  imgLease1.renderTarget->release();
  auto* resRT = imgLease1.renderTarget;
  auto* nextResRT = imgLease2.renderTarget;
  for (size_t i = 2; i < layers.size(); ++i) {
    nextResRT->bind();
    nextResRT->clear();
    setViewport(nextResRT->size());
    m_MIPImageAlphaBlendRenderer.setColorTexture1(resRT->colorTexture());
    m_MIPImageAlphaBlendRenderer.setDepthTexture1(resRT->depthTexture());
    m_MIPImageAlphaBlendRenderer.setColorTexture2(layers[i].first);
    m_MIPImageAlphaBlendRenderer.setDepthTexture2(layers[i].second);
    m_rendererBase.render(eye, m_MIPImageAlphaBlendRenderer);
    nextResRT->release();
    std::swap(resRT, nextResRT);
  }

  colorTex = resRT->colorTexture();
  depthTex = resRT->depthTexture();
  return true;
}

void Z3DCompositor::downloadTextureToLocalColorBuffer(const Z3DTexture& tex, Z3DLocalColorBuffer& localColorBuffer)
{
  // Set up format and type
  GLenum dataFormat = GL_BGRA;
  GLenum dataType = GL_UNSIGNED_INT_8_8_8_8_REV;

  // Allocate buffer for texture data
  auto desiredSize = Z3DTexture::bypePerPixel(dataFormat, dataType) * tex.numPixels();
  if (localColorBuffer.data.size() < desiredSize) {
    localColorBuffer.data.resize(desiredSize);
  }

#if 0
  if (true) {
    m_PBO.bind(GL_PIXEL_PACK_BUFFER);
    glBufferData(GL_PIXEL_PACK_BUFFER, desiredSize, nullptr, GL_STREAM_READ);
    tex.downloadTextureToBuffer(dataFormat, dataType, nullptr);
    // Map PBO to client memory
    auto ptr = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (ptr) {
      std::memcpy(localColorBuffer.data.data(), ptr, desiredSize);
      glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    } else {
      LOG(WARNING) << "glMapBuffer failed on PBO";
      tex.downloadTextureToBuffer(dataFormat, dataType, localColorBuffer.data.data());
    }
    m_PBO.release(GL_PIXEL_PACK_BUFFER);
  } else {
    // Download texture data to buffer
    tex.downloadTextureToBuffer(dataFormat, dataType, localColorBuffer.data.data());
  }
#else
  tex.downloadTextureToBuffer(dataFormat, dataType, localColorBuffer.data.data());
#endif
  localColorBuffer.width = tex.width();
  localColorBuffer.height = tex.height();
}

} // namespace nim
