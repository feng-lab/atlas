#include "z3dboundedfilter.h"

#include <Wm5DistLine3Ray3.h>
#ifndef _QT4_
#include <QtMath>  // for M_PI
#else
#include <cmath>
#endif

namespace nim {

Z3DBoundedFilter::Z3DBoundedFilter(Z3DGlobalParameters &globalPara, QObject *parent)
  : Z3DProcessor(parent)
  , m_rendererBase(globalPara)
  , m_baseBoundBoxRenderer(m_rendererBase)
  , m_selectionBoundBoxRenderer(m_rendererBase)
  , m_selectionCornerRenderer(m_rendererBase)
  , m_handleCenterRenderer(m_rendererBase)
  , m_handleArrowRenderer(m_rendererBase)
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
  , m_axisAlignedBoundBox(6)
  , m_notTransformedBoundBox(6)
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
  connect(&m_manipulatorSize, SIGNAL(valueChanged()), this, SLOT(invalidateHandle()));

  connect(&m_rendererBase, SIGNAL(coordTransformChanged()), this, SLOT(updateAxisAlignedBoundBox()));
  connect(&m_rendererBase, SIGNAL(sizeScaleChanged()), this, SLOT(updateBoundBox()));
  connect(&m_rendererBase.globalCameraPara(), SIGNAL(valueChanged()), this, SLOT(invalidateHandle()));

  m_xCut.setSingleStep(1);
  m_yCut.setSingleStep(1);
  m_zCut.setSingleStep(1);
  connect(&m_xCut, SIGNAL(valueChanged()), this, SLOT(setClipPlanes()));
  connect(&m_yCut, SIGNAL(valueChanged()), this, SLOT(setClipPlanes()));
  connect(&m_zCut, SIGNAL(valueChanged()), this, SLOT(setClipPlanes()));
  connect(&m_boundBoxMode, SIGNAL(valueChanged()), this, SLOT(onBoundBoxModeChanged()));
  m_boundBoxLineColor.setStyle("COLOR");
  //m_boundBoxLineColor.get().reset(0., 1., QColor(133,163,240,255), QColor(248,60,35,255));
  //m_boundBoxLineColor.get().addKey(ZColorMapKey(.1, QColor(233,239,235,255)));
  //m_boundBoxLineColor.get().addKey(ZColorMapKey(.2, QColor(240,241,237,255)));
  //m_boundBoxLineColor.get().addKey(ZColorMapKey(.3, QColor(248,205,165,255)));
  connect(&m_boundBoxLineColor, SIGNAL(valueChanged()), this, SLOT(updateBoundBoxLineColors()));
  m_selectionLineColor.setStyle("COLOR");
  connect(&m_selectionLineColor, SIGNAL(valueChanged()), this, SLOT(updateSelectionLineColors()));

  addParameter(m_xCut);
  addParameter(m_yCut);
  addParameter(m_zCut);
  addParameter(m_boundBoxMode);
  addParameter(m_boundBoxLineWidth);
  addParameter(m_boundBoxLineColor);
  addParameter(m_selectionLineWidth);
  addParameter(m_selectionLineColor);

  onBoundBoxModeChanged();

  m_baseBoundBoxRenderer.setUseDisplayList(false);
  m_baseBoundBoxRenderer.setFollowSizeScale(false);
  m_baseBoundBoxRenderer.setFollowCoordTransform(false);
  m_baseBoundBoxRenderer.setLineWidth(m_boundBoxLineWidth.get());
  connect(&m_boundBoxLineWidth, SIGNAL(valueChanged()), this, SLOT(onBoundBoxLineWidthChanged()));
  updateBoundBoxLineColors();

