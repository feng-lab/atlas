#include "z3dpunctafilter.h"

#include "zpuncta.h"
#include "zrandom.h"

namespace nim {

Z3DPunctaFilter::Z3DPunctaFilter(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DGeometryFilter(globalParas, parent)
  , m_sphereRenderer(m_rendererBase)
  , m_colorMode("Color Mode")
  , m_singleColorForAllPuncta("Puncta Color",
                              glm::vec4(ZRandom::instance().randReal<float>(),
                                        ZRandom::instance().randReal<float>(),
                                        ZRandom::instance().randReal<float>(),
                                        1.f))
  , m_colorMapScore("Score Color Map", -1., 1., QColor(255, 255, 0), QColor(0, 0, 255))
  , m_colorMapMeanIntensity("Mean Intensity Color Map", 0., 1., QColor(255, 0, 0), QColor(0, 0, 0))
  , m_colorMapMaxIntensity("Max Intensity Color Map", 0., 1., QColor(255, 0, 0), QColor(0, 0, 0))
  , m_useSameSizeForAllPuncta("Use Same Size", false)
  , m_useDynamicMaterial("Calculate Material Property From Intensity", true)
  //  , m_glowSphereRenderer(m_rendererBase)
  //  , m_textureGlowRenderer(m_rendererBase)
  //  , m_randomGlow("Random Glow", false)
  //  , m_glowPercentage("Glow Percentage", 0.2f, 0.f, 1.f)
  //  , m_textureCopyRenderer(m_rendererBase)
  , m_selectPunctumEvent("Select Puncta", false)
  , m_deleteSelectedPunctaEvent("Delete Selected Puncta", true)
  , m_contextMenuEvent("Context Menu", false)
{
  //  addPrivateRenderPort(m_monoEyeOutport);
  //  addPrivateRenderPort(m_leftEyeOutRenderTarget1);
  //  addPrivateRenderPort(m_rightEyeOutRenderTarget1);
  //  addPrivateRenderPort(m_monoEyeOutport2);
  //  addPrivateRenderPort(m_leftEyeOutport2);
  //  addPrivateRenderPort(m_rightEyeOutport2);

  // m_textureCopyRenderer.setDiscardTransparent(true);

  m_singleColorForAllPuncta.setStyle("COLOR");
  connect(&m_singleColorForAllPuncta, &ZVec4Parameter::valueChanged, this, &Z3DPunctaFilter::prepareColor);
  connect(&m_colorMapScore, &ZColorMapParameter::valueChanged, this, &Z3DPunctaFilter::prepareColor);
  connect(&m_colorMapMeanIntensity, &ZColorMapParameter::valueChanged, this, &Z3DPunctaFilter::prepareColor);
  connect(&m_colorMapMaxIntensity, &ZColorMapParameter::valueChanged, this, &Z3DPunctaFilter::prepareColor);

  // Color Mode
  m_colorMode.addOptions("Single Color", "Random Color", "Original Point Color", "Colormap Score");
  m_colorMode.select("Single Color");
  m_colorMode.setDescription(QStringLiteral(
    "Controls how puncta are colored:\n"
    "- 'Single Color' uses the 'Puncta Color' parameter below.\n"
    "- 'Random Color' assigns a random color per punctum.\n"
    "- 'Original Point Color' uses colors stored in the source file (if present).\n"
    "- 'Colormap Score' maps the 'score' attribute through the 'Score Color Map'."));

  connect(&m_colorMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DPunctaFilter::prepareColor);
  connect(&m_colorMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DPunctaFilter::adjustWidgets);

  connect(&m_useSameSizeForAllPuncta, &ZBoolParameter::valueChanged, this, &Z3DPunctaFilter::changePunctaSize);

  addParameter(m_colorMode);

  addParameter(m_singleColorForAllPuncta);
  m_singleColorForAllPuncta.setDescription(QStringLiteral(
    "Solid RGBA color used when 'Color Mode' is set to 'Single Color'."));
  addParameter(m_colorMapScore);
  m_colorMapScore.setDescription(QStringLiteral(
    "Color map used to visualize puncta 'score' values (min..max)."));
  addParameter(m_colorMapMeanIntensity);
  m_colorMapMeanIntensity.setDescription(QStringLiteral(
    "Color map used to visualize puncta 'mean intensity' values."));
  addParameter(m_colorMapMaxIntensity);
  m_colorMapMaxIntensity.setDescription(QStringLiteral(
    "Color map used to visualize puncta 'max intensity' values."));

  addParameter(m_useSameSizeForAllPuncta);
  m_useSameSizeForAllPuncta.setDescription(QStringLiteral(
    "Render all puncta with the same size, ignoring per-point radii when enabled."));

  addParameter(m_useDynamicMaterial);
  m_useDynamicMaterial.setDescription(QStringLiteral(
    "When enabled, derive shading properties from intensity (specular/shine)."));
  connect(&m_useDynamicMaterial, &ZBoolParameter::valueChanged, this, [this]() {
    m_sphereRenderer.setUseDynamicMaterial(m_useDynamicMaterial.get());
  });
  m_sphereRenderer.setUseDynamicMaterial(m_useDynamicMaterial.get());

  //  m_glowSphereRenderer.useDynamicMaterialPara().set(false);
  //  connect(&m_randomGlow, &ZBoolParameter::valueChanged, this, &Z3DPunctaFilter::adjustWidgets);
  //  addParameter(m_randomGlow);
  //  addParameter(m_textureGlowRenderer.glowModePara());
  //  addParameter(m_textureGlowRenderer.blurRadiusPara());
  //  addParameter(m_textureGlowRenderer.blurScalePara());
  //  addParameter(m_textureGlowRenderer.blurStrengthPara());
  //  addParameter(m_glowPercentage);

  m_selectPunctumEvent.listenTo("select punctum", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
  m_selectPunctumEvent.listenTo("select punctum", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonRelease);
  m_selectPunctumEvent.listenTo("select punctum", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonDblClick);
  m_selectPunctumEvent.listenTo("select punctum", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonDblClick);
  m_selectPunctumEvent.listenTo("append select punctum", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonPress);
  m_selectPunctumEvent.listenTo("append select punctum",
                                Qt::LeftButton,
                                Qt::ControlModifier,
                                QEvent::MouseButtonRelease);
  connect(&m_selectPunctumEvent, &ZEventListenerParameter::mouseEventTriggered, this, &Z3DPunctaFilter::selectPuncta);
  addEventListener(m_selectPunctumEvent);

  m_deleteSelectedPunctaEvent.listenTo("delete", Qt::Key_Delete, Qt::NoModifier, QEvent::KeyPress);
  m_deleteSelectedPunctaEvent.listenTo("backspace", Qt::Key_Backspace, Qt::NoModifier, QEvent::KeyPress);
  connect(&m_deleteSelectedPunctaEvent,
          &ZEventListenerParameter::keyEventTriggered,
          this,
          &Z3DPunctaFilter::deleteSelectedPuncta);
  addEventListener(m_deleteSelectedPunctaEvent);

  m_contextMenuEvent.listenToContextMenuEvent();
  connect(&m_contextMenuEvent,
          &ZEventListenerParameter::contextMenuEventTriggered,
          this,
          &Z3DPunctaFilter::contextMenuEvent);
  addEventListener(m_contextMenuEvent);

  adjustWidgets();
}

double Z3DPunctaFilter::process(Z3DEye)
{
  syncRendererState();

  if (m_dataIsInvalid) {
    prepareData();
  }
  //  if (m_randomGlow.get()) {
  //    m_pointAndRadiusGlow.clear();
  //    m_specularAndShininessGlow.clear();
  //    m_pointColorsGlow.clear();
  //    m_pointAndRadiusNormal.clear();
  //    m_specularAndShininessNormal.clear();
  //    m_pointColorsNormal.clear();

  //    for (size_t i=0; i<m_punctaList.size(); ++i) {
  //      if (ZRandomInstance.randReal<float>() < m_glowPercentage.get()) {
  //        m_pointAndRadiusGlow.push_back(m_pointAndRadius[i]);
  //        m_specularAndShininessGlow.push_back(m_specularAndShininess[i]);
  //        m_pointColorsGlow.push_back(m_pointColors[i]);
  //      } else {
  //        m_pointAndRadiusNormal.push_back(m_pointAndRadius[i]);
  //        m_specularAndShininessNormal.push_back(m_specularAndShininess[i]);
  //        m_pointColorsNormal.push_back(m_pointColors[i]);
  //      }
  //    }
  //    m_sphereRenderer.setData(&m_pointAndRadiusNormal, &m_specularAndShininessNormal);
  //    m_sphereRenderer.setDataColors(&m_pointColorsNormal);
  //    m_glowSphereRenderer.setData(&m_pointAndRadiusGlow, &m_specularAndShininessGlow);
  //    m_glowSphereRenderer.setDataColors(&m_pointColorsGlow);

  //    glEnable(GL_DEPTH_TEST);
  //    glEnable(GL_BLEND);
  //    glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);

  //    Z3DRenderOutputPort &currentOutport = (eye == Mono) ?
  //          m_monoEyeOutport : (eye == Left) ? m_leftEyeOutRenderTarget1 : m_rightEyeOutRenderTarget1;

  //    currentOutport.bindTarget();
  //    currentOutport.clearTarget();
  //    m_rendererBase.setViewport(currentOutport.size());
  //    m_rendererBase.render(eye, m_glowSphereRenderer);
  //    CHECK_GL_ERROR
  //    currentOutport.releaseTarget();

  //    Z3DRenderOutputPort &currentOutport2 = (eye == Mono) ?
  //          m_monoEyeOutport2 : (eye == Left) ? m_leftEyeOutport2 : m_rightEyeOutport2;
  //    currentOutport2.bindTarget();
  //    currentOutport2.clearTarget();
  //    m_rendererBase.setViewport(currentOutport2.size());
  //    m_textureGlowRenderer.setColorTexture(currentOutport.colorTexture());
  //    m_textureGlowRenderer.setDepthTexture(currentOutport.depthTexture());
  //    m_rendererBase.render(eye, m_textureGlowRenderer);
  //    CHECK_GL_ERROR
  //    currentOutport2.releaseTarget();

  //    glBlendFunc(GL_ONE,GL_ZERO);
  //    glDisable(GL_BLEND);
  //    glDisable(GL_DEPTH_TEST);
  //  }

  return 1.;
}

void Z3DPunctaFilter::setData(ZPunctaPack& puncta)
{
  CHECK(!m_punctaPack);
  m_punctaPack = &puncta;
  updateData();

  connect(m_punctaPack, &ZPunctaPack::selectionChanged, this, &Z3DPunctaFilter::invalidateResult);
  connect(this, &Z3DPunctaFilter::punctumSelected, m_punctaPack, &ZPunctaPack::onPunctumSelected);
  connect(m_punctaPack, &ZPunctaPack::punctaChanged, this, &Z3DPunctaFilter::updateData);
  connect(m_punctaPack, &ZPunctaPack::lockedStateChanged, this, &Z3DPunctaFilter::invalidateResult);
  connect(this, &Z3DPunctaFilter::showPunctaContextMenu, m_punctaPack, &ZPunctaPack::showPunctaContextMenu);
}

bool Z3DPunctaFilter::isReady(Z3DEye eye) const
{
  return Z3DGeometryFilter::isReady(eye) && m_visible.get() && m_punctaPack;
}

// namespace {

// bool compareParameterName(const ZParameter *p1, const ZParameter *p2)
//{
//   QString n1 = p1->getName().mid(7); // "Source "
//   QString n2 = p2->getName().mid(7);
//   n1.remove(n1.size()-6, 6); //" Color"
//   n2.remove(n2.size()-6, 6);
//   return n1.toInt() < n2.toInt();
// }

//}

std::shared_ptr<ZWidgetsGroup> Z3DPunctaFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Puncta", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_stayOnTop, 1);
    m_widgetsGroup->addChild(m_colorMode, 1);
    m_widgetsGroup->addChild(m_singleColorForAllPuncta, 1);
    m_widgetsGroup->addChild(m_colorMapScore, 1);
    m_widgetsGroup->addChild(m_colorMapMeanIntensity, 1);
    m_widgetsGroup->addChild(m_colorMapMaxIntensity, 1);
    m_widgetsGroup->addChild(m_useSameSizeForAllPuncta, 1);
    m_widgetsGroup->addChild(m_useDynamicMaterial, 7);

    auto& rendererParas = m_rendererParameters;
    m_widgetsGroup->addChild(rendererParas.coordTransform, 5);
    m_widgetsGroup->addChild(rendererParas.sizeScale, 2);
    m_widgetsGroup->addChild(rendererParas.opacity, 3);
    m_widgetsGroup->addChild(rendererParas.materialAmbient, 7);
    m_widgetsGroup->addChild(rendererParas.materialSpecular, 7);
    m_widgetsGroup->addChild(rendererParas.materialShininess, 7);

    //    m_widgetsGroup->addChild(&m_randomGlow, 5);
    //    m_widgetsGroup->addChild(&m_glowPercentage, 5);
    //    m_widgetsGroup->addChild(&m_textureGlowRenderer.glowModePara(), 5);
    //    m_widgetsGroup->addChild(&m_textureGlowRenderer.blurRadiusPara(), 5);
    //    m_widgetsGroup->addChild(&m_textureGlowRenderer.blurScalePara(), 5);
    //    m_widgetsGroup->addChild(&m_textureGlowRenderer.blurStrengthPara(), 5);

    m_widgetsGroup->addChild(m_xCut, 5);
    m_widgetsGroup->addChild(m_yCut, 5);
    m_widgetsGroup->addChild(m_zCut, 5);
    m_widgetsGroup->addChild(m_boundBoxMode, 5);
    m_widgetsGroup->addChild(m_boundBoxLineWidth, 5);
    m_widgetsGroup->addChild(m_boundBoxLineColor, 5);
    m_widgetsGroup->addChild(m_selectionLineWidth, 7);
    m_widgetsGroup->addChild(m_selectionLineColor, 7);
    m_widgetsGroup->addChild(m_manipulatorSize, 7);
    m_widgetsGroup->setBasicAdvancedCutoff(5);
  }
  return m_widgetsGroup;
}

void Z3DPunctaFilter::renderOpaque(Z3DEye eye)
{
  //  if (m_randomGlow.get()) {
  //    Z3DRenderOutputPort &currentOutport2 = (eye == Mono) ?
  //          m_monoEyeOutport2 : (eye == Left) ? m_leftEyeOutport2 : m_rightEyeOutport2;
  //    m_textureCopyRenderer.setColorTexture(currentOutport2.colorTexture());
  //    m_textureCopyRenderer.setDepthTexture(currentOutport2.depthTexture());
  //    m_rendererBase.render(eye, m_textureCopyRenderer);
  //    renderBoundBox(eye);
  //  }
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    m_rendererBase.renderVulkan(eye, m_sphereRenderer);
  } else {
    m_rendererBase.render(eye, m_sphereRenderer);
  }
  renderBoundBox(eye);
  renderEditingSelectionBox(eye);
}

