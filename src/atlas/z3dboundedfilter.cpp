#include "z3dboundedfilter.h"
#include "z3dgl.h"
#include "z3drenderglobalstate.h"
#include "zexception.h"
#include "zlog.h"
#include <cmath>
#include <algorithm>
#include <optional>
#include <folly/ScopeGuard.h>
#include <Mathematics/DistLineRay.h>
#include <boost/math/constants/constants.hpp>
#include <utility>
#include <vector>

namespace nim {

Z3DBoundedFilter::RendererParameters::RendererParameters()
  : coordTransform("Coord Transform", glm::mat4(1.f))
  , sizeScale("Size Scale", 1.f, .001f, std::numeric_limits<float>::max())
  , opacity("Opacity", 1.0f, .0f, 1.f)
  , materialAmbient("Material Ambient", glm::vec4(0.1f, .1f, .1f, 1.f))
  , materialSpecular("Material Specular", glm::vec4(1.f, 1.f, 1.f, 1.f))
  , materialShininess("Material Shininess", 100.f, 1.f, 200.f)
{
  coordTransform.setDescription(QStringLiteral(
    "Object transform bundle with fields: 'Scale Vec3', 'Rotation Vec4' (axis-angle, degrees), "
    "'Rotation Center Vec3', and 'Translation Vec3'.\n"
    "\n"
    "Matrix composition (GLM column-vector convention; rightmost term applies first):\n"
    "  M = T(Translation + RotationCenter*Scale) * R(Rotation) * T(-RotationCenter*Scale) * S(Scale)\n"
    "Meaning: scale about origin → rotate about (RotationCenter*Scale) → translate.\n"
    "RotationCenter*Scale is component-wise (cx*sx, cy*sy, cz*sz).\n"
    "RPC/scene_apply: you may provide a partial object (e.g., only 'Translation Vec3'); unspecified fields stay unchanged.\n"
    "Note: Rotation Center affects rotation only; scaling is always about the object origin (0,0,0)."));
  sizeScale.setSingleStep(0.001);
  sizeScale.setDecimal(3);
  sizeScale.setStyle("SPINBOX");
  sizeScale.setDescription(QStringLiteral("Uniform scale factor applied to the object (multiplicative)."));

  opacity.setDescription(QStringLiteral("Overall surface opacity (1.0=opaque, 0.0=fully transparent)."));
  materialAmbient.setStyle("COLOR");
  materialSpecular.setStyle("COLOR");
}

Z3DBoundedFilter::Z3DBoundedFilter(Z3DGlobalParameters& globalPara, QObject* parent)
  : Z3DFilter(parent)
  , m_globalParameters(globalPara)
  , m_rendererBase(m_rendererParameterState,
                   m_rendererFrameState,
                   Z3DRenderGlobalState::instance().rendererState().viewState,
                   Z3DRenderGlobalState::instance().rendererState().sceneState,
                   static_cast<RenderBackend>(globalPara.renderBackend.associatedData()))
  , m_baseBoundBoxRenderer(m_rendererBase)
  , m_selectionBoundBoxRenderer(m_rendererBase)
  , m_selectionCornerRenderer(m_rendererBase)
  , m_handleCenterRenderer(m_rendererBase)
  , m_handleArrowRenderer(m_rendererBase)
  , m_visible("Visible", true)
  , m_xCut("X Cut", glm::vec2(0, 0), 0, 0)
  , m_yCut("Y Cut", glm::vec2(0, 0), 0, 0)
  , m_zCut("Z Cut", glm::vec2(0, 0), 0, 0)
  , m_boundBoxMode("Bound Box")
  , m_boundBoxLineWidth("Bound Box Line Width", 1, 1, 100)
  , m_boundBoxLineColor("Bound Box Line Color", glm::vec4(0.f, 1.f, 1.f, 1.f))
  //, m_boundBoxLineColor("Bound Box Line Color")
  , m_selectionLineWidth("Selection Line Width", 1, 1, 100)
  , m_selectionLineColor("Selection Line Color", glm::vec4(1.f, 1.f, 0.f, 1.f))
  , m_manipulatorSize("Manipulator Size", 150, 50, 1e5)
  , m_handleEvent("handle", false)
  , m_handleCenterAndRadius(1)
  , m_handleArrowTailPosAndTailRadius(3)
  , m_handleArrowheadPosAndHeadRadius(3)
  , m_canUpdateClipPlane(true)
  , m_isSelected(false)
  , m_selectedHandle(0)
  , m_transformEnabled(true)
  , m_handleValid(false)
{
  m_boundBoxMode.addOptions("No Bound Box", "Bound Box", "Axis Aligned Bound Box");
  m_boundBoxMode.select("No Bound Box");

  m_manipulatorSize.setStyle("SPINBOX");
  m_manipulatorSize.setDecimal(0);
  m_manipulatorSize.setSingleStep(10);
  connect(&m_manipulatorSize, &ZFloatParameter::valueChanged, this, &Z3DBoundedFilter::invalidateHandle);
  m_manipulatorSize.setDescription(QStringLiteral("Size of transform/selection handles in pixels."));

  connect(&m_globalParameters.camera, &Z3DCameraParameter::valueChanged, this, &Z3DBoundedFilter::invalidateHandle);

  connect(&m_visible, &ZBoolParameter::boolChanged, this, &Z3DBoundedFilter::objVisibleChanged);
  m_visible.setDescription(QStringLiteral("Toggle object visibility in the 3D view."));
  m_xCut.setSingleStep(1);
  m_yCut.setSingleStep(1);
  m_zCut.setSingleStep(1);
  m_xCut.setDescription(
    QStringLiteral("Local clipping interval along the X axis in the object's pre-transform space."));
  m_yCut.setDescription(
    QStringLiteral("Local clipping interval along the Y axis in the object's pre-transform space."));
  m_zCut.setDescription(
    QStringLiteral("Local clipping interval along the Z axis in the object's pre-transform space."));
  // XYZ cut parameters can be animated and often change in quick succession
  // (e.g. when applying a timeline time). Recomputing clip planes (and thus
  // recompiling GLSL shaders when the clip-plane count changes) on every
  // intermediate step is extremely expensive.
  //
  // Instead, mark clip planes dirty and rebuild them once just before the next
  // render in syncRendererState().
  connect(&m_xCut, &ZFloatSpanParameter::valueChanged, this, &Z3DBoundedFilter::markClipPlanesDirty);
  connect(&m_yCut, &ZFloatSpanParameter::valueChanged, this, &Z3DBoundedFilter::markClipPlanesDirty);
  connect(&m_zCut, &ZFloatSpanParameter::valueChanged, this, &Z3DBoundedFilter::markClipPlanesDirty);
  connect(&m_globalParameters.globalXCut,
          &ZFloatSpanParameter::valueChanged,
          this,
          &Z3DBoundedFilter::markClipPlanesDirty);
  connect(&m_globalParameters.globalYCut,
          &ZFloatSpanParameter::valueChanged,
          this,
          &Z3DBoundedFilter::markClipPlanesDirty);
  connect(&m_globalParameters.globalZCut,
          &ZFloatSpanParameter::valueChanged,
          this,
          &Z3DBoundedFilter::markClipPlanesDirty);
  connect(&m_boundBoxMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DBoundedFilter::onBoundBoxModeChanged);
  m_boundBoxMode.setDescription(QStringLiteral("Bounding box overlay mode (None / Object Box / Axis-Aligned Box)."));
  m_boundBoxLineColor.setStyle("COLOR");
  // m_boundBoxLineColor.get().reset(0., 1., QColor(133,163,240,255), QColor(248,60,35,255));
  // m_boundBoxLineColor.get().addKey(ZColorMapKey(.1, QColor(233,239,235,255)));
  // m_boundBoxLineColor.get().addKey(ZColorMapKey(.2, QColor(240,241,237,255)));
  // m_boundBoxLineColor.get().addKey(ZColorMapKey(.3, QColor(248,205,165,255)));
  connect(&m_boundBoxLineColor, &ZVec4Parameter::valueChanged, this, &Z3DBoundedFilter::updateBoundBoxLineColors);
  m_boundBoxLineWidth.setDescription(QStringLiteral("Line width (pixels) for the bounding box overlay."));
  m_boundBoxLineColor.setDescription(QStringLiteral("Bounding box line color (RGBA)."));
  m_selectionLineColor.setStyle("COLOR");
  connect(&m_selectionLineColor, &ZVec4Parameter::valueChanged, this, &Z3DBoundedFilter::updateSelectionLineColors);
  m_selectionLineWidth.setDescription(QStringLiteral("Line width (pixels) for selection highlight."));
  m_selectionLineColor.setDescription(QStringLiteral("Selection highlight line color (RGBA)."));

  addParameter(m_visible);
  addParameter(m_xCut);
  addParameter(m_yCut);
  addParameter(m_zCut);
  addParameter(m_boundBoxMode);
  addParameter(m_boundBoxLineWidth);
  addParameter(m_boundBoxLineColor);
  addParameter(m_selectionLineWidth);
  addParameter(m_selectionLineColor);

  connect(&m_globalParameters.fogMode, &Z3DCameraParameter::valueChanged, this, [this]() {
    syncRendererState();
    m_rendererBase.compile();
  });

  connect(&m_rendererParameters.coordTransform, &Z3DTransformParameter::valueChanged, this, [this]() {
    m_rendererParameterState.coordTransform = m_rendererParameters.coordTransform.get();
    // Local XYZ cuts are defined in the filter's untransformed/object space,
    // but clipping is evaluated in world space. Z3DRendererBase::setClipPlanes()
    // transforms plane equations by the inverse-transpose of coordTransform.
    //
    // When coordTransform is animated (timeline scrubbing / keyframes), we must
    // recompute those transformed planes; otherwise the clip planes remain at
    // the previous world-space location and appear as a "stuck" axis cut that
    // slices through the moving object (Vulkan and OpenGL both consume the
    // stored world-space planes).
    markClipPlanesDirty();
    updateAxisAlignedBoundBox();
    Q_EMIT rendererCoordTransformChanged();
  });

  connect(&m_rendererParameters.sizeScale, &ZParameter::valueChanged, this, [this]() {
    m_rendererParameterState.sizeScale = m_rendererParameters.sizeScale.get();
    updateBoundBox();
    Q_EMIT rendererSizeScaleChanged();
  });

  connect(&m_rendererParameters.opacity, &ZParameter::valueChanged, this, [this]() {
    m_rendererParameterState.opacity = m_rendererParameters.opacity.get();
  });

  connect(&m_rendererParameters.materialAmbient, &ZParameter::valueChanged, this, [this]() {
    m_rendererParameterState.materialAmbient = m_rendererParameters.materialAmbient.get();
  });

  connect(&m_rendererParameters.materialSpecular, &ZParameter::valueChanged, this, [this]() {
    m_rendererParameterState.materialSpecular = m_rendererParameters.materialSpecular.get();
  });

  connect(&m_rendererParameters.materialShininess, &ZParameter::valueChanged, this, [this]() {
    m_rendererParameterState.materialShininess = m_rendererParameters.materialShininess.get();
  });

  addParameter(m_rendererParameters.coordTransform);
  addParameter(m_rendererParameters.sizeScale);
  addParameter(m_rendererParameters.opacity);
  addParameter(m_rendererParameters.materialAmbient);
  addParameter(m_rendererParameters.materialSpecular);
  addParameter(m_rendererParameters.materialShininess);

  onBoundBoxModeChanged();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  m_baseBoundBoxRenderer.setUseDisplayList(false);
#endif
  m_baseBoundBoxRenderer.setFollowSizeScale(false);
  m_baseBoundBoxRenderer.setFollowCoordTransform(false);
  m_baseBoundBoxRenderer.setLineWidth(m_boundBoxLineWidth.get());
  connect(&m_boundBoxLineWidth, &ZIntParameter::valueChanged, this, &Z3DBoundedFilter::onBoundBoxLineWidthChanged);
  updateBoundBoxLineColors();

#if !defined(ATLAS_USE_CORE_PROFILE) && defined(ATLAS_SUPPORT_FIXED_PIPELINE)
  m_selectionBoundBoxRenderer.setUseDisplayList(false);
#endif
  m_selectionBoundBoxRenderer.setFollowCoordTransform(false);
  m_selectionBoundBoxRenderer.setFollowSizeScale(false);
  m_selectionBoundBoxRenderer.setFollowOpacity(false);
  m_selectionBoundBoxRenderer.setFollowSupersampling(false);
  m_selectionBoundBoxRenderer.setLineWidth(m_selectionLineWidth.get());
  connect(&m_selectionLineWidth,
          &ZIntParameter::valueChanged,
          this,
          &Z3DBoundedFilter::onSelectionBoundBoxLineWidthChanged);
  updateSelectionLineColors();

  m_selectionCornerRenderer.setColorSource(MeshColorSource::CustomColor);
  m_selectionCornerRenderer.setFollowCoordTransform(false);
  m_selectionCornerRenderer.setFollowOpacity(false);

  m_handleCenterRenderer.setFollowCoordTransform(false);
  m_handleCenterRenderer.setFollowOpacity(false);
  m_handleCenterRenderer.setFollowSizeScale(false);
  m_handleCenterRenderer.setNeedLighting(false);
  m_handleArrowRenderer.setFollowCoordTransform(false);
  m_handleArrowRenderer.setFollowOpacity(false);
  m_handleArrowRenderer.setFollowSizeScale(false);
  m_handleArrowRenderer.setNeedLighting(false);
  m_handleCenterColors.emplace_back(.5, .5, 0, .5);
  m_handleCenterRenderer.setDataColors(&m_handleCenterColors);
  m_handleArrowColors.emplace_back(.5, 0, 0, .5);
  m_handleArrowColors.emplace_back(0, .5, 0, .5);
  m_handleArrowColors.emplace_back(0, 0, .5, .5);
  m_handleArrowRenderer.setArrowColors(&m_handleArrowColors);
  registerHandlePickingColors();

  m_handleEvent.listenTo("transform handle", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
  m_handleEvent.listenTo("transform handle", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonRelease);
  m_handleEvent.listenTo("transform handle", Qt::LeftButton, Qt::NoModifier, QEvent::MouseMove);
  connect(&m_handleEvent, &ZEventListenerParameter::mouseEventTriggered, this, &Z3DBoundedFilter::handleEvent);
  addEventListener(m_handleEvent);
  m_handleEvent.setEnabled(m_isSelected);
}

void Z3DBoundedFilter::setSelected(bool v)
{
  if (m_isSelected != v) {
    m_isSelected = v;
    m_handleEvent.setEnabled(v);
    invalidateResult();
  }
}

void Z3DBoundedFilter::renderHandle(Z3DEye eye)
{
  if (m_isSelected && m_transformEnabled) {
    if (!m_handleValid) {
      updateHandle();
    }
    m_rendererBase.setClipEnabled(false);
    if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
      m_rendererBase.renderVulkan(eye, m_handleArrowRenderer, m_handleCenterRenderer);
    } else {
      m_rendererBase.render(eye, m_handleArrowRenderer, m_handleCenterRenderer);
    }
    m_rendererBase.setClipEnabled(true);
  }
}

void Z3DBoundedFilter::renderHandlePicking(Z3DEye eye)
{
  if (m_isSelected) {
    m_rendererBase.setClipEnabled(false);
    if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
      m_rendererBase.renderPickingVulkan(eye, m_handleArrowRenderer, m_handleCenterRenderer);
    } else {
      m_rendererBase.renderPicking(eye, m_handleArrowRenderer, m_handleCenterRenderer);
    }
    m_rendererBase.setClipEnabled(true);
  }
}