  m_selectionBoundBoxRenderer.setUseDisplayList(false);
  m_selectionBoundBoxRenderer.setFollowCoordTransform(false);
  m_selectionBoundBoxRenderer.setFollowSizeScale(false);
  m_selectionBoundBoxRenderer.setFollowOpacity(false);
  m_selectionBoundBoxRenderer.setEnableMultisample(false);
  m_selectionBoundBoxRenderer.setLineWidth(m_selectionLineWidth.get());
  connect(&m_selectionLineWidth, SIGNAL(valueChanged()), this, SLOT(onSelectionBoundBoxLineWidthChanged()));
  updateSelectionLineColors();

  m_selectionCornerRenderer.setColorSource("CustomColor");
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
  m_handleCenterColors.emplace_back(.5,.5,0,.5);
  m_handleCenterRenderer.setDataColors(&m_handleCenterColors);
  m_handleArrowColors.emplace_back(.5,0,0,.5);
  m_handleArrowColors.emplace_back(0,.5,0,.5);
  m_handleArrowColors.emplace_back(0,0,.5,.5);
  m_handleArrowRenderer.setArrowColors(&m_handleArrowColors);
  registerHandlePickingColors();

  m_handleEvent.listenTo("transform handle", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
  m_handleEvent.listenTo("transform handle", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonRelease);
  m_handleEvent.listenTo("transform handle", Qt::LeftButton, Qt::NoModifier, QEvent::MouseMove);
  connect(&m_handleEvent, SIGNAL(mouseEventTriggered(QMouseEvent*,int,int)), this, SLOT(handleEvent(QMouseEvent*,int,int)));
  addEventListener(m_handleEvent);
  m_handleEvent.setEnabled(m_isSelected);

  const std::vector<ZParameter*>& globalParas = m_rendererBase.globalParameters();
  for (size_t i=0; i<globalParas.size(); i++) {
    connect(globalParas[i], SIGNAL(valueChanged()), this, SLOT(invalidateResult()));
  }
  const std::vector<ZParameter*>& paras = m_rendererBase.parameters();
  for (size_t i=0; i<paras.size(); i++) {
    addParameter(paras[i]);
  }
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
    if (!m_handleValid)
      updateHandle();
    m_rendererBase.setClipEnabled(false);
    m_rendererBase.render(eye, m_handleArrowRenderer, m_handleCenterRenderer);
    m_rendererBase.setClipEnabled(true);
  }
}

void Z3DBoundedFilter::renderHandlePicking(Z3DEye eye)
{
  if (m_isSelected) {
    m_rendererBase.setClipEnabled(false);
    m_rendererBase.renderPicking(eye, m_handleArrowRenderer, m_handleCenterRenderer);
    m_rendererBase.setClipEnabled(true);
  }
}

void Z3DBoundedFilter::renderSelectionBox(Z3DEye eye)
{
  if (m_isSelected) {
    m_selectionLines.resize(24);
    addSelectionLines();
    m_selectionBoundBoxRenderer.setData(&m_selectionLines);
    if (m_selectionLineColors.size() < m_selectionLines.size()) {
      for (size_t i=m_selectionLineColors.size(); i<m_selectionLines.size(); ++i) {
        m_selectionLineColors.push_back(m_selectionLineColor.get());
      }
      m_selectionBoundBoxRenderer.setDataColors(&m_selectionLineColors);
    }
    m_rendererBase.setClipEnabled(false);
    m_rendererBase.render(eye, m_selectionBoundBoxRenderer, m_selectionCornerRenderer);
    m_rendererBase.setClipEnabled(true);
  }
}

void Z3DBoundedFilter::rotateX()
{
  if (!m_isSelected || !m_transformEnabled)
    return;
  m_rendererBase.coordTransformPara().rotate(glm::vec3(1,0,0), 2 * M_PI / 360., m_center);
}

void Z3DBoundedFilter::rotateY()
{
  if (!m_isSelected || !m_transformEnabled)
    return;
  m_rendererBase.coordTransformPara().rotate(glm::vec3(0,1,0), 2 * M_PI / 360., m_center);
}

void Z3DBoundedFilter::rotateZ()
{
  if (!m_isSelected || !m_transformEnabled)
    return;
  m_rendererBase.coordTransformPara().rotate(glm::vec3(0,0,1), 2 * M_PI / 360., m_center);
}