void Z3DPunctaFilter::renderTransparent(Z3DEye eye)
{
  //  if (m_randomGlow.get()) {
  //    Z3DRenderOutputPort &currentOutport = (eye == Mono) ?
  //          m_monoEyeOutport : (eye == Left) ? m_leftEyeOutRenderTarget1 : m_rightEyeOutRenderTarget1;
  //    Z3DRenderOutputPort &currentOutport2 = (eye == Mono) ?
  //          m_monoEyeOutport2 : (eye == Left) ? m_leftEyeOutport2 : m_rightEyeOutport2;
  //    m_textureCopyRenderer.setColorTexture(currentOutport2.colorTexture());
  //    m_textureCopyRenderer.setDepthTexture(currentOutport.depthTexture());
  //    m_rendererBase.render(eye, m_textureCopyRenderer);
  //    renderBoundBox(eye);
  //  }
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    m_rendererBase.renderVulkan(eye, m_sphereRenderer);
  } else {
    m_rendererBase.render(eye, m_sphereRenderer);
  }
  renderBoundBox(eye);
  renderEditingSelectionBox(eye);
}

void Z3DPunctaFilter::renderPicking(Z3DEye eye)
{
  if (!m_pickingObjectsRegistered) {
    registerPickingObjects();
  }
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    m_rendererBase.renderPickingVulkan(eye, m_sphereRenderer);
  } else {
    m_rendererBase.renderPicking(eye, m_sphereRenderer);
  }
}