void Z3DBoundedFilter::renderSelectionBox(Z3DEye eye)
{
  if (m_isSelected) {
    m_selectionLines.resize(24);
    addSelectionLines();
    m_selectionBoundBoxRenderer.setData(std::span<const glm::vec3>(m_selectionLines));
    // Keep the renderer color buffer in sync with the current geometry size.
    //
    // Z3DLineRenderer::setData() resizes its internal color storage to match the
    // new vertex count. If the vertex count grows later and we don't restage the
    // colors, the newly-added vertices default to black (appearing grey in UI),
    // which makes only the first selection box look "yellow".
    if (m_selectionLineColors.size() != m_selectionLines.size()) {
      m_selectionLineColors.resize(m_selectionLines.size(), m_selectionLineColor.get());
      m_selectionBoundBoxRenderer.setDataColors(std::span<const glm::vec4>(m_selectionLineColors));
    }
    m_rendererBase.setClipEnabled(false);
    if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
      m_rendererBase.renderVulkan(eye, m_selectionBoundBoxRenderer, m_selectionCornerRenderer);
    } else {
      m_rendererBase.render(eye, m_selectionBoundBoxRenderer, m_selectionCornerRenderer);
    }
    m_rendererBase.setClipEnabled(true);
  }
}

void Z3DBoundedFilter::renderEditingSelectionBox(Z3DEye eye)
{
  if (!m_isSelected) {
    m_editingSelectionLines.clear();
    addEditingSelectionLines();
    if (m_editingSelectionLines.empty()) {
      return;
    }
    m_selectionBoundBoxRenderer.setData(std::span<const glm::vec3>(m_editingSelectionLines));
    // See renderSelectionBox() for why this must be keyed off geometry size,
    // not off whether our cached vector has ever been large enough.
    if (m_selectionLineColors.size() != m_editingSelectionLines.size()) {
      m_selectionLineColors.resize(m_editingSelectionLines.size(), m_selectionLineColor.get());
      m_selectionBoundBoxRenderer.setDataColors(std::span<const glm::vec4>(m_selectionLineColors));
    }
    m_rendererBase.setClipEnabled(false);
    if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
      m_rendererBase.renderVulkan(eye, m_selectionBoundBoxRenderer);
    } else {
      m_rendererBase.render(eye, m_selectionBoundBoxRenderer);
    }
    m_rendererBase.setClipEnabled(true);
  }
}

