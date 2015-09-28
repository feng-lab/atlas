#include "z3dpunctafilter.h"

#include <iostream>

#include "zpuncta.h"
#include "zrandom.h"

namespace nim {

Z3DPunctaFilter::Z3DPunctaFilter(Z3DGlobalParameters& globalParas, QObject *parent)
  : Z3DGeometryFilter(globalParas, parent)
  , m_monoEyeOutport("Image")
  , m_leftEyeOutport("LeftEyeImage")
  , m_rightEyeOutport("RightEyeImage")
  , m_monoEyeOutport2("Image2")
  , m_leftEyeOutport2("LeftEyeImage2")
  , m_rightEyeOutport2("RightEyeImage2")
  , m_sphereRenderer(m_rendererBase)
  , m_visible("Visible", true)
  , m_colorMode("Color Mode")
  , m_singleColorForAllPuncta("Puncta Color", glm::vec4(ZRandomInstance.randReal<float>(),
                                                       ZRandomInstance.randReal<float>(),
                                                       ZRandomInstance.randReal<float>(),
                                                       1.f))
  , m_colorMapScore("Score Color Map", -1., 1., QColor(255,255,0), QColor(0,0,255))
  , m_colorMapMeanIntensity("Mean Intensity Color Map", 0., 1., QColor(255,0,0), QColor(0,0,0))
  , m_colorMapMaxIntensity("Max Intensity Color Map", 0., 1., QColor(255,0,0), QColor(0,0,0))
  , m_useSameSizeForAllPuncta("Use Same Size", false)
  //  , m_glowSphereRenderer(m_rendererBase)
  //  , m_textureGlowRenderer(m_rendererBase)
  //  , m_randomGlow("Random Glow", false)
  //  , m_glowPercentage("Glow Percentage", 0.2f, 0.f, 1.f)
  //  , m_textureCopyRenderer(m_rendererBase)
  , m_selectPunctumEvent("Select Puncta", false)
  , m_pressedPunctum(nullptr)
  , m_selectedPuncta(nullptr)
  , m_dataIsInvalid(false)
  , m_origPuncta(nullptr)
{
  addPrivateRenderPort(m_monoEyeOutport);
  addPrivateRenderPort(m_leftEyeOutport);
  addPrivateRenderPort(m_rightEyeOutport);
  addPrivateRenderPort(m_monoEyeOutport2);
  addPrivateRenderPort(m_leftEyeOutport2);
  addPrivateRenderPort(m_rightEyeOutport2);

  //m_textureCopyRenderer.setDiscardTransparent(true);

  m_singleColorForAllPuncta.setStyle("COLOR");
  connect(&m_singleColorForAllPuncta, SIGNAL(valueChanged()), this, SLOT(prepareColor()));
  connect(&m_colorMapScore, SIGNAL(valueChanged()), this, SLOT(prepareColor()));
  connect(&m_colorMapMeanIntensity, SIGNAL(valueChanged()), this, SLOT(prepareColor()));
  connect(&m_colorMapMaxIntensity, SIGNAL(valueChanged()), this, SLOT(prepareColor()));

  // Color Mode
  m_colorMode.addOptions("Single Color", "Random Color", "Original Point Color", "Colormap Score");
  m_colorMode.select("Original Point Color");

  connect(&m_colorMode, SIGNAL(valueChanged()), this, SLOT(prepareColor()));
  connect(&m_colorMode, SIGNAL(valueChanged()), this, SLOT(adjustWidgets()));

  connect(&m_useSameSizeForAllPuncta, SIGNAL(valueChanged()), this, SLOT(changePunctaSize()));

  addParameter(m_visible);
  addParameter(m_colorMode);

  addParameter(m_singleColorForAllPuncta);
  addParameter(m_colorMapScore);
  addParameter(m_colorMapMeanIntensity);
  addParameter(m_colorMapMaxIntensity);

  addParameter(m_useSameSizeForAllPuncta);

  addParameter(m_sphereRenderer.useDynamicMaterialPara());

  //  m_glowSphereRenderer.useDynamicMaterialPara().set(false);
  //  connect(&m_randomGlow, SIGNAL(valueChanged()), this, SLOT(adjustWidgets()));
  //  addParameter(m_randomGlow);
  //  addParameter(m_textureGlowRenderer.glowModePara());
  //  addParameter(m_textureGlowRenderer.blurRadiusPara());
  //  addParameter(m_textureGlowRenderer.blurScalePara());
  //  addParameter(m_textureGlowRenderer.blurStrengthPara());
  //  addParameter(m_glowPercentage);

  m_selectPunctumEvent.listenTo("select punctum", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
  m_selectPunctumEvent.listenTo("select punctum", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonRelease);
  m_selectPunctumEvent.listenTo("select punctum", Qt::LeftButton,
                                Qt::NoModifier, QEvent::MouseButtonDblClick);
  m_selectPunctumEvent.listenTo("select punctum", Qt::LeftButton,
                                Qt::ControlModifier, QEvent::MouseButtonDblClick);
  m_selectPunctumEvent.listenTo("append select punctum", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonPress);
  m_selectPunctumEvent.listenTo("append select punctum", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonRelease);
  connect(&m_selectPunctumEvent, SIGNAL(mouseEventTriggered(QMouseEvent*,int,int)), this, SLOT(selectPuncta(QMouseEvent*,int,int)));
  addEventListener(m_selectPunctumEvent);

  adjustWidgets();

  connect(&m_visible, SIGNAL(valueChanged(bool)), this, SIGNAL(objVisibleChanged(bool)));
}

Z3DPunctaFilter::~Z3DPunctaFilter()
{
}

void Z3DPunctaFilter::process(Z3DEye eye)
{
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

  //    for (size_t i=0; i<m_punctaList.size(); i++) {
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

  //    Z3DRenderOutputPort &currentOutport = (eye == Z3DEye::Mono) ?
  //          m_monoEyeOutport : (eye == Z3DEye::Left) ? m_leftEyeOutport : m_rightEyeOutport;

  //    currentOutport.bindTarget();
  //    currentOutport.clearTarget();
  //    m_rendererBase.setViewport(currentOutport.size());
  //    m_rendererBase.render(eye, m_glowSphereRenderer);
  //    CHECK_GL_ERROR;
  //    currentOutport.releaseTarget();

  //    Z3DRenderOutputPort &currentOutport2 = (eye == Z3DEye::Mono) ?
  //          m_monoEyeOutport2 : (eye == Z3DEye::Left) ? m_leftEyeOutport2 : m_rightEyeOutport2;
  //    currentOutport2.bindTarget();
  //    currentOutport2.clearTarget();
  //    m_rendererBase.setViewport(currentOutport2.size());
  //    m_textureGlowRenderer.setColorTexture(currentOutport.colorTexture());
  //    m_textureGlowRenderer.setDepthTexture(currentOutport.depthTexture());
  //    m_rendererBase.render(eye, m_textureGlowRenderer);
  //    CHECK_GL_ERROR;
  //    currentOutport2.releaseTarget();

  //    glBlendFunc(GL_ONE,GL_ZERO);
  //    glDisable(GL_BLEND);
  //    glDisable(GL_DEPTH_TEST);
  //  }
}

void Z3DPunctaFilter::setData(ZPuncta &puncta)
{
  m_origPuncta = &puncta;
  updateData();
}

bool Z3DPunctaFilter::isReady(Z3DEye eye) const
{
  return Z3DGeometryFilter::isReady(eye) && m_visible.get() && m_origPuncta;
}

//namespace {

//bool compareParameterName(const ZParameter *p1, const ZParameter *p2)
//{
//  QString n1 = p1->getName().mid(7); // "Source "
//  QString n2 = p2->getName().mid(7);
//  n1.remove(n1.size()-6, 6); //" Color"
//  n2.remove(n2.size()-6, 6);
//  return n1.toInt() < n2.toInt();
//}

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
    m_widgetsGroup->addChild(m_sphereRenderer.useDynamicMaterialPara(), 7);

    const std::vector<ZParameter*>& paras = m_rendererBase.parameters();
    for (size_t i=0; i<paras.size(); i++) {
      ZParameter *para = paras[i];
      if (para->name() == "Coord Transform")
        m_widgetsGroup->addChild(*para, 2);
      else if (para->name() == "Size Scale")
        m_widgetsGroup->addChild(*para, 3);
      else if (para->name() == "Rendering Method")
        m_widgetsGroup->addChild(*para, 4);
      else if (para->name() == "Opacity")
        m_widgetsGroup->addChild(*para, 5);
      else
        m_widgetsGroup->addChild(*para, 7);
    }

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
  //    Z3DRenderOutputPort &currentOutport2 = (eye == Z3DEye::Mono) ?
  //          m_monoEyeOutport2 : (eye == Z3DEye::Left) ? m_leftEyeOutport2 : m_rightEyeOutport2;
  //    m_textureCopyRenderer.setColorTexture(currentOutport2.colorTexture());
  //    m_textureCopyRenderer.setDepthTexture(currentOutport2.depthTexture());
  //    m_rendererBase.render(eye, m_textureCopyRenderer);
  //    renderBoundBox(eye);
  //  }
  m_rendererBase.render(eye, m_sphereRenderer);
  renderBoundBox(eye);
}

void Z3DPunctaFilter::renderTransparent(Z3DEye eye)
{
  //  if (m_randomGlow.get()) {
  //    Z3DRenderOutputPort &currentOutport = (eye == Z3DEye::Mono) ?
  //          m_monoEyeOutport : (eye == Z3DEye::Left) ? m_leftEyeOutport : m_rightEyeOutport;
  //    Z3DRenderOutputPort &currentOutport2 = (eye == Z3DEye::Mono) ?
  //          m_monoEyeOutport2 : (eye == Z3DEye::Left) ? m_leftEyeOutport2 : m_rightEyeOutport2;
  //    m_textureCopyRenderer.setColorTexture(currentOutport2.colorTexture());
  //    m_textureCopyRenderer.setDepthTexture(currentOutport.depthTexture());
  //    m_rendererBase.render(eye, m_textureCopyRenderer);
  //    renderBoundBox(eye);
  //  }
  m_rendererBase.render(eye, m_sphereRenderer);
  renderBoundBox(eye);
}

void Z3DPunctaFilter::renderPicking(Z3DEye eye)
{
  if (!m_pickingObjectsRegistered)
    registerPickingObjects();
  m_rendererBase.renderPicking(eye, m_sphereRenderer);
}

void Z3DPunctaFilter::registerPickingObjects()
{
  if (!m_pickingObjectsRegistered) {
    for (size_t i=0; i<m_punctaList.size(); i++) {
      pickingManager().registerObject(m_punctaList[i]);
    }
    m_registeredPunctaList = m_punctaList;
    m_pointPickingColors.clear();
    for (size_t i=0; i<m_punctaList.size(); i++) {
      glm::col4 pickingColor = pickingManager().colorOfObject(m_punctaList[i]);
      glm::vec4 fPickingColor(pickingColor[0]/255.f, pickingColor[1]/255.f, pickingColor[2]/255.f, pickingColor[3]/255.f);
      m_pointPickingColors.push_back(fPickingColor);
    }
    m_sphereRenderer.setDataPickingColors(&m_pointPickingColors);
  }

  m_pickingObjectsRegistered = true;
}

void Z3DPunctaFilter::deregisterPickingObjects()
{
  if (m_pickingObjectsRegistered) {
    for (size_t i=0; i<m_registeredPunctaList.size(); i++) {
      pickingManager().deregisterObject(m_registeredPunctaList[i]);
    }
    m_registeredPunctaList.clear();
  }

  m_pickingObjectsRegistered = false;
}

void Z3DPunctaFilter::prepareData()
{
  if (!m_dataIsInvalid)
    return;

  deregisterPickingObjects();

  // convert puncta to format that glsl can use
  m_specularAndShininess.clear();
  m_pointAndRadius.clear();
  for (size_t i=0; i<m_punctaList.size(); i++) {
    if (m_useSameSizeForAllPuncta.get())
      m_pointAndRadius.emplace_back(m_punctaList[i]->x(), m_punctaList[i]->y(), m_punctaList[i]->z(), 2.f);
    else
      m_pointAndRadius.emplace_back(m_punctaList[i]->x(), m_punctaList[i]->y(), m_punctaList[i]->z(), m_punctaList[i]->radius());
    m_specularAndShininess.emplace_back(m_punctaList[i]->maxIntensity()/255.f,
                                        m_punctaList[i]->maxIntensity()/255.f,
                                        m_punctaList[i]->maxIntensity()/255.f,
                                        m_punctaList[i]->maxIntensity()/2.f);
  }

  initializeCutRange();
  initializeRotationCenter();

  m_sphereRenderer.setData(&m_pointAndRadius, &m_specularAndShininess);
  prepareColor();
  adjustWidgets();
  m_dataIsInvalid = false;
}

void Z3DPunctaFilter::punctumBound(const ZPunctum &p, std::vector<double> &result) const
{
  double radius = p.radius() * m_rendererBase.sizeScale();
  if (m_useSameSizeForAllPuncta.get())
    radius = 2.0 * m_rendererBase.sizeScale();
  glm::vec3 cent = glm::applyMatrix(coordTransform(), glm::vec3(p.x(), p.y(), p.z()));
  result[0] = cent.x - radius;
  result[1] = cent.x + radius;
  result[2] = cent.y - radius;
  result[3] = cent.y + radius;
  result[4] = cent.z - radius;
  result[5] = cent.z + radius;
}

void Z3DPunctaFilter::updateData()
{
  double minMeanInten = std::numeric_limits<double>::max();
  double maxMeanInten = -std::numeric_limits<double>::max();
  double minMaxInten = std::numeric_limits<double>::max();
  double maxMaxInten = -std::numeric_limits<double>::max();
  for (ZPuncta::const_iterator it=m_origPuncta->begin(); it != m_origPuncta->end(); ++it) {
    minMeanInten = std::min(minMeanInten, it->meanIntensity());
    maxMeanInten = std::max(maxMeanInten, it->meanIntensity());
    minMaxInten = std::min(minMaxInten, it->maxIntensity());
    maxMaxInten = std::max(maxMaxInten, it->maxIntensity());
  }
  //todo: set correct range for colormap

  getVisibleData();
  m_dataIsInvalid = true;
  invalidateResult();

  updateBoundBox();
}

void Z3DPunctaFilter::notTransformedPunctumBound(const ZPunctum &p, std::vector<double> &result) const
{
  double radius = p.radius() * m_rendererBase.sizeScale();
  if (m_useSameSizeForAllPuncta.get())
    radius = 2.0 * m_rendererBase.sizeScale();
  result[0] = p.x() - radius;
  result[1] = p.x() + radius;
  result[2] = p.y() - radius;
  result[3] = p.y() + radius;
  result[4] = p.z() - radius;
  result[5] = p.z() + radius;
}

//void Z3DPunctaFilter::updateAxisAlignedBoundBoxImpl()
//{
//  m_axisAlignedBoundBox[0] = m_axisAlignedBoundBox[2] = m_axisAlignedBoundBox[4] = std::numeric_limits<double>::max();
//  m_axisAlignedBoundBox[1] = m_axisAlignedBoundBox[3] = m_axisAlignedBoundBox[5] = -std::numeric_limits<double>::max();
//  std::vector<double> boundBox(6);
//  for (size_t i=0; i<m_origPunctaList.size(); ++i) {
//    getPunctumBound(m_origPunctaList[i], boundBox);
//    m_axisAlignedBoundBox[0] = std::min(boundBox[0], m_axisAlignedBoundBox[0]);
//    m_axisAlignedBoundBox[1] = std::max(boundBox[1], m_axisAlignedBoundBox[1]);
//    m_axisAlignedBoundBox[2] = std::min(boundBox[2], m_axisAlignedBoundBox[2]);
//    m_axisAlignedBoundBox[3] = std::max(boundBox[3], m_axisAlignedBoundBox[3]);
//    m_axisAlignedBoundBox[4] = std::min(boundBox[4], m_axisAlignedBoundBox[4]);
//    m_axisAlignedBoundBox[5] = std::max(boundBox[5], m_axisAlignedBoundBox[5]);
//  }
//}

void Z3DPunctaFilter::updateNotTransformedBoundBoxImpl()
{
  m_notTransformedBoundBox[0] = m_notTransformedBoundBox[2] = m_notTransformedBoundBox[4] = std::numeric_limits<double>::max();
  m_notTransformedBoundBox[1] = m_notTransformedBoundBox[3] = m_notTransformedBoundBox[5] = -std::numeric_limits<double>::max();
  std::vector<double> boundBox(6);
  for (ZPuncta::const_iterator it=m_origPuncta->begin(); it != m_origPuncta->end(); ++it) {
    notTransformedPunctumBound(*it, boundBox);
    m_notTransformedBoundBox[0] = std::min(boundBox[0], m_notTransformedBoundBox[0]);
    m_notTransformedBoundBox[1] = std::max(boundBox[1], m_notTransformedBoundBox[1]);
    m_notTransformedBoundBox[2] = std::min(boundBox[2], m_notTransformedBoundBox[2]);
    m_notTransformedBoundBox[3] = std::max(boundBox[3], m_notTransformedBoundBox[3]);
    m_notTransformedBoundBox[4] = std::min(boundBox[4], m_notTransformedBoundBox[4]);
    m_notTransformedBoundBox[5] = std::max(boundBox[5], m_notTransformedBoundBox[5]);
  }
}

void Z3DPunctaFilter::addSelectionLines()
{
  std::vector<double> boundBox(6);
  for (ZPuncta::const_iterator it=m_origPuncta->begin(); it != m_origPuncta->end(); ++it) {
    punctumBound(*it, boundBox);
    appendBoundboxLines(boundBox, m_selectionLines);
  }
}

void Z3DPunctaFilter::prepareColor()
{
  m_pointColors.clear();

  if (m_colorMode.isSelected("Original Point Color")) {
    for (size_t i=0; i<m_punctaList.size(); i++) {
      glm::vec4 color(m_punctaList[i]->color().redF(), m_punctaList[i]->color().greenF(), m_punctaList[i]->color().blueF(), m_punctaList[i]->color().alphaF());
      m_pointColors.push_back(color);
    }
  } else if (m_colorMode.isSelected("Random Color")) {
    for (size_t i=0; i<m_punctaList.size(); i++) {
      glm::vec4 color(ZRandomInstance.randReal<float>(), ZRandomInstance.randReal<float>(), ZRandomInstance.randReal<float>(), 1.0f);
      m_pointColors.push_back(color);
    }
  } else if (m_colorMode.isSelected("Single Color")) {
    for (size_t i=0; i<m_punctaList.size(); i++) {
      m_pointColors.push_back(m_singleColorForAllPuncta.get());
    }
  } else if (m_colorMode.isSelected("Colormap Score")) {
    for (size_t i=0; i<m_punctaList.size(); i++) {
      m_pointColors.push_back(m_colorMapScore.get().mappedFColor(m_punctaList[i]->score()));
    }
  } else if (m_colorMode.isSelected("Colormap Mean Intensity")) {
    for (size_t i=0; i<m_punctaList.size(); i++) {
      m_pointColors.push_back(m_colorMapMeanIntensity.get().mappedFColor(m_punctaList[i]->meanIntensity()));
    }
  } else if (m_colorMode.isSelected("Colormap Max Intensity")) {
    for (size_t i=0; i<m_punctaList.size(); i++) {
      m_pointColors.push_back(m_colorMapMaxIntensity.get().mappedFColor(m_punctaList[i]->maxIntensity()));
    }
  }

  m_sphereRenderer.setDataColors(&m_pointColors);
}

void Z3DPunctaFilter::adjustWidgets()
{
  if (m_colorMode.isSelected("Single Color"))
    m_singleColorForAllPuncta.setVisible(true);
  else
    m_singleColorForAllPuncta.setVisible(false);
  m_colorMapScore.setVisible(m_colorMode.isSelected("Colormap Score"));
  m_colorMapMeanIntensity.setVisible(m_colorMode.isSelected("Colormap Mean Intensity"));
  m_colorMapMaxIntensity.setVisible(m_colorMode.isSelected("Colormap Max Intensity"));

  //  m_glowPercentage.setVisible(m_randomGlow.get());
  //  m_textureGlowRenderer.glowModePara().setVisible(m_randomGlow.get());
  //  m_textureGlowRenderer.blurRadiusPara().setVisible(m_randomGlow.get());
  //  m_textureGlowRenderer.blurScalePara().setVisible(m_randomGlow.get());
  //  m_textureGlowRenderer.blurStrengthPara().setVisible(m_randomGlow.get());
}

void Z3DPunctaFilter::selectPuncta(QMouseEvent *e, int, int h)
{
  if (m_punctaList.empty())
    return;

  e->ignore();
  if (e->type() == QEvent::MouseButtonDblClick) {
    const void* obj = pickingManager().objectAtWidgetPos(
          glm::ivec2(e->x(), e->y()));
    bool appending = (e->modifiers() == Qt::ControlModifier);
    if (!obj && !appending && m_isSelected) {
      emit objDeselected();
      return;
    }
    bool hit = std::find(m_punctaList.begin(), m_punctaList.end(), (ZPunctum*)obj) != m_punctaList.end();
    if (hit) {
      emit objSelected(appending);
      e->accept();
    }
    return;
  }

  e->ignore();
  // Mouse button pressend
  // can not accept the event in button press, because we don't know if it is a selection or interaction
  if (e->type() == QEvent::MouseButtonPress) {
    m_startCoord.x = e->x();
    m_startCoord.y = e->y();
    const void* obj = pickingManager().objectAtWidgetPos(glm::ivec2(e->x(), e->y()));
    if (!obj) {
      return;
    }

    // Check if any point was selected...
    for (std::vector<ZPunctum*>::iterator it=m_punctaList.begin(); it!=m_punctaList.end(); ++it)
      if (*it == obj) {
        m_pressedPunctum = *it;
        break;
      }
    return;
  }

  if (e->type() == QEvent::MouseButtonRelease) {
    if (std::abs(e->x() - m_startCoord.x) < 2 && std::abs(m_startCoord.y - e->y()) < 2) {
      if (e->modifiers() == Qt::ControlModifier)
        emit punctumSelected(m_pressedPunctum, true);
      else
        emit punctumSelected(m_pressedPunctum, false);
      if (m_pressedPunctum)
        e->accept();
    }
    m_pressedPunctum = nullptr;
  }
}

void Z3DPunctaFilter::getVisibleData()
{
  m_punctaList.clear();
  for (ZPuncta::iterator it=m_origPuncta->begin(); it != m_origPuncta->end(); ++it) {
    //if (m_origPuncta[i]->isVisible())
      m_punctaList.push_back(&(*it));
  }
}

void Z3DPunctaFilter::updatePunctumVisibleState()
{
  getVisibleData();
  m_dataIsInvalid = true;
  invalidateResult();
}

void Z3DPunctaFilter::changePunctaSize()
{
  for (size_t i=0; i<m_pointAndRadius.size(); i++) {
    if (m_useSameSizeForAllPuncta.get())
      m_pointAndRadius.at(i).w = 2.f;
    else
      m_pointAndRadius.at(i).w = m_punctaList[i]->radius();
  }
  m_sphereRenderer.setData(&m_pointAndRadius, &m_specularAndShininess);
  updateBoundBox();
}

} // namespace nim