void Z3DPunctaFilter::registerPickingObjects()
{
  if (!m_pickingObjectsRegistered) {
    for (auto punctum : m_punctaPack->punctaPts()) {
      pickingManager().registerObject(punctum);
    }
    m_pointPickingColors.clear();
    for (auto punctum : m_punctaPack->punctaPts()) {
      glm::col4 pickingColor = pickingManager().colorOfObject(punctum);
      glm::vec4 fPickingColor(pickingColor[0] / 255.f,
                              pickingColor[1] / 255.f,
                              pickingColor[2] / 255.f,
                              pickingColor[3] / 255.f);
      m_pointPickingColors.push_back(fPickingColor);
    }
    m_sphereRenderer.setDataPickingColors(&m_pointPickingColors);
  }

  m_pickingObjectsRegistered = true;
}

void Z3DPunctaFilter::deregisterPickingObjects()
{
  if (m_pickingObjectsRegistered) {
    for (auto punctum : m_punctaPack->punctaPts()) {
      pickingManager().deregisterObject(punctum);
    }
  }

  m_pickingObjectsRegistered = false;
}

void Z3DPunctaFilter::prepareData()
{
  if (!m_dataIsInvalid) {
    return;
  }

  deregisterPickingObjects();

  // convert puncta to format that glsl can use
  m_specularAndShininess.clear();
  m_pointAndRadius.clear();
  for (auto punctum : m_punctaPack->punctaPts()) {
    if (m_useSameSizeForAllPuncta.get()) {
      m_pointAndRadius.emplace_back(punctum->x(), punctum->y(), punctum->z(), 2.f);
    } else {
      m_pointAndRadius.emplace_back(punctum->x(), punctum->y(), punctum->z(), punctum->radius());
    }
    m_specularAndShininess.emplace_back(punctum->maxIntensity() / 255.f,
                                        punctum->maxIntensity() / 255.f,
                                        punctum->maxIntensity() / 255.f,
                                        punctum->maxIntensity() / 2.f);
  }

  initializeCutRange();
  initializeRotationCenter();

  m_sphereRenderer.setData(&m_pointAndRadius, &m_specularAndShininess);
  prepareColor();
  adjustWidgets();
  m_dataIsInvalid = false;
}