void Z3DBoundedFilter::rotateX()
{
  if (!m_isSelected || !m_transformEnabled) {
    return;
  }
  m_rendererParameters.coordTransform.rotate(glm::vec3(1, 0, 0), boost::math::float_constants::degree, m_center);
}

void Z3DBoundedFilter::rotateY()
{
  if (!m_isSelected || !m_transformEnabled) {
    return;
  }
  m_rendererParameters.coordTransform.rotate(glm::vec3(0, 1, 0), boost::math::float_constants::degree, m_center);
}

void Z3DBoundedFilter::rotateZ()
{
  if (!m_isSelected || !m_transformEnabled) {
    return;
  }
  m_rendererParameters.coordTransform.rotate(glm::vec3(0, 0, 1), boost::math::float_constants::degree, m_center);
}

void Z3DBoundedFilter::rotateXM()
{
  if (!m_isSelected || !m_transformEnabled) {
    return;
  }
  m_rendererParameters.coordTransform.rotate(glm::vec3(1, 0, 0), -boost::math::float_constants::degree, m_center);
}

void Z3DBoundedFilter::rotateYM()
{
  if (!m_isSelected || !m_transformEnabled) {
    return;
  }
  m_rendererParameters.coordTransform.rotate(glm::vec3(0, 1, 0), -boost::math::float_constants::degree, m_center);
}

void Z3DBoundedFilter::rotateZM()
{
  if (!m_isSelected || !m_transformEnabled) {
    return;
  }
  m_rendererParameters.coordTransform.rotate(glm::vec3(0, 0, 1), -boost::math::float_constants::degree, m_center);
}

