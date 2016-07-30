#include "z3dswcfilter.h"

#include <iostream>

#include "zrandom.h"
#include "zlog.h"
#include <QMessageBox>
#include <QApplication>

namespace nim {

Z3DSwcFilter::Z3DSwcFilter(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DGeometryFilter(globalParas, parent)
  , m_lineRenderer(m_rendererBase)
  , m_coneRenderer(m_rendererBase)
  , m_sphereRenderer(m_rendererBase)
  , m_sphereRendererForCone(m_rendererBase)
  , m_visible("Visible", true)
  , m_renderingPrimitive("Rendering Mode")
  , m_colorMode("Color Mode")
  , m_swcTreeColor("Color", glm::vec4(1, 0, 0, 1))
  , m_colorMapBranchType("Branch Type Color Map")
  , m_selectSwcEvent("Select Puncta", false)
  , m_pressedSwc(nullptr)
  , m_selectedSwcs(nullptr)
  , m_pressedSwcTreeNode(nullptr)
  , m_selectedSwcTreeNodes(nullptr)
  , m_dataIsInvalid(false)
  , m_swcTree(nullptr)
  , m_registeredSwcTree(nullptr)
  , m_interactionMode(InteractionMode::Select)
{
  initTopologyColor();
  initTypeColor();
  initSubclassTypeColor();

  addParameter(m_visible);
  // rendering primitive
  m_renderingPrimitive.addOptions("Normal", "Line", "Sphere", "Cylinder");
  m_renderingPrimitive.select("Normal");
  connect(&m_renderingPrimitive, &ZStringIntOptionParameter::valueChanged, this, &Z3DSwcFilter::updateBoundBox);

  m_colorMode.addOptions("Single Color",
                         "Branch Type",
                         "Topology",
                         "Colormap Branch Type",
                         "Subclass");

  m_colorMode.select("Branch Type");

  connect(&m_colorMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DSwcFilter::prepareColor);
  connect(&m_colorMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DSwcFilter::adjustWidgets);

  addParameter(m_renderingPrimitive);
  addParameter(m_colorMode);

  m_swcTreeColor.setStyle("COLOR");
  connect(&m_swcTreeColor, &ZVec4Parameter::valueChanged, this, &Z3DSwcFilter::prepareColor);
  addParameter(m_swcTreeColor);

  for (size_t i = 0; i < m_colorsForDifferentType.size(); i++) {
    addParameter(*m_colorsForDifferentType[i].get());
  }

  for (size_t i = 0; i < m_colorsForDifferentTopology.size(); i++) {
    addParameter(*m_colorsForDifferentTopology[i].get());
  }

  for (size_t i = 0; i < m_colorsForSubclassType.size(); i++) {
    addParameter(*m_colorsForSubclassType[i].get());
  }

  m_selectSwcEvent.listenTo("select swc", Qt::LeftButton,
                            Qt::NoModifier, QEvent::MouseButtonPress);
  m_selectSwcEvent.listenTo("select swc", Qt::LeftButton,
                            Qt::NoModifier, QEvent::MouseButtonRelease);
  m_selectSwcEvent.listenTo("select swc", Qt::LeftButton,
                            Qt::NoModifier, QEvent::MouseButtonDblClick);
  m_selectSwcEvent.listenTo("select swc", Qt::LeftButton,
                            Qt::ControlModifier, QEvent::MouseButtonDblClick);
  /*
  m_selectSwcEvent.listenTo("select swc", Qt::RightButton,
                             Qt::NoModifier, QEvent::MouseButtonPress);
  m_selectSwcEvent.listenTo("select swc", Qt::RightButton, Qt::NoModifier,
                             QEvent::MouseButtonRelease);
                             */
  m_selectSwcEvent.listenTo("select swc connection", Qt::LeftButton,
                            Qt::ShiftModifier, QEvent::MouseButtonPress);
  m_selectSwcEvent.listenTo("select swc connection", Qt::LeftButton,
                            Qt::ShiftModifier, QEvent::MouseButtonRelease);

  m_selectSwcEvent.listenTo("append select swc", Qt::LeftButton,
                            Qt::ControlModifier, QEvent::MouseButtonPress);
  m_selectSwcEvent.listenTo("append select swc", Qt::LeftButton,
                            Qt::ControlModifier, QEvent::MouseButtonRelease);

  /*
  m_selectSwcEvent.listenTo("append select swc", Qt::RightButton,
                             Qt::ControlModifier, QEvent::MouseButtonPress);
  m_selectSwcEvent.listenTo("append select swc", Qt::RightButton,
                             Qt::ControlModifier, QEvent::MouseButtonRelease);
*/
  connect(&m_selectSwcEvent, &ZEventListenerParameter::mouseEventTriggered,
          this, &Z3DSwcFilter::selectSwc);
  addEventListener(m_selectSwcEvent);

  addParameter(m_colorMapBranchType);
  connect(&m_colorMapBranchType, &ZColorMapParameter::valueChanged, this, &Z3DSwcFilter::prepareColor);

  adjustWidgets();

  connect(&m_visible, &ZBoolParameter::boolChanged, this, &Z3DSwcFilter::objVisibleChanged);
}

Z3DSwcFilter::~Z3DSwcFilter()
{
}

void Z3DSwcFilter::process(Z3DEye)
{
  if (m_dataIsInvalid) {
    prepareData();
  }
}

void Z3DSwcFilter::initTopologyColor()
{
  // topology colors (root, branch point, leaf, others)
  m_colorsForDifferentTopology.emplace_back(
    std::make_unique<ZVec4Parameter>("Root Color", glm::vec4(0 / 255.f, 0 / 255.f, 255 / 255.f, 1.f)));
  m_colorsForDifferentTopology.emplace_back(
    std::make_unique<ZVec4Parameter>("Branch Point Color", glm::vec4(0 / 255.f, 255 / 255.f, 0 / 255.f, 1.f)));
  m_colorsForDifferentTopology.emplace_back(
    std::make_unique<ZVec4Parameter>("Leaf Color", glm::vec4(255 / 255.f, 255 / 255.f, 0 / 255.f, 1.f)));
  m_colorsForDifferentTopology.emplace_back(
    std::make_unique<ZVec4Parameter>("Other", glm::vec4(255 / 255.f, 0 / 255.f, 0 / 255.f, 1.f)));
  for (size_t i = 0; i < m_colorsForDifferentTopology.size(); i++) {
    m_colorsForDifferentTopology[i]->setStyle("COLOR");
    connect(m_colorsForDifferentTopology[i].get(), &ZVec4Parameter::valueChanged, this, &Z3DSwcFilter::prepareColor);
  }
}

void Z3DSwcFilter::initTypeColor()
{
  // type colors
  int index = 0;
  QString name = QString("Type %1 Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(255 / 255.f, 255 / 255.f, 255 / 255.f, 1.f))); //white
  // 1
  name = QString("Type %1 (Soma) Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(20 / 255.f, 20 / 255.f, 20 / 255.f, 1.f))); //black
  // 2
  name = QString("Type %1 (Axon) Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(200 / 255.f, 20 / 255.f, 0 / 255.f, 1.f))); //red
  // 3
  name = QString("Type %1 (Basal Dendrite) Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0 / 255.f, 20 / 255.f, 200 / 255.f, 1.f))); //blue
  // 4
  name = QString("Type %1 (Apical Dendrite) Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(200 / 255.f, 0 / 255.f, 200 / 255.f, 1.f))); //purple
  // 5
  name = QString("Type %1 (Main Trunk) Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0 / 255.f, 200 / 255.f, 200 / 255.f, 1.f))); //cyan
  // 6
  name = QString("Type %1 (Basal Intermediate) Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(220 / 255.f, 200 / 255.f, 0 / 255.f, 1.f))); //yellow
  // 7
  name = QString("Type %1 (Basal Terminal) Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0 / 255.f, 200 / 255.f, 20 / 255.f, 1.f))); //green
  // 8
  name = QString("Type %1 (Apical Oblique Intermediate) Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(188 / 255.f, 94 / 255.f, 37 / 255.f, 1.f))); //coffee
  // 9
  name = QString("Type %1 (Apical Oblique Terminal) Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(180 / 255.f, 200 / 255.f, 120 / 255.f, 1.f))); //asparagus
  // 10
  name = QString("Type %1 (Apical Tuft) Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(250 / 255.f, 100 / 255.f, 120 / 255.f, 1.f))); //salmon
  // 11
  name = QString("Type %1 Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(120 / 255.f, 200 / 255.f, 200 / 255.f, 1.f))); //ice
  // 12
  name = QString("Type %1 Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(100 / 255.f, 120 / 255.f, 200 / 255.f, 1.f))); //orchid
  // 13
  name = QString("Type %1 Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(255 / 255.f, 128 / 255.f, 168 / 255.f, 1.f)));
  // 14
  name = QString("Type %1 Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(128 / 255.f, 255 / 255.f, 168 / 255.f, 1.f)));
  // 15
  name = QString("Type %1 Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(128 / 255.f, 168 / 255.f, 255 / 255.f, 1.f)));
  // 16
  name = QString("Type %1 Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(128 / 255.f, 255 / 255.f, 168 / 255.f, 1.f)));
  // 17
  name = QString("Type %1 Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(255 / 255.f, 168 / 255.f, 128 / 255.f, 1.f)));
  // 18
  name = QString("Type %1 Color").arg(index++);
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(168 / 255.f, 128 / 255.f, 255 / 255.f, 1.f)));
  // 19
  name = QString("Undefined Type Color");
  m_colorsForDifferentType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0xcc / 255.f, 0xcc / 255.f, 0xcc / 255.f, 1.f)));

  for (size_t i = 0; i < m_colorsForDifferentType.size(); i++) {
    m_colorsForDifferentType[i]->setStyle("COLOR");
    connect(m_colorsForDifferentType[i].get(), &ZVec4Parameter::valueChanged, this, &Z3DSwcFilter::prepareColor);
  }
}

void Z3DSwcFilter::initSubclassTypeColor()
{
  // subclass type color
  QString name = QString("Soma Color");
  m_subclassTypeColorMapper[1] = m_colorsForSubclassType.size();
  m_colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0 / 255.f, 0 / 255.f, 0 / 255.f, 1.f)));
  name = QString("Main Trunk Color");
  m_subclassTypeColorMapper[5] = m_colorsForSubclassType.size();
  m_colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0 / 255.f, 0 / 255.f, 0 / 255.f, 1.f)));
  name = QString("Basal Intermediate Color");
  m_subclassTypeColorMapper[6] = m_colorsForSubclassType.size();
  m_colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0x33 / 255.f, 0xcc / 255.f, 0xff / 255.f, 1.f)));
  name = QString("Basal Terminal Color");
  m_subclassTypeColorMapper[7] = m_colorsForSubclassType.size();
  m_colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0x33 / 255.f, 0x66 / 255.f, 0xcc / 255.f, 1.f)));
  name = QString("Apical Oblique Intermediate Color");
  m_subclassTypeColorMapper[8] = m_colorsForSubclassType.size();
  m_colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0xff / 255.f, 0xff / 255.f, 0 / 255.f, 1.f)));
  name = QString("Apical Oblique Terminal Color");
  m_subclassTypeColorMapper[9] = m_colorsForSubclassType.size();
  m_colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0xcc / 255.f, 0x33 / 255.f, 0x66 / 255.f, 1.f)));
  name = QString("Apical Tuft Color");
  m_subclassTypeColorMapper[10] = m_colorsForSubclassType.size();
  m_colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0 / 255.f, 0x99 / 255.f, 0 / 255.f, 1.f)));
  name = QString("Other Undefined class Color");
  m_colorsForSubclassType.emplace_back(
    std::make_unique<ZVec4Parameter>(name, glm::vec4(0xcc / 255.f, 0xcc / 255.f, 0xcc / 255.f, 1.f)));
  for (size_t i = 0; i < m_colorsForSubclassType.size(); i++) {
    m_colorsForSubclassType[i]->setStyle("COLOR");
    connect(m_colorsForSubclassType[i].get(), &ZVec4Parameter::valueChanged, this, &Z3DSwcFilter::prepareColor);
  }
}