void Z3DPunctaFilter::punctumBound(const ZPunctum& p, ZBBox<glm::dvec3>& result) const
{
  double radius = p.radius() * m_rendererParameters.sizeScale.get();
  if (m_useSameSizeForAllPuncta.get()) {
    radius = 2.0 * m_rendererParameters.sizeScale.get();
  }
  glm::dvec3 cent = glm::dvec3(glm::applyMatrix(coordTransform(), glm::vec3(p.x(), p.y(), p.z())));
  result.setMinCorner(cent - radius);
  result.setMaxCorner(cent + radius);
}

void Z3DPunctaFilter::updateData()
{
  double minMeanInten = std::numeric_limits<double>::max();
  double maxMeanInten = std::numeric_limits<double>::lowest();
  double minMaxInten = std::numeric_limits<double>::max();
  double maxMaxInten = std::numeric_limits<double>::lowest();
  for (const auto& p : m_punctaPack->punctaPts()) {
    minMeanInten = std::min(minMeanInten, p->meanIntensity());
    maxMeanInten = std::max(maxMeanInten, p->meanIntensity());
    minMaxInten = std::min(minMaxInten, p->maxIntensity());
    maxMaxInten = std::max(maxMaxInten, p->maxIntensity());
  }
  // todo: set correct range for colormap

  m_dataIsInvalid = true;
  invalidateResult();

  updateBoundBox();
}