ZBBox<glm::dvec3> Z3DBoundedFilter::axisAlignedBoundBoxAfterClipping() const
{
  ZBBox<glm::dvec3> res;
  if (auto notTransformedBBAfterCutting = notTransformedBoundBoxAfterClipping();
      !notTransformedBBAfterCutting.empty()) {
    glm::dmat4 tfm(m_rendererParameters.coordTransform.get());
    auto physicalLUF = notTransformedBBAfterCutting.minCorner;
    auto physicalRDB = notTransformedBBAfterCutting.maxCorner;
    res.expand(glm::applyMatrix(tfm, physicalLUF));
    res.expand(glm::applyMatrix(tfm, glm::dvec3(physicalLUF.x, physicalRDB.y, physicalRDB.z)));
    res.expand(glm::applyMatrix(tfm, glm::dvec3(physicalLUF.x, physicalRDB.y, physicalLUF.z)));
    res.expand(glm::applyMatrix(tfm, glm::dvec3(physicalLUF.x, physicalLUF.y, physicalRDB.z)));
    res.expand(glm::applyMatrix(tfm, glm::dvec3(physicalRDB.x, physicalLUF.y, physicalLUF.z)));
    res.expand(glm::applyMatrix(tfm, physicalRDB));
    res.expand(glm::applyMatrix(tfm, glm::dvec3(physicalRDB.x, physicalRDB.y, physicalLUF.z)));
    res.expand(glm::applyMatrix(tfm, glm::dvec3(physicalRDB.x, physicalLUF.y, physicalRDB.z)));
  }
  return res;
}

ZBBox<glm::dvec3> Z3DBoundedFilter::notTransformedBoundBoxAfterClipping() const
{
  ZBBox<glm::dvec3> res = notTransformedBoundBox();
  res.setMinCorner(glm::max(res.minCorner, glm::dvec3(m_xCut.get().x, m_yCut.get().x, m_zCut.get().x)));
  res.setMaxCorner(glm::min(res.maxCorner, glm::dvec3(m_xCut.get().y, m_yCut.get().y, m_zCut.get().y)));
  return res;
}

void Z3DBoundedFilter::updateBoundBox()
{
  updateNotTransformedBoundBox();
  m_normalBoundBoxLines.clear();
  appendBoundboxLines(m_notTransformedBoundBox, m_normalBoundBoxLines);
  if (m_boundBoxMode.isSelected("Bound Box")) {
    m_baseBoundBoxRenderer.setData(std::span<const glm::vec3>(m_normalBoundBoxLines));
  }
  updateAxisAlignedBoundBox();
}

void Z3DBoundedFilter::setClipPlanes()
{
  if (!m_canUpdateClipPlane) {
    m_clipPlanesDirty = true;
    return;
  }

  std::vector<glm::vec4> clipPlanes;

  // Semantics:
  // - Local XYZ cuts (m_xCut/m_yCut/m_zCut) are defined in the filter's
  //   untransformed/object space.
  // - Global XYZ cuts (globalXCut/globalYCut/globalZCut) are defined in world
  //   space and must remain fixed as objects animate via coordTransform.
  //
  // Z3DRendererBase::setClipPlanes() expects *object-space* plane equations and
  // converts them to world space via M^{-T}, where M is coordTransform.
  //
  // Therefore:
  // - Local planes are appended directly (object-space).
  // - Global (world-space) planes are converted into the expected input space:
  //     p_in = M^{T} * p_world
  //   so that the renderer's internal transform yields:
  //     M^{-T} * p_in = p_world
  const glm::mat4 coordTransform = m_rendererParameters.coordTransform.get();
  const glm::mat4 coordTransformT = glm::transpose(coordTransform);

  auto appendLocalPlane = [&](const glm::vec4& pObject) {
    clipPlanes.emplace_back(pObject);
  };
  auto appendWorldPlane = [&](const glm::vec4& pWorld) {
    clipPlanes.emplace_back(coordTransformT * pWorld);
  };

  if (m_xCut.lowerValue() != m_xCut.minimum()) {
    appendLocalPlane(glm::vec4(1.f, 0.f, 0.f, -m_xCut.lowerValue()));
  }
  if (m_xCut.upperValue() != m_xCut.maximum()) {
    appendLocalPlane(glm::vec4(-1.f, 0.f, 0.f, m_xCut.upperValue()));
  }
  if (m_yCut.lowerValue() != m_yCut.minimum()) {
    appendLocalPlane(glm::vec4(0.f, 1.f, 0.f, -m_yCut.lowerValue()));
  }
  if (m_yCut.upperValue() != m_yCut.maximum()) {
    appendLocalPlane(glm::vec4(0.f, -1.f, 0.f, m_yCut.upperValue()));
  }
  if (m_zCut.lowerValue() != m_zCut.minimum()) {
    appendLocalPlane(glm::vec4(0.f, 0.f, 1.f, -m_zCut.lowerValue()));
  }
  if (m_zCut.upperValue() != m_zCut.maximum()) {
    appendLocalPlane(glm::vec4(0.f, 0.f, -1.f, m_zCut.upperValue()));
  }

  if (m_globalParameters.globalXCut.lowerValue() != m_globalParameters.globalXCut.minimum()) {
    appendWorldPlane(glm::vec4(1.f, 0.f, 0.f, -m_globalParameters.globalXCut.lowerValue()));
  }
  if (m_globalParameters.globalXCut.upperValue() != m_globalParameters.globalXCut.maximum()) {
    appendWorldPlane(glm::vec4(-1.f, 0.f, 0.f, m_globalParameters.globalXCut.upperValue()));
  }
  if (m_globalParameters.globalYCut.lowerValue() != m_globalParameters.globalYCut.minimum()) {
    appendWorldPlane(glm::vec4(0.f, 1.f, 0.f, -m_globalParameters.globalYCut.lowerValue()));
  }
  if (m_globalParameters.globalYCut.upperValue() != m_globalParameters.globalYCut.maximum()) {
    appendWorldPlane(glm::vec4(0.f, -1.f, 0.f, m_globalParameters.globalYCut.upperValue()));
  }
  if (m_globalParameters.globalZCut.lowerValue() != m_globalParameters.globalZCut.minimum()) {
    appendWorldPlane(glm::vec4(0.f, 0.f, 1.f, -m_globalParameters.globalZCut.lowerValue()));
  }
  if (m_globalParameters.globalZCut.upperValue() != m_globalParameters.globalZCut.maximum()) {
    appendWorldPlane(glm::vec4(0.f, 0.f, -1.f, m_globalParameters.globalZCut.upperValue()));
  }

  m_rendererBase.setClipPlanes(&clipPlanes);
  m_clipPlanesDirty = false;
}