void Z3DSwcFilter::registerPickingObjects()
{
  if (!m_pickingObjectsRegistered) {
    pickingManager().registerObject(m_swcTree);
    for (size_t j = 0; j < m_decomposedNodes.size(); j++) {
      pickingManager().registerObject(&m_decomposedNodes[j]);
      m_registeredSwcTreeNodeList.push_back(&m_decomposedNodes[j]);
    }
    m_registeredSwcTree = m_swcTree;
    m_swcPickingColors.clear();
    m_linePickingColors.clear();
    m_pointPickingColors.clear();
    glm::col4 pickingColor = pickingManager().colorOfObject(m_swcTree);
    glm::vec4 fPickingColor(pickingColor[0] / 255.f, pickingColor[1] / 255.f, pickingColor[2] / 255.f,
                            pickingColor[3] / 255.f);
    for (size_t j = 0; j < m_decompsedNodePairs.size(); j++) {
      m_swcPickingColors.push_back(fPickingColor);
      m_linePickingColors.push_back(fPickingColor);
      m_linePickingColors.push_back(fPickingColor);
    }
    m_sphereForConePickingColors = m_swcPickingColors;
    m_sphereForConePickingColors.push_back(fPickingColor);
    for (size_t j = 0; j < m_decomposedNodes.size(); j++) {
      pickingColor = pickingManager().colorOfObject(&m_decomposedNodes[j]);
      fPickingColor = glm::vec4(pickingColor[0] / 255.f, pickingColor[1] / 255.f, pickingColor[2] / 255.f,
                                pickingColor[3] / 255.f);
      m_pointPickingColors.push_back(fPickingColor);
    }

    m_coneRenderer.setDataPickingColors(&m_swcPickingColors);
    m_sphereRendererForCone.setDataPickingColors(&m_sphereForConePickingColors);
    m_lineRenderer.setDataPickingColors(&m_linePickingColors);
    m_sphereRenderer.setDataPickingColors(&m_pointPickingColors);
  }

  m_pickingObjectsRegistered = true;
}