void Z3DPunctaFilter::notTransformedPunctumBound(const ZPunctum& p, ZBBox<glm::dvec3>& result) const
{
  double radius = p.radius() * m_rendererParameters.sizeScale.get();
  if (m_useSameSizeForAllPuncta.get()) {
    radius = 2.0 * m_rendererParameters.sizeScale.get();
  }
  glm::dvec3 cent(p.x(), p.y(), p.z());
  result.setMinCorner(cent - radius);
  result.setMaxCorner(cent + radius);
}

void Z3DPunctaFilter::updateNotTransformedBoundBoxImpl()
{
  m_notTransformedBoundBox.reset();
  ZBBox<glm::dvec3> boundBox;
  for (const auto& p : m_punctaPack->punctaPts()) {
    notTransformedPunctumBound(*p, boundBox);
    m_notTransformedBoundBox.expand(boundBox);
  }
}

void Z3DPunctaFilter::addSelectionLines()
{
  ZBBox<glm::dvec3> boundBox;
  for (const auto& p : m_punctaPack->punctaPts()) {
    punctumBound(*p, boundBox);
    appendBoundboxLines(boundBox, m_selectionLines);
  }
}

void Z3DPunctaFilter::addEditingSelectionLines()
{
  if (m_punctaPack->isLocked()) {
    return;
  }
  ZBBox<glm::dvec3> boundBox;
  for (auto p : m_punctaPack->selectedPuncta()) {
    punctumBound(*p, boundBox);
    appendBoundboxLines(boundBox, m_editingSelectionLines);
  }
}