void Z3DBoundedFilter::handleEvent(QMouseEvent* e, int w, int h)
{
  e->ignore();
  // Mouse button pressend
  if (e->type() == QEvent::MouseButtonPress) {
    m_lastMousePosition = glm::ivec2(e->position().x(), e->position().y());
    const void* obj = pickingManager().objectAtWidgetPos(m_lastMousePosition);
    int handleIdx = selectedHandle(obj);
    if (handleIdx == 0) {
      return;
    }
    updateSelectedHandle(handleIdx);
    glm::ivec4 viewport(0, 0, w, h);
    m_startTrans = m_rendererParameters.coordTransform.translation();
    if (handleIdx == 1) {
      m_startDepth = m_globalParameters.camera.get().worldToScreen(m_center, viewport).z;
      m_startMouseWorldPos =
        m_globalParameters.camera.get().screenToWorld(glm::vec3(e->position().x(), h - e->position().y(), m_startDepth),
                                                      viewport);
    } else {
      GLfloat WindowPosZ = pickingManager().depthAtWidgetPos(glm::ivec2(e->position().x(), e->position().y()));
      CHECK_GL_ERROR
      m_startMouseWorldPos =
        m_globalParameters.camera.get().screenToWorld(glm::vec3(e->position().x(), h - e->position().y(), WindowPosZ),
                                                      viewport);
    }
    e->accept();
    return;
  }

  if (e->type() == QEvent::MouseMove) {
    if (m_selectedHandle == 0) {
      return;
    }
    if (m_selectedHandle == 1) {
      glm::vec3 endInWorld = m_globalParameters.camera.get().screenToWorld(
        glm::vec3(glm::vec2(e->position().x(), h - e->position().y()), m_startDepth),
        glm::ivec4(0, 0, w, h));
      m_rendererParameters.coordTransform.setTranslation(m_startTrans + endInWorld - m_startMouseWorldPos);
    } else {
      glm::vec3 v1, v2;
      rayUnderScreenPoint(v1, v2, e->position().x(), e->position().y(), w, h);
      v2 -= v1;
      gte::Ray<3, float> ray(gte::Vector<3, float>{v1.x, v1.y, v1.z}, gte::Vector<3, float>{v2.x, v2.y, v2.z});
      gte::DCPLineRay<3, float> dist;
      if (m_selectedHandle == 2) {
        gte::Line<3, float> xLine(
          gte::Vector<3, float>{m_startMouseWorldPos.x, m_startMouseWorldPos.y, m_startMouseWorldPos.z},
          gte::Vector<3, float>{1, 0, 0});
        auto result = dist(xLine, ray);
        m_rendererParameters.coordTransform.setTranslation(m_startTrans + glm::vec3(result.parameter[0], 0, 0));
      } else if (m_selectedHandle == 3) {
        gte::Line<3, float> xLine(
          gte::Vector<3, float>{m_startMouseWorldPos.x, m_startMouseWorldPos.y, m_startMouseWorldPos.z},
          gte::Vector<3, float>{0, 1, 0});
        auto result = dist(xLine, ray);
        m_rendererParameters.coordTransform.setTranslation(m_startTrans + glm::vec3(0, result.parameter[0], 0));
      } else if (m_selectedHandle == 4) {
        gte::Line<3, float> xLine(
          gte::Vector<3, float>{m_startMouseWorldPos.x, m_startMouseWorldPos.y, m_startMouseWorldPos.z},
          gte::Vector<3, float>{0, 0, 1});
        auto result = dist(xLine, ray);
        m_rendererParameters.coordTransform.setTranslation(m_startTrans + glm::vec3(0, 0, result.parameter[0]));
      }
    }
    m_lastMousePosition = glm::ivec2(e->position().x(), e->position().y());
    return;
  }

  if (e->type() == QEvent::MouseButtonRelease) {
    updateSelectedHandle(0);
  }
}

void Z3DBoundedFilter::initializeCutRange()
{
  m_canUpdateClipPlane = false;
  const ZBBox<glm::dvec3>& bound = notTransformedBoundBox();
  m_xCut.setRange(std::floor(bound.minCorner.x) - 1, std::ceil(bound.maxCorner.x) + 1);
  m_xCut.set(m_xCut.range());
  m_yCut.setRange(std::floor(bound.minCorner.y) - 1, std::ceil(bound.maxCorner.y) + 1);
  m_yCut.set(m_yCut.range());
  m_zCut.setRange(std::floor(bound.minCorner.z) - 1, std::ceil(bound.maxCorner.z) + 1);
  m_zCut.set(m_zCut.range());
  m_canUpdateClipPlane = true;
  m_rendererBase.setClipPlanes(nullptr);
}

void Z3DBoundedFilter::initializeRotationCenter()
{
  const ZBBox<glm::dvec3>& bound = notTransformedBoundBox();
  m_rendererParameters.coordTransform.setRotationCenter(glm::vec3((bound.minCorner + bound.maxCorner) / 2.0));
}

void Z3DBoundedFilter::initializeRotationCenterIfDefault()
{
  auto& transform = m_rendererParameters.coordTransform;

  auto isNearZeroVec3 = [](const glm::vec3& v) {
    constexpr float kVecEps = 1e-6f;
    return std::abs(v.x) <= kVecEps && std::abs(v.y) <= kVecEps && std::abs(v.z) <= kVecEps;
  };
  auto isNearOneVec3 = [](const glm::vec3& v) {
    constexpr float kVecEps = 1e-6f;
    return std::abs(v.x - 1.f) <= kVecEps && std::abs(v.y - 1.f) <= kVecEps && std::abs(v.z - 1.f) <= kVecEps;
  };
  auto isNearIdentityQuat = [](const glm::quat& q) {
    constexpr float kQuatEps = 1e-6f;
    const float absW = std::abs(q.w);
    return std::abs(absW - 1.f) <= kQuatEps && std::abs(q.x) <= kQuatEps && std::abs(q.y) <= kQuatEps &&
           std::abs(q.z) <= kQuatEps;
  };

  const glm::vec3 scale = transform.scale();
  const glm::vec3 translation = transform.translation();
  const glm::quat rotation = transform.rotation();
  const glm::vec3 rotationCenter = transform.rotationCenter();

  const bool isDefaultTransform = isNearOneVec3(scale) && isNearZeroVec3(translation) && isNearIdentityQuat(rotation) &&
                                  isNearZeroVec3(rotationCenter);
  if (!isDefaultTransform) {
    return;
  }

  const ZBBox<glm::dvec3>& bound = notTransformedBoundBox();
  if (bound.empty()) {
    return;
  }

  const glm::vec3 desiredCenter = glm::vec3((bound.minCorner + bound.maxCorner) / 2.0);
  if (isNearZeroVec3(desiredCenter - rotationCenter)) {
    return;
  }

  transform.setRotationCenter(desiredCenter);
}