void Z3DSwcFilter::deregisterPickingObjects()
{
  if (m_pickingObjectsRegistered) {
    if (m_registeredSwcTree)
      pickingManager().deregisterObject(m_registeredSwcTree);
    for (size_t i = 0; i < m_registeredSwcTreeNodeList.size(); i++) {
      pickingManager().deregisterObject(m_registeredSwcTreeNodeList[i]);
    }
    m_registeredSwcTree = nullptr;
    m_registeredSwcTreeNodeList.clear();
  }

  m_pickingObjectsRegistered = false;
}

void Z3DSwcFilter::setData(ZSwc& tree)
{
  m_swcTree = &tree;
  m_dataIsInvalid = true;
  invalidateResult();

  updateBoundBox();
}

bool Z3DSwcFilter::isReady(Z3DEye eye) const
{
  return Z3DGeometryFilter::isReady(eye) && m_visible.get() && m_swcTree;
}

std::shared_ptr<ZWidgetsGroup> Z3DSwcFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Swc", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_stayOnTop, 1);
    m_widgetsGroup->addChild(m_renderingPrimitive, 1);
    m_widgetsGroup->addChild(m_colorMode, 1);
    m_widgetsGroup->addChild(m_swcTreeColor, 1);

    for (size_t i = 0; i < m_colorsForDifferentType.size(); i++) {
      m_widgetsGroup->addChild(*m_colorsForDifferentType[i], 1);
    }
    for (size_t i = 0; i < m_colorsForSubclassType.size(); i++) {
      m_widgetsGroup->addChild(*m_colorsForSubclassType[i], 1);
    }
    for (size_t i = 0; i < m_colorsForDifferentTopology.size(); i++) {
      m_widgetsGroup->addChild(*m_colorsForDifferentTopology[i], 1);
    }
    m_widgetsGroup->addChild(m_colorMapBranchType, 1);

    const std::vector<ZParameter*>& paras = m_rendererBase.parameters();
    for (size_t i = 0; i < paras.size(); i++) {
      ZParameter* para = paras[i];
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

void Z3DSwcFilter::renderOpaque(Z3DEye eye)
{
  if (m_renderingPrimitive.isSelected("Normal")) {
    m_rendererBase.render(eye, m_sphereRendererForCone, m_coneRenderer);
  } else if (m_renderingPrimitive.isSelected("Cylinder")) {
    m_rendererBase.render(eye, m_coneRenderer);
  } else if (m_renderingPrimitive.isSelected("Line")) {
    m_rendererBase.render(eye, m_lineRenderer);
  } else /* (m_renderingPrimitive.get() == "Sphere") */{
    m_rendererBase.render(eye, m_lineRenderer, m_sphereRenderer);
  }
  renderBoundBox(eye);
}

void Z3DSwcFilter::renderTransparent(Z3DEye eye)
{
  if (m_renderingPrimitive.isSelected("Normal")) {
    m_rendererBase.render(eye, m_sphereRendererForCone, m_coneRenderer);
  } else if (m_renderingPrimitive.isSelected("Cylinder")) {
    m_rendererBase.render(eye, m_coneRenderer);
  } else if (m_renderingPrimitive.isSelected("Line")) {
    m_rendererBase.render(eye, m_lineRenderer);
  } else /* (m_renderingPrimitive.get() == "Sphere") */{
    m_rendererBase.render(eye, m_lineRenderer, m_sphereRenderer);
  }
  renderBoundBox(eye);
}

void Z3DSwcFilter::renderPicking(Z3DEye eye)
{
  if (!m_pickingObjectsRegistered)
    registerPickingObjects();

  if (m_renderingPrimitive.isSelected("Normal")) {
    m_rendererBase.renderPicking(eye, m_coneRenderer, m_sphereRendererForCone);
  } else if (m_renderingPrimitive.isSelected("Cylinder")) {
    m_rendererBase.renderPicking(eye, m_coneRenderer);
  } else if (m_renderingPrimitive.isSelected("Line")) {
    m_rendererBase.renderPicking(eye, m_lineRenderer);
  } else /* (m_renderingPrimitive.get() == "Sphere") */{
    m_rendererBase.renderPicking(eye, m_lineRenderer, m_sphereRenderer);
  }
}

void Z3DSwcFilter::addSelectionBox(
  const std::pair<SwcTreeNode, SwcTreeNode>& nodePair,
  std::vector<glm::vec3>& lines)
{
  const SwcTreeNode& n1 = nodePair.first;
  const SwcTreeNode& n2 = nodePair.second;
  //  glm::vec3 bPos(n1->node.x * getCoordTransform().x,
  //                 n1->node.y * getCoordTransform().y,
  //                 n1->node.z * getCoordTransform().z);
  //  glm::vec3 tPos(n2->node.x * getCoordTransform().x,
  //                 n2->node.y * getCoordTransform().y,
  //                 n2->node.z * getCoordTransform().z);
  glm::vec3 bPos = glm::applyMatrix(coordTransform(), glm::vec3(n1->x, n1->y, n1->z));
  glm::vec3 tPos = glm::applyMatrix(coordTransform(), glm::vec3(n2->x, n2->y, n2->z));
  float bRadius = std::max(.5, n1->radius) * m_rendererBase.sizeScale();
  float tRadius = std::max(.5, n2->radius) * m_rendererBase.sizeScale();
  glm::vec3 axis = tPos - bPos;
  if (glm::length(axis) < std::numeric_limits<float>::epsilon() * 1e2) {
    LOG(WARNING) << "node and parent node too close";
    return;
  }
  // vector perpendicular to axis
  glm::vec3 v1, v2;
  glm::getOrthogonalVectors(axis, v1, v2);

  glm::vec3 p1 = bPos - bRadius * v1 - v2 * bRadius;
  glm::vec3 p2 = bPos - v1 * bRadius + v2 * bRadius;
  glm::vec3 p3 = bPos + v1 * bRadius + v2 * bRadius;
  glm::vec3 p4 = bPos + v1 * bRadius - v2 * bRadius;
  glm::vec3 p5 = tPos - v1 * tRadius - v2 * tRadius;
  glm::vec3 p6 = tPos - v1 * tRadius + v2 * tRadius;
  glm::vec3 p7 = tPos + v1 * tRadius + v2 * tRadius;
  glm::vec3 p8 = tPos + v1 * tRadius - v2 * tRadius;

  lines.push_back(p1);
  lines.push_back(p2);
  lines.push_back(p2);
  lines.push_back(p3);
  lines.push_back(p3);
  lines.push_back(p4);
  lines.push_back(p4);
  lines.push_back(p1);

  lines.push_back(p5);
  lines.push_back(p6);
  lines.push_back(p6);
  lines.push_back(p7);
  lines.push_back(p7);
  lines.push_back(p8);
  lines.push_back(p8);
  lines.push_back(p5);

  lines.push_back(p1);
  lines.push_back(p5);
  lines.push_back(p2);
  lines.push_back(p6);
  lines.push_back(p3);
  lines.push_back(p7);
  lines.push_back(p4);
  lines.push_back(p8);
}

void Z3DSwcFilter::addSelectionBox(
  const SwcTreeNode& tn, std::vector<glm::vec3>& lines)
{
  float radius = std::max(.5, tn->radius) * m_rendererBase.sizeScale();
  glm::vec3 cent = glm::applyMatrix(coordTransform(), glm::vec3(tn->x, tn->y, tn->z));
  float xmin = cent.x - radius;
  float xmax = cent.x + radius;
  float ymin = cent.y - radius;
  float ymax = cent.y + radius;
  float zmin = cent.z - radius;
  float zmax = cent.z + radius;
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

void Z3DSwcFilter::prepareData()
{
  if (!m_dataIsInvalid)
    return;

  decompseSwcTree();

  // get min max of type for colormap
  m_colorMapBranchType.blockSignals(true);
  if (m_allNodeType.empty())
    m_colorMapBranchType.get().reset();
  else
    m_colorMapBranchType.get().reset(m_allNodeType.begin(), m_allNodeType.end(),
                                     glm::col4(0, 0, 255, 255), glm::col4(255, 0, 0, 255));
  m_colorMapBranchType.blockSignals(false);

  deregisterPickingObjects();

  //convert swc to format that glsl can use
  m_axisAndTopRadius.clear();
  m_baseAndBaseRadius.clear();
  m_pointAndRadius.clear();
  m_lines.clear();

  bool checkRadius = false; //m_renderingPrimitive.isSelected("Normal") || m_renderingPrimitive.isSelected("Cylinder");
  for (size_t j = 0; j < m_decompsedNodePairs.size(); j++) {
    const SwcTreeNode& n1 = m_decompsedNodePairs[j].first;
    const SwcTreeNode& n2 = m_decompsedNodePairs[j].second;

    if (checkRadius && n1->radius < std::numeric_limits<double>::epsilon() &&
        n2->radius < std::numeric_limits<double>::epsilon()) {
      checkRadius = false;
      QMessageBox::information(QApplication::activeWindow(),
                               "Reset SWC Rendering Mode",
                               "SWC contains segments with zero radius. "
                                 "The geometrical primitive of SWC rendering "
                                 "will be set to 'Line' to "
                                 "make those segments visible.");
      m_renderingPrimitive.select("Line");
    }

    glm::vec4 baseAndbRadius, axisAndtRadius;
    // make sure base has smaller radius.
    if (n1->radius <= n2->radius) {
      baseAndbRadius = glm::vec4(n1->x, n1->y, n1->z, std::max(.5, n1->radius));
      axisAndtRadius = glm::vec4(n2->x - n1->x,
                                 n2->y - n1->y,
                                 n2->z - n1->z, std::max(.5, n2->radius));
    } else {
      baseAndbRadius = glm::vec4(n2->x, n2->y, n2->z, std::max(.5, n2->radius));
      axisAndtRadius = glm::vec4(n1->x - n2->x,
                                 n1->y - n2->y,
                                 n1->z - n2->z, std::max(.5, n1->radius));
    }
    m_baseAndBaseRadius.push_back(baseAndbRadius);
    m_axisAndTopRadius.push_back(axisAndtRadius);
    m_lines.push_back(baseAndbRadius.xyz());
    m_lines.push_back(glm::vec3(baseAndbRadius.xyz()) + glm::vec3(axisAndtRadius.xyz()));
  }
  for (size_t j = 0; j < m_decomposedNodes.size(); j++) {
    const SwcTreeNode& tn = m_decomposedNodes[j];
    m_pointAndRadius.emplace_back(tn->x, tn->y, tn->z, std::max(.5, tn->radius));
  }

  initializeCutRange();
  initializeRotationCenter();

  m_coneRenderer.setData(&m_baseAndBaseRadius, &m_axisAndTopRadius);
  m_lineRenderer.setData(&m_lines);
  m_sphereRenderer.setData(&m_pointAndRadius);
  m_sphereRendererForCone.setData(&m_pointAndRadius);
  prepareColor();
  adjustWidgets();
  m_dataIsInvalid = false;
}

void Z3DSwcFilter::treeBound(ZSwc* tree, std::vector<double>& res) const
{
  res[0] = res[2] = res[4] = std::numeric_limits<double>::max();
  res[1] = res[3] = res[5] = std::numeric_limits<double>::lowest();
  std::vector<double> nodeBound(6);
  for (ZSwc::Iterator tn = tree->begin(); tn != tree->end(); ++tn) {
    treeNodeBound(tn, nodeBound);
    res[0] = std::min(res[0], nodeBound[0]);
    res[1] = std::max(res[1], nodeBound[1]);
    res[2] = std::min(res[2], nodeBound[2]);
    res[3] = std::max(res[3], nodeBound[3]);
    res[4] = std::min(res[4], nodeBound[4]);
    res[5] = std::max(res[5], nodeBound[5]);
  }
}

void Z3DSwcFilter::treeNodeBound(const SwcTreeNode& tn, std::vector<double>& result) const
{
  glm::vec3 cent = glm::applyMatrix(coordTransform(), glm::vec3(tn->x, tn->y, tn->z));
  result[0] = cent.x - std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale());
  result[1] = cent.x + std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale());
  result[2] = cent.y - std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale());
  result[3] = cent.y + std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale());
  result[4] = cent.z - std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale());
  result[5] = cent.z + std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale());
}