void Z3DPunctaFilter::prepareColor()
{
  m_pointColors.clear();

  if (m_colorMode.isSelected("Original Point Color")) {
    for (auto punctum : m_punctaPack->punctaPts()) {
      glm::vec4 color(punctum->color().redF(),
                      punctum->color().greenF(),
                      punctum->color().blueF(),
                      punctum->color().alphaF());
      m_pointColors.push_back(color);
    }
  } else if (m_colorMode.isSelected("Random Color")) {
    for (size_t i = 0; i < m_punctaPack->punctaPts().size(); ++i) {
      glm::vec4 color(ZRandom::instance().randReal<float>(),
                      ZRandom::instance().randReal<float>(),
                      ZRandom::instance().randReal<float>(),
                      1.0f);
      m_pointColors.push_back(color);
    }
  } else if (m_colorMode.isSelected("Single Color")) {
    for (size_t i = 0; i < m_punctaPack->punctaPts().size(); ++i) {
      m_pointColors.push_back(m_singleColorForAllPuncta.get());
    }
  } else if (m_colorMode.isSelected("Colormap Score")) {
    for (auto punctum : m_punctaPack->punctaPts()) {
      m_pointColors.push_back(m_colorMapScore.get().mappedFColor(punctum->score()));
    }
  } else if (m_colorMode.isSelected("Colormap Mean Intensity")) {
    for (auto punctum : m_punctaPack->punctaPts()) {
      m_pointColors.push_back(m_colorMapMeanIntensity.get().mappedFColor(punctum->meanIntensity()));
    }
  } else if (m_colorMode.isSelected("Colormap Max Intensity")) {
    for (auto punctum : m_punctaPack->punctaPts()) {
      m_pointColors.push_back(m_colorMapMaxIntensity.get().mappedFColor(punctum->maxIntensity()));
    }
  }

  m_sphereRenderer.setDataColors(&m_pointColors);
}