void Z3DBoundedFilter::renderBoundBox(Z3DEye eye)
{
  if (!m_boundBoxMode.isSelected("No Bound Box")) {
    m_rendererBase.setClipEnabled(false);
    if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
      m_rendererBase.renderVulkan(eye, m_baseBoundBoxRenderer);
    } else {
      m_rendererBase.render(eye, m_baseBoundBoxRenderer);
    }
    m_rendererBase.setClipEnabled(true);
  }
}

void Z3DBoundedFilter::renderBoundBox(Z3DEye eye, BoundBoxRenderStyle style)
{
  if (style == BoundBoxRenderStyle::InheritState) {
    renderBoundBox(eye);
    return;
  }
  if (!m_boundBoxMode.isSelected("No Bound Box")) {
    if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
      m_rendererBase.setClipEnabled(false);
      m_rendererBase.renderVulkan(eye, m_baseBoundBoxRenderer);
      m_rendererBase.setClipEnabled(true);
    } else {
      glEnable(GL_DEPTH_TEST);
      glEnable(GL_BLEND);
      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      auto guard = folly::makeGuard([]() {
        glBlendFunc(GL_ONE, GL_ZERO);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
      });
      m_rendererBase.setClipEnabled(false);
      m_rendererBase.render(eye, m_baseBoundBoxRenderer);
      m_rendererBase.setClipEnabled(true);
    }
  }
}

void Z3DBoundedFilter::switchRendererBackend(RenderBackend backendRequest)
{
  VLOG(1) << "Switching renderer backend for " << className();
  m_rendererBase.setBackend(backendRequest);
}

void Z3DBoundedFilter::pushRendererParametersToBase()
{
  m_rendererParameterState.coordTransform = m_rendererParameters.coordTransform.get();
  m_rendererParameterState.sizeScale = m_rendererParameters.sizeScale.get();
  m_rendererParameterState.opacity = m_rendererParameters.opacity.get();
  m_rendererParameterState.materialAmbient = m_rendererParameters.materialAmbient.get();
  m_rendererParameterState.materialSpecular = m_rendererParameters.materialSpecular.get();
  m_rendererParameterState.materialShininess = m_rendererParameters.materialShininess.get();
}

void Z3DBoundedFilter::syncRendererState()
{
  updateClipPlanesIfDirty();

  auto& globalState = Z3DRenderGlobalState::instance();
  globalState.ensureSceneState(m_globalParameters);
  globalState.ensureViewState(m_globalParameters.camera.get());
}

void Z3DBoundedFilter::markClipPlanesDirty()
{
  m_clipPlanesDirty = true;
}

void Z3DBoundedFilter::updateClipPlanesIfDirty()
{
  if (!m_clipPlanesDirty) {
    return;
  }
  if (!m_canUpdateClipPlane) {
    return;
  }
  setClipPlanes();
  m_clipPlanesDirty = false;
}

void Z3DBoundedFilter::appendBoundboxLines(const ZBBox<glm::dvec3>& bound, std::vector<glm::vec3>& lines)
{
  float xmin = bound.minCorner.x;
  float xmax = bound.maxCorner.x;
  float ymin = bound.minCorner.y;
  float ymax = bound.maxCorner.y;
  float zmin = bound.minCorner.z;
  float zmax = bound.maxCorner.z;
  lines.emplace_back(xmin, ymin, zmin);
  lines.emplace_back(xmin, ymin, zmax);
  lines.emplace_back(xmin, ymax, zmin);
  lines.emplace_back(xmin, ymax, zmax);

  lines.emplace_back(xmax, ymin, zmin);
  lines.emplace_back(xmax, ymin, zmax);
  lines.emplace_back(xmax, ymax, zmin);
  lines.emplace_back(xmax, ymax, zmax);

  lines.emplace_back(xmin, ymin, zmin);
  lines.emplace_back(xmax, ymin, zmin);
  lines.emplace_back(xmin, ymax, zmin);
  lines.emplace_back(xmax, ymax, zmin);

  lines.emplace_back(xmin, ymin, zmax);
  lines.emplace_back(xmax, ymin, zmax);
  lines.emplace_back(xmin, ymax, zmax);
  lines.emplace_back(xmax, ymax, zmax);

  lines.emplace_back(xmin, ymin, zmin);
  lines.emplace_back(xmin, ymax, zmin);
  lines.emplace_back(xmax, ymin, zmin);
  lines.emplace_back(xmax, ymax, zmin);

  lines.emplace_back(xmin, ymin, zmax);
  lines.emplace_back(xmin, ymax, zmax);
  lines.emplace_back(xmax, ymin, zmax);
  lines.emplace_back(xmax, ymax, zmax);
}

void Z3DBoundedFilter::rayUnderScreenPoint(glm::vec3& v1, glm::vec3& v2, int x, int y, int width, int height)
{
  const glm::mat4& projection = m_globalParameters.camera.get().projectionMatrix(MonoEye);
  const glm::mat4& modelview = m_globalParameters.camera.get().viewMatrix(MonoEye);

  glm::ivec4 viewport;
  viewport[0] = 0;
  viewport[1] = 0;
  viewport[2] = width;
  viewport[3] = height;

  v1 = glm::unProject(glm::vec3(x, height - y, 0.f), modelview, projection, viewport);
  v2 = glm::unProject(glm::vec3(x, height - y, 1.f), modelview, projection, viewport);
  v2 = glm::normalize(v2 - v1) + v1;
}

void Z3DBoundedFilter::rayUnderScreenPoint(glm::dvec3& v1, glm::dvec3& v2, int x, int y, int width, int height)
{
  const glm::dmat4& projection = glm::dmat4(m_globalParameters.camera.get().projectionMatrix(MonoEye));
  const glm::dmat4& modelview = glm::dmat4(m_globalParameters.camera.get().viewMatrix(MonoEye));

  glm::ivec4 viewport;
  viewport[0] = 0;
  viewport[1] = 0;
  viewport[2] = width;
  viewport[3] = height;

  v1 = glm::unProject(glm::dvec3(x, height - y, 0.f), modelview, projection, viewport);
  v2 = glm::unProject(glm::dvec3(x, height - y, 1.f), modelview, projection, viewport);
  v2 = glm::normalize(v2 - v1) + v1;
}