void Z3DSwcFilter::notTransformedTreeBound(ZSwc* tree, std::vector<double>& res) const
{
  res[0] = res[2] = res[4] = std::numeric_limits<double>::max();
  res[1] = res[3] = res[5] = std::numeric_limits<double>::lowest();
  std::vector<double> nodeBound(6);
  for (ZSwc::Iterator tn = tree->begin(); tn != tree->end(); ++tn) {
    notTransformedTreeNodeBound(tn, nodeBound);
    res[0] = std::min(res[0], nodeBound[0]);
    res[1] = std::max(res[1], nodeBound[1]);
    res[2] = std::min(res[2], nodeBound[2]);
    res[3] = std::max(res[3], nodeBound[3]);
    res[4] = std::min(res[4], nodeBound[4]);
    res[5] = std::max(res[5], nodeBound[5]);
  }
}

//void Z3DSwcFilter::updateAxisAlignedBoundBoxImpl()
//{
//  getTreeBound(m_swcTree, m_axisAlignedBoundBox);
//}

void Z3DSwcFilter::updateNotTransformedBoundBoxImpl()
{
  notTransformedTreeBound(m_swcTree, m_notTransformedBoundBox);
}

void Z3DSwcFilter::addSelectionLines()
{
  for (size_t j = 0; j < m_decompsedNodePairs.size(); j++) {
    addSelectionBox(m_decompsedNodePairs[j], m_selectionLines);
  }
  for (size_t j = 0; j < m_decomposedNodes.size(); j++) {
    const SwcTreeNode& tn = m_decomposedNodes[j];
    if (ZSwc::isRoot(tn) && ZSwc::isLeaf(tn)) {
      addSelectionBox(m_decomposedNodes[j], m_selectionLines);
    }
  }
}