void Z3DPunctaFilter::adjustWidgets()
{
  m_singleColorForAllPuncta.setVisible(m_colorMode.isSelected("Single Color"));
  m_colorMapScore.setVisible(m_colorMode.isSelected("Colormap Score"));
  m_colorMapMeanIntensity.setVisible(m_colorMode.isSelected("Colormap Mean Intensity"));
  m_colorMapMaxIntensity.setVisible(m_colorMode.isSelected("Colormap Max Intensity"));

  //  m_glowPercentage.setVisible(m_randomGlow.get());
  //  m_textureGlowRenderer.glowModePara().setVisible(m_randomGlow.get());
  //  m_textureGlowRenderer.blurRadiusPara().setVisible(m_randomGlow.get());
  //  m_textureGlowRenderer.blurScalePara().setVisible(m_randomGlow.get());
  //  m_textureGlowRenderer.blurStrengthPara().setVisible(m_randomGlow.get());
}

void Z3DPunctaFilter::selectPuncta(QMouseEvent* e, int /*w*/, int /*h*/)
{
  if (!m_punctaPack || m_punctaPack->puncta().data.empty()) {
    return;
  }

  e->ignore();
  if (e->type() == QEvent::MouseButtonDblClick) {
    const void* obj = pickingManager().objectAtWidgetPos(glm::ivec2(e->position().x(), e->position().y()));
    bool appending = (e->modifiers() == Qt::ControlModifier);
    if (!obj && !appending && m_isSelected) {
      Q_EMIT objDeselected();
      return;
    }
    bool hit = contains(m_punctaPack->punctaPts(), static_cast<const ZPunctum*>(obj));
    if (hit) {
      Q_EMIT objSelected(appending);
      e->accept();
    }
    return;
  }

  if (m_punctaPack->isLocked()) {
    return;
  }

  e->ignore();
  // Mouse button pressend
  // can not accept the event in button press, because we don't know if it is a selection or interaction
  if (e->type() == QEvent::MouseButtonPress) {
    m_startCoord.x = e->position().x();
    m_startCoord.y = e->position().y();
    const void* obj = pickingManager().objectAtWidgetPos(glm::ivec2(e->position().x(), e->position().y()));
    if (!obj) {
      return;
    }

    // Check if any point was selected...
    for (auto p : m_punctaPack->punctaPts()) {
      if (p == obj) {
        m_pressedPunctum = p;
        break;
      }
    }
    return;
  }

  if (e->type() == QEvent::MouseButtonRelease) {
    if (std::abs(e->position().x() - m_startCoord.x) < 2 && std::abs(m_startCoord.y - e->position().y()) < 2) {
      Q_EMIT punctumSelected(m_pressedPunctum, e->modifiers() == Qt::ControlModifier);
      if (m_pressedPunctum) {
        e->accept();
      }
    }
    m_pressedPunctum = nullptr;
  }
}

void Z3DPunctaFilter::contextMenuEvent(QContextMenuEvent* e, int, int)
{
  if (m_punctaPack->isLocked()) {
    return;
  }

  if (isVisible() && !isSelected() && m_punctaPack && !m_punctaPack->selectedPuncta().empty()) {
    const void* obj = pickingManager().objectAtWidgetPos(glm::ivec2(e->x(), e->y()));
    if (!obj) {
      return;
    }

    bool hasSelectedPunctumUnderMouse = false;
    for (auto p : m_punctaPack->selectedPuncta()) {
      if (p == obj) {
        hasSelectedPunctumUnderMouse = true;
        break;
      }
    }
    if (!hasSelectedPunctumUnderMouse) {
      return;
    }

    Q_EMIT showPunctaContextMenu(e->globalPos());
  }
}

void Z3DPunctaFilter::changePunctaSize()
{
  for (size_t i = 0; i < m_pointAndRadius.size(); ++i) {
    if (m_useSameSizeForAllPuncta.get()) {
      m_pointAndRadius[i].w = 2.f;
    } else {
      m_pointAndRadius[i].w = m_punctaPack->punctaPts()[i]->radius();
    }
  }
  m_sphereRenderer.setData(&m_pointAndRadius, &m_specularAndShininess);
  updateBoundBox();
}

void Z3DPunctaFilter::deleteSelectedPuncta()
{
  if (m_punctaPack->isLocked()) {
    return;
  }
  m_punctaPack->deleteSelectedPuncta();
}

} // namespace nim