void Z3DBoundedFilter::updateAxisAlignedBoundBoxImpl()
{
  m_axisAlignedBoundBox.reset();
  if (!m_notTransformedBoundBox.empty()) {
    m_axisAlignedBoundBox.expand(glm::dvec3(worldLUF()));
    m_axisAlignedBoundBox.expand(glm::dvec3(worldLDB()));
    m_axisAlignedBoundBox.expand(glm::dvec3(worldLDF()));
    m_axisAlignedBoundBox.expand(glm::dvec3(worldLUB()));
    m_axisAlignedBoundBox.expand(glm::dvec3(worldRUF()));
    m_axisAlignedBoundBox.expand(glm::dvec3(worldRDB()));
    m_axisAlignedBoundBox.expand(glm::dvec3(worldRDF()));
    m_axisAlignedBoundBox.expand(glm::dvec3(worldRUB()));
  }
}

void Z3DBoundedFilter::expandCutRange()
{
  m_canUpdateClipPlane = false;
  const ZBBox<glm::dvec3>& bound = notTransformedBoundBox();
  bool noLowXCut = m_xCut.lowerValue() == m_xCut.minimum();
  bool noHighXCut = m_xCut.upperValue() == m_xCut.maximum();
  bool noLowYCut = m_yCut.lowerValue() == m_yCut.minimum();
  bool noHighYCut = m_yCut.upperValue() == m_yCut.maximum();
  bool noLowZCut = m_zCut.lowerValue() == m_zCut.minimum();
  bool noHighZCut = m_zCut.upperValue() == m_zCut.maximum();
  m_xCut.setRange(std::min(m_xCut.minimum(), float(std::floor(bound.minCorner.x) - 1)),
                  std::max(m_xCut.maximum(), float(std::ceil(bound.maxCorner.x) + 1)));
  m_yCut.setRange(std::min(m_yCut.minimum(), float(std::floor(bound.minCorner.y) - 1)),
                  std::max(m_yCut.maximum(), float(std::ceil(bound.maxCorner.y) + 1)));
  m_zCut.setRange(std::min(m_zCut.minimum(), float(std::floor(bound.minCorner.z) - 1)),
                  std::max(m_zCut.maximum(), float(std::ceil(bound.maxCorner.z) + 1)));
  float xCutLow = noLowXCut ? m_xCut.minimum() : m_xCut.get().x;
  float xCutHigh = noHighXCut ? m_xCut.maximum() : m_xCut.get().y;
  float yCutLow = noLowYCut ? m_yCut.minimum() : m_yCut.get().x;
  float yCutHigh = noHighYCut ? m_yCut.maximum() : m_yCut.get().y;
  float zCutLow = noLowZCut ? m_zCut.minimum() : m_zCut.get().x;
  float zCutHigh = noHighZCut ? m_zCut.maximum() : m_zCut.get().y;
  m_xCut.set(glm::vec2(xCutLow, xCutHigh));
  m_yCut.set(glm::vec2(yCutLow, yCutHigh));
  m_zCut.set(glm::vec2(zCutLow, zCutHigh));
  m_canUpdateClipPlane = true;
  setClipPlanes();
}

void Z3DBoundedFilter::updateAxisAlignedBoundBox()
{
  if (m_rendererParameters.coordTransform.get() == glm::mat4(1.f)) {
    m_axisAlignedBoundBox = m_notTransformedBoundBox;
  } else {
    updateAxisAlignedBoundBoxImpl();
  }
  m_axisAlignedBoundBoxLines.clear();
  appendBoundboxLines(m_axisAlignedBoundBox, m_axisAlignedBoundBoxLines);
  if (m_boundBoxMode.isSelected("Axis Aligned Bound Box")) {
    m_baseBoundBoxRenderer.setData(std::span<const glm::vec3>(m_axisAlignedBoundBoxLines));
  }

  m_center = glm::vec3((m_axisAlignedBoundBox.minCorner + m_axisAlignedBoundBox.maxCorner) / 2.0);

  makeSelectionGeometries();
  m_handleValid = false;

  Q_EMIT boundBoxChanged();
}

void Z3DBoundedFilter::updateNotTransformedBoundBox()
{
  updateNotTransformedBoundBoxImpl();
  expandCutRange();
}

void Z3DBoundedFilter::onBoundBoxModeChanged()
{
  if (m_boundBoxMode.isSelected("Axis Aligned Bound Box")) {
    m_baseBoundBoxRenderer.setData(std::span<const glm::vec3>(m_axisAlignedBoundBoxLines));
    m_baseBoundBoxRenderer.setFollowCoordTransform(false);
  } else if (m_boundBoxMode.isSelected("Bound Box")) {
    m_baseBoundBoxRenderer.setData(std::span<const glm::vec3>(m_normalBoundBoxLines));
    m_baseBoundBoxRenderer.setFollowCoordTransform(true);
  }
  m_boundBoxLineWidth.setVisible(!m_boundBoxMode.isSelected("No Bound Box"));
  m_boundBoxLineColor.setVisible(!m_boundBoxMode.isSelected("No Bound Box"));
}

void Z3DBoundedFilter::updateBoundBoxLineColors()
{
  m_boundBoxLineColors.clear();
  m_boundBoxLineColors.resize(24, m_boundBoxLineColor.get());
  m_baseBoundBoxRenderer.setDataColors(std::span<const glm::vec4>(m_boundBoxLineColors));
  // m_baseBoundBoxRenderer->setTexture(m_boundBoxLineColor.get().getTexture());
}

void Z3DBoundedFilter::updateSelectionLineColors()
{
  m_selectionLineColors.clear();
  m_selectionLineColors.resize(24, m_selectionLineColor.get());
  m_selectionBoundBoxRenderer.setDataColors(std::span<const glm::vec4>(m_selectionLineColors));
  m_selectionCornerRenderer.setDataColors(&m_selectionLineColors);
}

void Z3DBoundedFilter::onBoundBoxLineWidthChanged()
{
  m_baseBoundBoxRenderer.setLineWidth(m_boundBoxLineWidth.get());
}

void Z3DBoundedFilter::onSelectionBoundBoxLineWidthChanged()
{
  m_selectionBoundBoxRenderer.setLineWidth(m_selectionLineWidth.get());
}