void Z3DSwcFilter::notTransformedTreeNodeBound(const SwcTreeNode& tn, std::vector<double>& result) const
{
  result[0] = tn->x - std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale());
  result[1] = tn->x + std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale());
  result[2] = tn->y - std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale());
  result[3] = tn->y + std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale());
  result[4] = tn->z - std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale());
  result[5] = tn->z + std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale());
}

glm::vec4 Z3DSwcFilter::colorByDirection(const SwcTreeNode& n)
{
  Q_UNUSED(n)
  return glm::vec4(0);
}

void Z3DSwcFilter::prepareColor()
{
  m_swcColors1.clear();
  m_swcColors2.clear();
  m_lineColors.clear();
  m_pointColors.clear();

  if (m_colorMode.isSelected("Branch Type") ||
      m_colorMode.isSelected("Colormap Branch Type") ||
      m_colorMode.isSelected("Subclass")) {
    for (size_t j = 0; j < m_decompsedNodePairs.size(); j++) {
      glm::vec4 color1 = colorByType(m_decompsedNodePairs[j].first);
      glm::vec4 color2 = colorByType(m_decompsedNodePairs[j].second);
      if (m_decompsedNodePairs[j].first->radius > m_decompsedNodePairs[j].second->radius) {
        std::swap(color1, color2);
      }

      m_swcColors1.push_back(color1);
      m_swcColors2.push_back(color2);
      m_lineColors.push_back(color1);
      m_lineColors.push_back(color2);
    }
    for (size_t j = 0; j < m_decomposedNodes.size(); j++) {
      m_pointColors.push_back(colorByType(m_decomposedNodes[j]));
    }

  } else if (m_colorMode.isSelected("Single Color")) {
    glm::vec4 color = m_swcTreeColor.get();
    for (size_t j = 0; j < m_decompsedNodePairs.size(); j++) {
      m_swcColors1.push_back(color);
      m_swcColors2.push_back(color);
      m_lineColors.push_back(color);
      m_lineColors.push_back(color);
    }
    for (size_t j = 0; j < m_decomposedNodes.size(); j++) {
      m_pointColors.push_back(color);
    }
  } else if (m_colorMode.isSelected("Topology")) {
    for (size_t j = 0; j < m_decompsedNodePairs.size(); j++) {
      const SwcTreeNode& n1 = m_decompsedNodePairs[j].first;
      const SwcTreeNode& n2 = m_decompsedNodePairs[j].second;
      glm::vec4 color1, color2;
      if (ZSwc::isRoot(n1))
        color1 = m_colorsForDifferentTopology[0]->get();
      else if (ZSwc::isBranchNode(n1))
        color1 = m_colorsForDifferentTopology[1]->get();
      else if (ZSwc::isLeaf(n1))
        color1 = m_colorsForDifferentTopology[2]->get();
      else
        color1 = m_colorsForDifferentTopology[3]->get();
      if (ZSwc::isRoot(n2))
        color2 = m_colorsForDifferentTopology[0]->get();
      else if (ZSwc::isBranchNode(n2))
        color2 = m_colorsForDifferentTopology[1]->get();
      else if (ZSwc::isLeaf(n2))
        color2 = m_colorsForDifferentTopology[2]->get();
      else
        color2 = m_colorsForDifferentTopology[3]->get();
      if (n1->radius > n2->radius) {
        std::swap(color1, color2);
      }

      m_swcColors1.push_back(color1);
      m_swcColors2.push_back(color2);
      m_lineColors.push_back(color1);
      m_lineColors.push_back(color2);
    }
    for (size_t j = 0; j < m_decomposedNodes.size(); j++) {
      const SwcTreeNode& n1 = m_decomposedNodes[j];
      glm::vec4 color1;
      if (ZSwc::isRoot(n1))
        color1 = m_colorsForDifferentTopology[0]->get();
      else if (ZSwc::isBranchNode(n1))
        color1 = m_colorsForDifferentTopology[1]->get();
      else if (ZSwc::isLeaf(n1))
        color1 = m_colorsForDifferentTopology[2]->get();
      else
        color1 = m_colorsForDifferentTopology[3]->get();
      m_pointColors.push_back(color1);
    }
  }

  m_coneRenderer.setDataColors(&m_swcColors1, &m_swcColors2);
  m_lineRenderer.setDataColors(&m_lineColors);
  m_sphereRenderer.setDataColors(&m_pointColors);
  m_sphereRendererForCone.setDataColors(&m_pointColors);
}