void Z3DBoundedFilter::rotateXM()
{
  if (!m_isSelected || !m_transformEnabled)
    return;
  m_rendererBase.coordTransformPara().rotate(glm::vec3(1,0,0), -2 * M_PI / 360., m_center);
}

void Z3DBoundedFilter::rotateYM()
{
  if (!m_isSelected || !m_transformEnabled)
    return;
  m_rendererBase.coordTransformPara().rotate(glm::vec3(0,1,0), -2 * M_PI / 360., m_center);
}

void Z3DBoundedFilter::rotateZM()
{
  if (!m_isSelected || !m_transformEnabled)
    return;
  m_rendererBase.coordTransformPara().rotate(glm::vec3(0,0,1), -2 * M_PI / 360., m_center);
}

void Z3DBoundedFilter::updateBoundBox()
{
  updateNotTransformedBoundBox();
  m_normalBoundBoxLines.clear();
  appendBoundboxLines(m_notTransformedBoundBox, m_normalBoundBoxLines);
  if (m_boundBoxMode.isSelected("Bound Box"))
    m_baseBoundBoxRenderer.setData(&m_normalBoundBoxLines);
  updateAxisAlignedBoundBox();
}

void Z3DBoundedFilter::setClipPlanes()
{
  if (!m_canUpdateClipPlane)
    return;
  std::vector<glm::vec4> clipPlanes;
  if (m_xCut.lowerValue() != m_xCut.minimum())
    clipPlanes.emplace_back(1., 0., 0., -m_xCut.lowerValue());
  if (m_xCut.upperValue() != m_xCut.maximum())
    clipPlanes.emplace_back(-1., 0., 0., m_xCut.upperValue());
  if (m_yCut.lowerValue() != m_yCut.minimum())
    clipPlanes.emplace_back(0., 1., 0., -m_yCut.lowerValue());
  if (m_yCut.upperValue() != m_yCut.maximum())
    clipPlanes.emplace_back(0., -1., 0., m_yCut.upperValue());
  if (m_zCut.lowerValue() != m_zCut.minimum())
    clipPlanes.emplace_back(0., 0., 1., -m_zCut.lowerValue());
  if (m_zCut.upperValue() != m_zCut.maximum())
    clipPlanes.emplace_back(0., 0., -1., m_zCut.upperValue());
  m_rendererBase.setClipPlanes(&clipPlanes);
}