void Z3DBoundedFilter::makeSelectionGeometries()
{
  auto bbsz = m_axisAlignedBoundBox.size();
  auto size =
    bbsz.x + bbsz.y + bbsz.z - std::max(bbsz.z, std::max(bbsz.x, bbsz.y)) - std::min(bbsz.z, std::min(bbsz.x, bbsz.y));
  auto cornerRadius = std::min(100.0, 0.01 * size);
  m_selectionBoundBox = m_axisAlignedBoundBox;
  m_selectionBoundBox.expand(cornerRadius);
  m_selectionLines.clear();
  appendBoundboxLines(m_selectionBoundBox, m_selectionLines);

  glm::vec3 cornerShift(cornerRadius, cornerRadius, cornerRadius);

  std::vector<glm::vec3> lowcoords;
  std::vector<glm::vec3> highcoords;
  double bd[6];
  bd[0] = m_selectionBoundBox.minCorner.x;
  bd[1] = m_selectionBoundBox.maxCorner.x;
  bd[2] = m_selectionBoundBox.minCorner.y;
  bd[3] = m_selectionBoundBox.maxCorner.y;
  bd[4] = m_selectionBoundBox.minCorner.z;
  bd[5] = m_selectionBoundBox.maxCorner.z;
  for (auto k = 0; k < 2; ++k) {
    for (auto j = 0; j < 2; ++j) {
      for (auto i = 0; i < 2; ++i) {
        glm::vec3 pos(bd[i], bd[2 + j], bd[4 + k]);
        lowcoords.push_back(pos - cornerShift);
        highcoords.push_back(pos + cornerShift);
      }
    }
  }

  m_selectionCornerCubes = ZMesh::createCubesWithNormal(lowcoords, highcoords);
  m_selectionCornerCubesWrapper.clear();
  m_selectionCornerCubesWrapper.push_back(&m_selectionCornerCubes);
  m_selectionCornerRenderer.setData(&m_selectionCornerCubesWrapper);
}

void Z3DBoundedFilter::updateHandle()
{
  if (!m_handleValid) {
    Z3DCamera& camera = m_globalParameters.camera.get();
    glm::mat4 mat = m_rendererFrameState.viewportMatrix * camera.projectionMatrix(MonoEye) * camera.viewMatrix(MonoEye);
    glm::vec3 rightVector(camera.viewMatrix(MonoEye)[0][0],
                          camera.viewMatrix(MonoEye)[1][0],
                          camera.viewMatrix(MonoEye)[2][0]);
    glm::vec3 centerScreen = glm::applyMatrix(mat, m_center);
    glm::vec3 rightScreen = glm::applyMatrix(mat, m_center + rightVector);
    float size = m_manipulatorSize.get() / (rightScreen.x - centerScreen.x);

    m_handleCenterAndRadius[0] = glm::vec4(m_center, 0.18f * size);
    m_handleArrowTailPosAndTailRadius[0] = glm::vec4(m_center, 0.028f * size);
    m_handleArrowTailPosAndTailRadius[1] = glm::vec4(m_center, 0.028f * size);
    m_handleArrowTailPosAndTailRadius[2] = glm::vec4(m_center, 0.028f * size);
    m_handleArrowheadPosAndHeadRadius[0] = glm::vec4(m_center, 0.12f * size) + glm::vec4(size, 0, 0, 0);
    m_handleArrowheadPosAndHeadRadius[1] = glm::vec4(m_center, 0.12f * size) + glm::vec4(0, size, 0, 0);
    m_handleArrowheadPosAndHeadRadius[2] = glm::vec4(m_center, 0.12f * size) + glm::vec4(0, 0, size, 0);
    m_handleCenterRenderer.setData(&m_handleCenterAndRadius);
    m_handleArrowRenderer.setArrowData(&m_handleArrowTailPosAndTailRadius, &m_handleArrowheadPosAndHeadRadius);

    m_handleValid = true;
  }
}

void Z3DBoundedFilter::registerHandlePickingColors()
{
  pickingManager().registerObject(&m_handleCenterRenderer); // center
  pickingManager().registerObject(&m_handleArrowRenderer); // axis x
  pickingManager().registerObject(&m_handleArrowTailPosAndTailRadius); // axis y
  pickingManager().registerObject(&m_handleArrowheadPosAndHeadRadius); // axis z
  m_handleCenterPickingColors.push_back(pickingManager().fColorOfObject(&m_handleCenterRenderer));
  m_handleArrowPickingColors.push_back(pickingManager().fColorOfObject(&m_handleArrowRenderer));
  m_handleArrowPickingColors.push_back(pickingManager().fColorOfObject(&m_handleArrowTailPosAndTailRadius));
  m_handleArrowPickingColors.push_back(pickingManager().fColorOfObject(&m_handleArrowheadPosAndHeadRadius));
  m_handleCenterRenderer.setDataPickingColors(&m_handleCenterPickingColors);
  m_handleArrowRenderer.setArrowPickingColors(&m_handleArrowPickingColors);
}

int Z3DBoundedFilter::selectedHandle(const void* obj) const
{
  if (obj == &m_handleCenterRenderer) {
    return 1;
  }
  if (obj == &m_handleArrowRenderer) {
    return 2;
  }
  if (obj == &m_handleArrowTailPosAndTailRadius) {
    return 3;
  }
  if (obj == &m_handleArrowheadPosAndHeadRadius) {
    return 4;
  }

  return 0;
}

void Z3DBoundedFilter::updateSelectedHandle(int handleIdx)
{
  if (handleIdx == m_selectedHandle) {
    return;
  }

  if (handleIdx == 0) {
    switch (m_selectedHandle) {
      case 1:
        m_handleCenterColors[0] = glm::vec4(.5, .5, 0, .5);
        m_handleCenterRenderer.setDataColors(&m_handleCenterColors);
        break;
      case 2:
        m_handleArrowColors[0] = glm::vec4(.5, 0, 0, .5);
        m_handleArrowRenderer.setArrowColors(&m_handleArrowColors);
        break;
      case 3:
        m_handleArrowColors[1] = glm::vec4(0, .5, 0, .5);
        m_handleArrowRenderer.setArrowColors(&m_handleArrowColors);
        break;
      case 4:
        m_handleArrowColors[2] = glm::vec4(0, 0, .5, .5);
        m_handleArrowRenderer.setArrowColors(&m_handleArrowColors);
        break;
      default:
        CHECK(false);
    }
    interactionHandler().setEnabled(true);
  } else {
    CHECK(m_selectedHandle == 0);
    float av = 95.f / 255.f;
    switch (handleIdx) {
      case 1:
        m_handleCenterColors[0] = glm::vec4(.5, .5, av, .5);
        m_handleCenterRenderer.setDataColors(&m_handleCenterColors);
        break;
      case 2:
        m_handleArrowColors[0] = glm::vec4(.5, av, av, .5);
        m_handleArrowRenderer.setArrowColors(&m_handleArrowColors);
        break;
      case 3:
        m_handleArrowColors[1] = glm::vec4(av, .5, av, .5);
        m_handleArrowRenderer.setArrowColors(&m_handleArrowColors);
        break;
      case 4:
        m_handleArrowColors[2] = glm::vec4(av, av, .5, .5);
        m_handleArrowRenderer.setArrowColors(&m_handleArrowColors);
        break;
      default:
        CHECK(false);
    }
    interactionHandler().setEnabled(false);
  }

  invalidateResult();
  m_selectedHandle = handleIdx;
}
} // namespace nim