void Z3DSwcFilter::adjustWidgets()
{
  m_swcTreeColor.setVisible(m_colorMode.isSelected("Single Color"));

  for (size_t i = 0; i < m_colorsForDifferentType.size(); i++) {
    if (m_allNodeType.find(i) != m_allNodeType.end() && m_colorMode.get() == "Branch Type") {
      m_colorsForDifferentType[i]->setVisible(true);
    } else {
      m_colorsForDifferentType[i]->setVisible(false);
    }
  }
  for (size_t i = 0; i < m_colorsForSubclassType.size(); i++) {
    if (m_colorMode.isSelected("Subclass")) {
      m_colorsForSubclassType[i]->setVisible(true);
    } else {
      m_colorsForSubclassType[i]->setVisible(false);
    }
  }
  for (size_t i = 0; i < m_colorsForDifferentTopology.size(); i++) {
    if (m_colorMode.isSelected("Topology")) {
      m_colorsForDifferentTopology[i]->setVisible(true);
    } else {
      m_colorsForDifferentTopology[i]->setVisible(false);
    }
  }
  if (m_colorMode.isSelected("Colormap Branch Type")) {
    m_colorMapBranchType.setVisible(true);
  } else {
    m_colorMapBranchType.setVisible(false);
  }
}

void Z3DSwcFilter::selectSwc(QMouseEvent* e, int w, int h)
{
  Q_UNUSED(w)
  Q_UNUSED(h)
  if (!m_swcTree)
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
    bool hit = obj == m_swcTree;
    if (isNodeRendering()) {
      hit = m_allNodesSet.find(const_cast<SwcTreeNode*>(static_cast<const SwcTreeNode*>(obj))) != m_allNodesSet.end();
    }
    if (hit) {
      emit objSelected(appending);
      e->accept();
    }
    return;
  }
}