void Z3DBoundedFilter::handleEvent(QMouseEvent *e, int w, int h)
{
  e->ignore();
  // Mouse button pressend
  if (e->type() == QEvent::MouseButtonPress) {
    m_lastMousePosition = glm::ivec2(e->x(), e->y());
    const void* obj = pickingManager().objectAtWidgetPos(m_lastMousePosition);
    int handleIdx = selectedHandle(obj);
    if (handleIdx == 0)
      return;
    updateSelectedHandle(handleIdx);
    glm::ivec4 viewport(0, 0, w, h);
    m_startTrans = m_rendererBase.coordTransformPara().translation();
    if (handleIdx == 1) {
      m_startDepth = camera().worldToScreen(m_center, viewport).z;
      m_startMouseWorldPos = camera().screenToWorld(glm::vec3(e->x(), h-e->y(), m_startDepth), viewport);
    } else {
      GLfloat WindowPosZ;
      pickingManager().bindTarget();
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(e->x(), h-e->y(), 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &WindowPosZ);
      pickingManager().releaseTarget();
      CHECK_GL_ERROR;
      m_startMouseWorldPos = camera().screenToWorld(glm::vec3(e->x(), h-e->y(), WindowPosZ), viewport);
    }
    e->accept();
    return;
  }

  if (e->type() == QEvent::MouseMove) {
    if (m_selectedHandle == 0)
      return;
    if (m_selectedHandle == 1) {
      glm::vec3 endInWorld = camera().screenToWorld(glm::vec3(glm::vec2(e->x(), h - e->y()), m_startDepth), glm::ivec4(0,0,w,h));
      m_rendererBase.coordTransformPara().setTranslation(m_startTrans + endInWorld - m_startMouseWorldPos);
    } else {
      glm::vec3 v1, v2;
      rayUnderScreenPoint(v1, v2, e->x(), e->y(), w, h);
      v2 -= v1;
      Wm5::Ray3f ray(Wm5::Vector3f(v1.x,v1.y,v1.z), Wm5::Vector3f(v2.x,v2.y,v2.z));
      if (m_selectedHandle == 2) {
        Wm5::Line3f xLine(Wm5::Vector3f(m_startMouseWorldPos.x,m_startMouseWorldPos.y,m_startMouseWorldPos.z), Wm5::Vector3f(1,0,0));
        Wm5::DistLine3Ray3f dist(xLine, ray);
        dist.GetSquared();
        m_rendererBase.coordTransformPara().setTranslation(m_startTrans + glm::vec3(dist.GetLineParameter(), 0, 0));
      } else if (m_selectedHandle == 3) {
        Wm5::Line3f xLine(Wm5::Vector3f(m_startMouseWorldPos.x,m_startMouseWorldPos.y,m_startMouseWorldPos.z), Wm5::Vector3f(0,1,0));
        Wm5::DistLine3Ray3f dist(xLine, ray);
        dist.GetSquared();
        m_rendererBase.coordTransformPara().setTranslation(m_startTrans + glm::vec3(0, dist.GetLineParameter(), 0));
      } else if (m_selectedHandle == 4) {
        Wm5::Line3f xLine(Wm5::Vector3f(m_startMouseWorldPos.x,m_startMouseWorldPos.y,m_startMouseWorldPos.z), Wm5::Vector3f(0,0,1));
        Wm5::DistLine3Ray3f dist(xLine, ray);
        dist.GetSquared();
        m_rendererBase.coordTransformPara().setTranslation(m_startTrans + glm::vec3(0, 0, dist.GetLineParameter()));
      }
    }
    m_lastMousePosition = glm::ivec2(e->x(), e->y());
    return;
  }

  if (e->type() == QEvent::MouseButtonRelease) {
    updateSelectedHandle(0);
  }
}

void Z3DBoundedFilter::initializeCutRange()
{
  m_canUpdateClipPlane = false;
  const std::vector<double> &bound = notTransformedBoundBox();
  m_xCut.setRange(std::floor(bound[0])-1, std::ceil(bound[1])+1);
  m_xCut.set(m_xCut.range());
  m_yCut.setRange(std::floor(bound[2])-1, std::ceil(bound[3])+1);
  m_yCut.set(m_yCut.range());
  m_zCut.setRange(std::floor(bound[4])-1, std::ceil(bound[5])+1);
  m_zCut.set(m_zCut.range());
  m_canUpdateClipPlane = true;
  m_rendererBase.setClipPlanes(nullptr);
}

void Z3DBoundedFilter::initializeRotationCenter()
{
  const std::vector<double> &bound = notTransformedBoundBox();
  m_rendererBase.setRotationCenter(glm::vec3((bound[0]+bound[1])/2.0, (bound[2]+bound[3])/2.0, (bound[4]+bound[5])/2.0));
}

void Z3DBoundedFilter::renderBoundBox(Z3DEye eye)
{
  if (!m_boundBoxMode.isSelected("No Bound Box")) {
    m_rendererBase.setClipEnabled(false);
    m_rendererBase.render(eye, m_baseBoundBoxRenderer);
    m_rendererBase.setClipEnabled(true);
  }
}

void Z3DBoundedFilter::appendBoundboxLines(const std::vector<double> &bound, std::vector<glm::vec3> &lines)
{
  float xmin = bound[0];
  float xmax = bound[1];
  float ymin = bound[2];
  float ymax = bound[3];
  float zmin = bound[4];
  float zmax = bound[5];
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

void Z3DBoundedFilter::rayUnderScreenPoint(glm::vec3 &v1, glm::vec3 &v2, int x, int y, int width, int height)
{
  const glm::mat4& projection = globalCamera().projectionMatrix(Z3DEye::Mono);
  const glm::mat4& modelview = globalCamera().viewMatrix(Z3DEye::Mono);

  glm::ivec4 viewport;
  viewport[0] = 0;
  viewport[1] = 0;
  viewport[2] = width;
  viewport[3] = height;

  v1 = glm::unProject(glm::vec3(x, height-y, 0.f), modelview, projection, viewport);
  v2 = glm::unProject(glm::vec3(x, height-y, 1.f), modelview, projection, viewport);
  v2 = glm::normalize(v2-v1) + v1;
}

void Z3DBoundedFilter::rayUnderScreenPoint(glm::dvec3 &v1, glm::dvec3 &v2, int x, int y, int width, int height)
{
  const glm::dmat4& projection = glm::dmat4(globalCamera().projectionMatrix(Z3DEye::Mono));
  const glm::dmat4& modelview = glm::dmat4(globalCamera().viewMatrix(Z3DEye::Mono));

  glm::ivec4 viewport;
  viewport[0] = 0;
  viewport[1] = 0;
  viewport[2] = width;
  viewport[3] = height;

  v1 = glm::unProject(glm::dvec3(x, height-y, 0.f), modelview, projection, viewport);
  v2 = glm::unProject(glm::dvec3(x, height-y, 1.f), modelview, projection, viewport);
  v2 = glm::normalize(v2-v1) + v1;
}

void Z3DBoundedFilter::updateAxisAlignedBoundBoxImpl()
{
  if (m_notTransformedBoundBox[0] > m_notTransformedBoundBox[1] ||
      m_notTransformedBoundBox[2] > m_notTransformedBoundBox[3] ||
      m_notTransformedBoundBox[4] > m_notTransformedBoundBox[5]) {
    m_axisAlignedBoundBox[0] = m_axisAlignedBoundBox[2] = m_axisAlignedBoundBox[4] = std::numeric_limits<double>::max();
    m_axisAlignedBoundBox[1] = m_axisAlignedBoundBox[3] = m_axisAlignedBoundBox[5] = -std::numeric_limits<double>::max();
    return;
  }

  glm::vec3 minCoord = glm::min(worldLUF(), worldRDB());
  glm::vec3 maxCoord = glm::max(worldLUF(), worldRDB());

  minCoord = glm::min(minCoord, worldLDB());
  maxCoord = glm::max(maxCoord, worldLDB());

  minCoord = glm::min(minCoord, worldLDF());
  maxCoord = glm::max(maxCoord, worldLDF());

  minCoord = glm::min(minCoord, worldLUB());
  maxCoord = glm::max(maxCoord, worldLUB());

  minCoord = glm::min(minCoord, worldRUF());
  maxCoord = glm::max(maxCoord, worldRUF());

  minCoord = glm::min(minCoord, worldRDF());
  maxCoord = glm::max(maxCoord, worldRDF());

  minCoord = glm::min(minCoord, worldRUB());
  maxCoord = glm::max(maxCoord, worldRUB());

  m_axisAlignedBoundBox[0] = minCoord.x;
  m_axisAlignedBoundBox[1] = maxCoord.x;
  m_axisAlignedBoundBox[2] = minCoord.y;
  m_axisAlignedBoundBox[3] = maxCoord.y;
  m_axisAlignedBoundBox[4] = minCoord.z;
  m_axisAlignedBoundBox[5] = maxCoord.z;
}

void Z3DBoundedFilter::expandCutRange()
{
  m_canUpdateClipPlane = false;
  const std::vector<double> &bound = notTransformedBoundBox();
  bool noLowXCut = m_xCut.lowerValue() == m_xCut.minimum();
  bool noHighXCut = m_xCut.upperValue() == m_xCut.maximum();
  bool noLowYCut = m_yCut.lowerValue() == m_yCut.minimum();
  bool noHighYCut = m_yCut.upperValue() == m_yCut.maximum();
  bool noLowZCut = m_zCut.lowerValue() == m_zCut.minimum();
  bool noHighZCut = m_zCut.upperValue() == m_zCut.maximum();
  m_xCut.setRange(std::min(m_xCut.minimum(), float(std::floor(bound[0])-1)),
      std::max(m_xCut.maximum(), float(std::ceil(bound[1])+1)));
  m_yCut.setRange(std::min(m_yCut.minimum(), float(std::floor(bound[2])-1)),
      std::max(m_yCut.maximum(), float(std::ceil(bound[3])+1)));
  m_zCut.setRange(std::min(m_yCut.minimum(), float(std::floor(bound[4])-1)),
      std::max(m_zCut.maximum(), float(std::ceil(bound[5])+1)));
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
  if (m_rendererBase.coordTransform() == glm::mat4(1.f)) {
    m_axisAlignedBoundBox = m_notTransformedBoundBox;
  } else {
    updateAxisAlignedBoundBoxImpl();
  }
  m_axisAlignedBoundBoxLines.clear();
  appendBoundboxLines(m_axisAlignedBoundBox, m_axisAlignedBoundBoxLines);
  if (m_boundBoxMode.isSelected("Axis Aligned Bound Box")) {
    m_baseBoundBoxRenderer.setData(&m_axisAlignedBoundBoxLines);
  }

  m_center = glm::vec3((m_axisAlignedBoundBox[0] + m_axisAlignedBoundBox[1]) / 2.f,
      (m_axisAlignedBoundBox[2] + m_axisAlignedBoundBox[3]) / 2.f,
      (m_axisAlignedBoundBox[4] + m_axisAlignedBoundBox[5]) / 2.f);

  makeSelectionGeometries();
  m_handleValid = false;

  emit boundBoxChanged();
}

void Z3DBoundedFilter::updateNotTransformedBoundBox()
{
  updateNotTransformedBoundBoxImpl();
  expandCutRange();
}

void Z3DBoundedFilter::onBoundBoxModeChanged()
{
  if (m_boundBoxMode.isSelected("Axis Aligned Bound Box")) {
    m_baseBoundBoxRenderer.setData(&m_axisAlignedBoundBoxLines);
    m_baseBoundBoxRenderer.setFollowCoordTransform(false);
  } else if (m_boundBoxMode.isSelected("Bound Box")) {
    m_baseBoundBoxRenderer.setData(&m_normalBoundBoxLines);
    m_baseBoundBoxRenderer.setFollowCoordTransform(true);
  }
  m_boundBoxLineWidth.setVisible(!m_boundBoxMode.isSelected("No Bound Box"));
  m_boundBoxLineColor.setVisible(!m_boundBoxMode.isSelected("No Bound Box"));
}

void Z3DBoundedFilter::updateBoundBoxLineColors()
{
  m_boundBoxLineColors.clear();
  m_boundBoxLineColors.resize(24, m_boundBoxLineColor.get());
  m_baseBoundBoxRenderer.setDataColors(&m_boundBoxLineColors);
  //m_baseBoundBoxRenderer->setTexture(m_boundBoxLineColor.get().getTexture());
}

void Z3DBoundedFilter::updateSelectionLineColors()
{
  m_selectionLineColors.clear();
  m_selectionLineColors.resize(24, m_selectionLineColor.get());
  m_selectionBoundBoxRenderer.setDataColors(&m_selectionLineColors);
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
  float sizeX = m_axisAlignedBoundBox[1] - m_axisAlignedBoundBox[0];
  float sizeY = m_axisAlignedBoundBox[3] - m_axisAlignedBoundBox[2];
  float sizeZ = m_axisAlignedBoundBox[5] - m_axisAlignedBoundBox[4];
  float size = sizeX + sizeY + sizeZ - std::max(sizeZ, std::max(sizeX, sizeY)) -
      std::min(sizeZ, std::min(sizeX, sizeY));
  float cornerRadius = std::min(100.f, 0.01f * size);
  m_selectionBoundBox = m_axisAlignedBoundBox;
  m_selectionBoundBox[0] -= cornerRadius;
  m_selectionBoundBox[1] += cornerRadius;
  m_selectionBoundBox[2] -= cornerRadius;
  m_selectionBoundBox[3] += cornerRadius;
  m_selectionBoundBox[4] -= cornerRadius;
  m_selectionBoundBox[5] += cornerRadius;
  m_selectionLines.clear();
  appendBoundboxLines(m_selectionBoundBox, m_selectionLines);

  glm::vec3 cornerShift(cornerRadius, cornerRadius, cornerRadius);

  std::vector<glm::vec3> lowcoords;
  std::vector<glm::vec3> highcoords;
  glm::vec3 pos(m_selectionBoundBox[0], m_selectionBoundBox[2], m_selectionBoundBox[4]);
  lowcoords.push_back(pos - cornerShift);
  highcoords.push_back(pos + cornerShift);
  pos = glm::vec3(m_selectionBoundBox[1], m_selectionBoundBox[2], m_selectionBoundBox[4]);
  lowcoords.push_back(pos - cornerShift);
  highcoords.push_back(pos + cornerShift);
  pos = glm::vec3(m_selectionBoundBox[0], m_selectionBoundBox[3], m_selectionBoundBox[4]);
  lowcoords.push_back(pos - cornerShift);
  highcoords.push_back(pos + cornerShift);
  pos = glm::vec3(m_selectionBoundBox[1], m_selectionBoundBox[3], m_selectionBoundBox[4]);
  lowcoords.push_back(pos - cornerShift);
  highcoords.push_back(pos + cornerShift);
  pos = glm::vec3(m_selectionBoundBox[0], m_selectionBoundBox[2], m_selectionBoundBox[5]);
  lowcoords.push_back(pos - cornerShift);
  highcoords.push_back(pos + cornerShift);
  pos = glm::vec3(m_selectionBoundBox[1], m_selectionBoundBox[2], m_selectionBoundBox[5]);
  lowcoords.push_back(pos - cornerShift);
  highcoords.push_back(pos + cornerShift);
  pos = glm::vec3(m_selectionBoundBox[0], m_selectionBoundBox[3], m_selectionBoundBox[5]);
  lowcoords.push_back(pos - cornerShift);
  highcoords.push_back(pos + cornerShift);
  pos = glm::vec3(m_selectionBoundBox[1], m_selectionBoundBox[3], m_selectionBoundBox[5]);
  lowcoords.push_back(pos - cornerShift);
  highcoords.push_back(pos + cornerShift);

  m_selectionCornerCubes = ZMesh::createCubesWithNormal(lowcoords, highcoords);
  m_selectionCornerCubesWrapper.clear();
  m_selectionCornerCubesWrapper.push_back(&m_selectionCornerCubes);
  m_selectionCornerRenderer.setData(&m_selectionCornerCubesWrapper);
}

void Z3DBoundedFilter::updateHandle()
{
  if (!m_handleValid) {
    Z3DCamera& camera = m_rendererBase.globalCamera();
    glm::mat4 mat = m_rendererBase.viewportMatrix() * camera.projectionMatrix(Z3DEye::Mono) *
        camera.viewMatrix(Z3DEye::Mono);
    glm::vec3 rightVector = glm::vec3(camera.viewMatrix(Z3DEye::Mono)[0][0], camera.viewMatrix(Z3DEye::Mono)[1][0], camera.viewMatrix(Z3DEye::Mono)[2][0]);
    glm::vec3 centerScreen = glm::applyMatrix(mat, m_center);
    glm::vec3 rightScreen = glm::applyMatrix(mat, m_center + rightVector);
    float size = m_manipulatorSize.get() / (rightScreen.x - centerScreen.x);

    m_handleCenterAndRadius[0] = glm::vec4(m_center, 0.18f * size);
    m_handleArrowTailPosAndTailRadius[0] = glm::vec4(m_center, 0.028f * size);
    m_handleArrowTailPosAndTailRadius[1] = glm::vec4(m_center, 0.028f * size);
    m_handleArrowTailPosAndTailRadius[2] = glm::vec4(m_center, 0.028f * size);
    m_handleArrowheadPosAndHeadRadius[0] = glm::vec4(m_center, 0.12f * size) + glm::vec4(size,0,0,0);
    m_handleArrowheadPosAndHeadRadius[1] = glm::vec4(m_center, 0.12f * size) + glm::vec4(0,size,0,0);
    m_handleArrowheadPosAndHeadRadius[2] = glm::vec4(m_center, 0.12f * size) + glm::vec4(0,0,size,0);
    m_handleCenterRenderer.setData(&m_handleCenterAndRadius);
    m_handleArrowRenderer.setArrowData(&m_handleArrowTailPosAndTailRadius,
                                       &m_handleArrowheadPosAndHeadRadius);

    m_handleValid = true;
  }
}

void Z3DBoundedFilter::registerHandlePickingColors()
{
  pickingManager().registerObject(&m_handleCenterRenderer);  // center
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

int Z3DBoundedFilter::selectedHandle(const void *obj)
{
  if (!obj)
    return 0;
  else if (obj == &m_handleCenterRenderer)
    return 1;
  else if (obj == &m_handleArrowRenderer)
    return 2;
  else if (obj == &m_handleArrowTailPosAndTailRadius)
    return 3;
  else if (obj == &m_handleArrowheadPosAndHeadRadius)
    return 4;
  else
    return 0;
}

void Z3DBoundedFilter::updateSelectedHandle(int handleIdx)
{
  if (handleIdx == m_selectedHandle)
    return;

  if (handleIdx == 0) {
    switch (m_selectedHandle) {
    case 1:
      m_handleCenterColors[0] = glm::vec4(.5,.5,0,.5);
      m_handleCenterRenderer.setDataColors(&m_handleCenterColors);
      break;
    case 2:
      m_handleArrowColors[0] = glm::vec4(.5,0,0,.5);
      m_handleArrowRenderer.setArrowColors(&m_handleArrowColors);
      break;
    case 3:
      m_handleArrowColors[1] = glm::vec4(0,.5,0,.5);
      m_handleArrowRenderer.setArrowColors(&m_handleArrowColors);
      break;
    case 4:
      m_handleArrowColors[2] = glm::vec4(0,0,.5,.5);
      m_handleArrowRenderer.setArrowColors(&m_handleArrowColors);
      break;
    default:
      assert(false);
    }
    interactionHandler().setEnabled(true);
  } else {
    assert(m_selectedHandle == 0);
    float av = 95.f / 255.f;
    switch (handleIdx) {
    case 1:
      m_handleCenterColors[0] = glm::vec4(.5,.5,av,.5);
      m_handleCenterRenderer.setDataColors(&m_handleCenterColors);
      break;
    case 2:
      m_handleArrowColors[0] = glm::vec4(.5,av,av,.5);
      m_handleArrowRenderer.setArrowColors(&m_handleArrowColors);
      break;
    case 3:
      m_handleArrowColors[1] = glm::vec4(av,.5,av,.5);
      m_handleArrowRenderer.setArrowColors(&m_handleArrowColors);
      break;
    case 4:
      m_handleArrowColors[2] = glm::vec4(av,av,.5,.5);
      m_handleArrowRenderer.setArrowColors(&m_handleArrowColors);
      break;
    default:
      assert(false);
    }
    interactionHandler().setEnabled(false);
  }

  invalidateResult();
  m_selectedHandle = handleIdx;
}

} // namespace nim