void Z3DSwcFilter::decompseSwcTree()
{
  m_allNodeType.clear();
  m_decompsedNodePairs.clear();
  m_decomposedNodes.clear();
  m_allNodesSet.clear();

  for (ZSwc::Iterator tn = m_swcTree->begin(); tn != m_swcTree->end(); ++tn) {
    m_allNodeType.insert(tn->type);
    m_decomposedNodes.push_back(tn);
    if (!ZSwc::isRoot(tn))
      m_decompsedNodePairs.emplace_back(tn, ZSwc::parent(tn));
  }
  for (size_t i = 0; i < m_decomposedNodes.size(); ++i) {
    m_allNodesSet.insert(&m_decomposedNodes[i]);
  }
}

glm::vec4 Z3DSwcFilter::colorByType(const SwcTreeNode& n)
{
  if (m_colorMode.isSelected("Branch Type")) {
    if (static_cast<size_t>(n->type) + 1 < m_colorsForDifferentType.size()) {
      return m_colorsForDifferentType[n->type]->get();
    } else {
      return m_colorsForDifferentType[m_colorsForDifferentType.size() - 1]->get();
    }
  } else if (m_colorMode.isSelected("Subclass")) {
    if (m_subclassTypeColorMapper.find(n->type) != m_subclassTypeColorMapper.end()) {
      return m_colorsForSubclassType[m_subclassTypeColorMapper[n->type]]->get();
    } else {
      return m_colorsForSubclassType[m_colorsForSubclassType.size() - 1]->get();
    }
  } else  /*if (m_colorMode.get() == "ColorMap Branch Type")*/ {
    return m_colorMapBranchType.get().mappedFColor(n->type);
  }
}

glm::dvec3 Z3DSwcFilter::projectPointOnRay(glm::dvec3 pt, const glm::dvec3& v1, const glm::dvec3& v2)
{
  return v1 + glm::dot(pt - v1, v2 - v1) * (v2 - v1);
}

void Z3DSwcFilter::setColorMode(const std::string& mode)
{
  m_colorMode.select(mode.c_str());
}

} // namespace nim
