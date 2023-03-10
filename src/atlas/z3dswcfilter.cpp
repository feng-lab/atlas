#include "z3dswcfilter.h"

#include "zrandom.h"
#include "zlog.h"
#include <QMessageBox>
#include <QApplication>
#include <iostream>

namespace nim {

Z3DSwcFilter::Z3DSwcFilter(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DGeometryFilter(globalParas, parent)
  , m_lineRenderer(m_rendererBase)
  , m_coneRenderer(m_rendererBase)
  , m_sphereRenderer(m_rendererBase)
  , m_sphereRendererForCone(m_rendererBase)
  , m_renderingPrimitive("Rendering Mode")
  , m_selectSwcEvent("Select Swc", false)
  , m_deleteSelectedNodesEvent("Delete Selected Swc Nodes", true)
  , m_contextMenuEvent("Context Menu", false)
  , m_dataIsInvalid(false)
  , m_interactionMode(InteractionMode::Select)
{
  // rendering primitive
  m_renderingPrimitive.addOptions("Normal", "Line", "Sphere", "Cylinder");
  m_renderingPrimitive.select("Normal");
  connect(&m_renderingPrimitive, &ZStringIntOptionParameter::valueChanged, this, &Z3DSwcFilter::updateBoundBox);
  addParameter(m_renderingPrimitive);

  connect(&m_swcColorParameters.colorMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DSwcFilter::prepareColor);
  addParameter(m_swcColorParameters.colorMode);

  connect(&m_swcColorParameters.swcTreeColor, &ZVec4Parameter::valueChanged, this, &Z3DSwcFilter::prepareColor);
  addParameter(m_swcColorParameters.swcTreeColor);

  for (const auto& color : m_swcColorParameters.colorsForDifferentType) {
    connect(color.get(), &ZVec4Parameter::valueChanged, this, &Z3DSwcFilter::prepareColor);
    addParameter(*color);
  }

  for (const auto& color : m_swcColorParameters.colorsForDifferentTopology) {
    connect(color.get(), &ZVec4Parameter::valueChanged, this, &Z3DSwcFilter::prepareColor);
    addParameter(*color);
  }

  for (const auto& color : m_swcColorParameters.colorsForSubclassType) {
    connect(color.get(), &ZVec4Parameter::valueChanged, this, &Z3DSwcFilter::prepareColor);
    addParameter(*color);
  }

  addParameter(m_swcColorParameters.colorMapBranchType);
  connect(&m_swcColorParameters.colorMapBranchType,
          &ZColorMapParameter::valueChanged,
          this,
          &Z3DSwcFilter::prepareColor);

  m_selectSwcEvent.listenTo("select swc", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
  m_selectSwcEvent.listenTo("select swc", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonRelease);
  m_selectSwcEvent.listenTo("select swc", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonDblClick);
  m_selectSwcEvent.listenTo("select swc", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonDblClick);
  /*
  m_selectSwcEvent.listenTo("select swc", Qt::RightButton,
                             Qt::NoModifier, QEvent::MouseButtonPress);
  m_selectSwcEvent.listenTo("select swc", Qt::RightButton, Qt::NoModifier,
                             QEvent::MouseButtonRelease);
                             */
  m_selectSwcEvent.listenTo("extend select swc", Qt::LeftButton, Qt::ShiftModifier, QEvent::MouseButtonPress);
  m_selectSwcEvent.listenTo("extend select swc", Qt::LeftButton, Qt::ShiftModifier, QEvent::MouseButtonRelease);

  m_selectSwcEvent.listenTo("append select swc", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonPress);
  m_selectSwcEvent.listenTo("append select swc", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonRelease);

  /*
  m_selectSwcEvent.listenTo("append select swc", Qt::RightButton,
                             Qt::ControlModifier, QEvent::MouseButtonPress);
  m_selectSwcEvent.listenTo("append select swc", Qt::RightButton,
                             Qt::ControlModifier, QEvent::MouseButtonRelease);
*/
  connect(&m_selectSwcEvent, &ZEventListenerParameter::mouseEventTriggered, this, &Z3DSwcFilter::selectSwc);
  addEventListener(m_selectSwcEvent);

  m_deleteSelectedNodesEvent.listenTo("delete", Qt::Key_Delete, Qt::NoModifier, QEvent::KeyPress);
  m_deleteSelectedNodesEvent.listenTo("backspace", Qt::Key_Backspace, Qt::NoModifier, QEvent::KeyPress);
  connect(&m_deleteSelectedNodesEvent,
          &ZEventListenerParameter::keyEventTriggered,
          this,
          &Z3DSwcFilter::deleteSelectedNodes);
  addEventListener(m_deleteSelectedNodesEvent);

  m_contextMenuEvent.listenToContextMenuEvent();
  connect(&m_contextMenuEvent,
          &ZEventListenerParameter::contextMenuEventTriggered,
          this,
          &Z3DSwcFilter::contextMenuEvent);
  addEventListener(m_contextMenuEvent);
}

double Z3DSwcFilter::process(Z3DEye /*unused*/)
{
  if (m_dataIsInvalid) {
    prepareData();
  }
  return 1.;
}

void Z3DSwcFilter::registerPickingObjects()
{
  if (!m_pickingObjectsRegistered) {
    pickingManager().registerObject(m_swcPack);
    for (auto node : m_swcPack->allNodesSet()) {
      pickingManager().registerObject(node);
    }
    m_registeredSwcPack = m_swcPack;
    m_swcPickingColors.clear();
    m_linePickingColors.clear();
    m_pointPickingColors.clear();
    glm::col4 pickingColor = pickingManager().colorOfObject(m_swcPack);
    glm::vec4 fPickingColor(pickingColor[0] / 255.f,
                            pickingColor[1] / 255.f,
                            pickingColor[2] / 255.f,
                            pickingColor[3] / 255.f);
    for (size_t j = 0; j < m_swcPack->decompsedNodePairs().size(); ++j) {
      m_swcPickingColors.push_back(fPickingColor);
      m_linePickingColors.push_back(fPickingColor);
      m_linePickingColors.push_back(fPickingColor);
    }
    m_sphereForConePickingColors = m_swcPickingColors;
    m_sphereForConePickingColors.push_back(fPickingColor);
    for (auto& tn : m_swcPack->decomposedNodes()) {
      pickingColor = pickingManager().colorOfObject(&tn);
      fPickingColor =
        glm::vec4(pickingColor[0] / 255.f, pickingColor[1] / 255.f, pickingColor[2] / 255.f, pickingColor[3] / 255.f);
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
    if (m_registeredSwcPack) {
      pickingManager().deregisterObject(m_registeredSwcPack);
    }
    for (auto node : m_swcPack->allNodesSet()) {
      pickingManager().deregisterObject(node);
    }
    m_registeredSwcPack = nullptr;
  }

  m_pickingObjectsRegistered = false;
}

void Z3DSwcFilter::setData(ZSwcPack& swcPack)
{
  m_swcPack = &swcPack;
  m_swcColorParameters.setData(m_swcPack);
  updateData();

  connect(m_swcPack, &ZSwcPack::selectionChanged, this, &Z3DSwcFilter::invalidateResult);
  connect(this, &Z3DSwcFilter::treeNodeSelected, m_swcPack, &ZSwcPack::onTreeNodeSelected);
  connect(m_swcPack, &ZSwcPack::swcChanged, this, &Z3DSwcFilter::updateData);
  connect(m_swcPack, &ZSwcPack::lockedStateChanged, this, &Z3DSwcFilter::invalidateResult);
}

bool Z3DSwcFilter::isReady(Z3DEye eye) const
{
  return Z3DGeometryFilter::isReady(eye) && m_visible.get() && m_swcPack;
}

std::shared_ptr<ZWidgetsGroup> Z3DSwcFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Swc", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_stayOnTop, 1);
    m_widgetsGroup->addChild(m_renderingPrimitive, 1);
    m_widgetsGroup->addChild(m_swcColorParameters.colorMode, 1);
    m_widgetsGroup->addChild(m_swcColorParameters.swcTreeColor, 1);

    for (const auto& color : m_swcColorParameters.colorsForDifferentType) {
      m_widgetsGroup->addChild(*color, 1);
    }
    for (const auto& color : m_swcColorParameters.colorsForSubclassType) {
      m_widgetsGroup->addChild(*color, 1);
    }
    for (const auto& color : m_swcColorParameters.colorsForDifferentTopology) {
      m_widgetsGroup->addChild(*color, 1);
    }
    m_widgetsGroup->addChild(m_swcColorParameters.colorMapBranchType, 1);

    const std::vector<ZParameter*>& paras = m_rendererBase.parameters();
    for (auto para : paras) {
      if (para->name() == "Coord Transform") {
        m_widgetsGroup->addChild(*para, 5);
      } else if (para->name() == "Size Scale") {
        m_widgetsGroup->addChild(*para, 2);
      } else if (para->name() == "Rendering Method") {
        m_widgetsGroup->addChild(*para, 4);
      } else if (para->name() == "Opacity") {
        m_widgetsGroup->addChild(*para, 3);
      } else {
        m_widgetsGroup->addChild(*para, 7);
      }
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
  } else /* (m_renderingPrimitive.get() == "Sphere") */ {
    m_rendererBase.render(eye, m_lineRenderer, m_sphereRenderer);
  }
  renderBoundBox(eye);
  renderEditingSelectionBox(eye);
}

void Z3DSwcFilter::renderTransparent(Z3DEye eye)
{
  if (m_renderingPrimitive.isSelected("Normal")) {
    m_rendererBase.render(eye, m_sphereRendererForCone, m_coneRenderer);
  } else if (m_renderingPrimitive.isSelected("Cylinder")) {
    m_rendererBase.render(eye, m_coneRenderer);
  } else if (m_renderingPrimitive.isSelected("Line")) {
    m_rendererBase.render(eye, m_lineRenderer);
  } else /* (m_renderingPrimitive.get() == "Sphere") */ {
    m_rendererBase.render(eye, m_lineRenderer, m_sphereRenderer);
  }
  renderBoundBox(eye);
  renderEditingSelectionBox(eye);
}

void Z3DSwcFilter::renderPicking(Z3DEye eye)
{
  if (!m_pickingObjectsRegistered) {
    registerPickingObjects();
  }

  if (m_renderingPrimitive.isSelected("Normal")) {
    m_rendererBase.renderPicking(eye, m_coneRenderer, m_sphereRendererForCone);
  } else if (m_renderingPrimitive.isSelected("Cylinder")) {
    m_rendererBase.renderPicking(eye, m_coneRenderer);
  } else if (m_renderingPrimitive.isSelected("Line")) {
    m_rendererBase.renderPicking(eye, m_lineRenderer);
  } else /* (m_renderingPrimitive.get() == "Sphere") */ {
    m_rendererBase.renderPicking(eye, m_lineRenderer, m_sphereRenderer);
  }
}

void Z3DSwcFilter::addSelectionBox(const std::pair<ZSwc::ConstSwcTreeNode, ZSwc::ConstSwcTreeNode>& nodePair,
                                   std::vector<glm::vec3>& lines)
{
  const ZSwc::ConstSwcTreeNode& n1 = nodePair.first;
  const ZSwc::ConstSwcTreeNode& n2 = nodePair.second;
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

void Z3DSwcFilter::addSelectionBox(const ZSwc::ConstSwcTreeNode& tn, std::vector<glm::vec3>& lines)
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
  if (!m_dataIsInvalid) {
    return;
  }

  deregisterPickingObjects();

  // convert swc to format that glsl can use
  m_axisAndTopRadius.clear();
  m_baseAndBaseRadius.clear();
  m_pointAndRadius.clear();
  m_lines.clear();

  bool checkRadius = false; // m_renderingPrimitive.isSelected("Normal") || m_renderingPrimitive.isSelected("Cylinder");
  for (const auto& [n1, n2] : m_swcPack->decompsedNodePairs()) {
    if (checkRadius && n1->radius < std::numeric_limits<double>::epsilon() &&
        n2->radius < std::numeric_limits<double>::epsilon()) {
      checkRadius = false;
      QMessageBox::information(QApplication::activeWindow(),
                               QApplication::applicationName(),
                               "Reset SWC Rendering Mode.\n"
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
      axisAndtRadius = glm::vec4(n2->x - n1->x, n2->y - n1->y, n2->z - n1->z, std::max(.5, n2->radius));
    } else {
      baseAndbRadius = glm::vec4(n2->x, n2->y, n2->z, std::max(.5, n2->radius));
      axisAndtRadius = glm::vec4(n1->x - n2->x, n1->y - n2->y, n1->z - n2->z, std::max(.5, n1->radius));
    }
    m_baseAndBaseRadius.push_back(baseAndbRadius);
    m_axisAndTopRadius.push_back(axisAndtRadius);
    m_lines.push_back(baseAndbRadius.xyz());
    m_lines.push_back(glm::vec3(baseAndbRadius.xyz()) + glm::vec3(axisAndtRadius.xyz()));
  }
  for (auto& tn : m_swcPack->decomposedNodes()) {
    m_pointAndRadius.emplace_back(tn->x, tn->y, tn->z, std::max(.5, tn->radius));
  }

  initializeCutRange();
  initializeRotationCenter();

  m_coneRenderer.setData(&m_baseAndBaseRadius, &m_axisAndTopRadius);
  m_lineRenderer.setData(&m_lines);
  m_sphereRenderer.setData(&m_pointAndRadius);
  m_sphereRendererForCone.setData(&m_pointAndRadius);
  prepareColor();
  m_dataIsInvalid = false;
}

void Z3DSwcFilter::treeBound(ZSwcPack* swcPack, ZBBox<glm::dvec3>& res) const
{
  res.reset();
  ZBBox<glm::dvec3> nodeBound;
  for (auto tn = swcPack->swc().begin(); tn != swcPack->swc().end(); ++tn) {
    treeNodeBound(tn, nodeBound);
    res.expand(nodeBound);
  }
}

void Z3DSwcFilter::updateData()
{
  m_dataIsInvalid = true;
  invalidateResult();

  updateBoundBox();
}

void Z3DSwcFilter::treeNodeBound(const ZSwc::ConstSwcTreeNode& tn, ZBBox<glm::dvec3>& result) const
{
  glm::dvec3 cent = glm::dvec3(glm::applyMatrix(coordTransform(), glm::vec3(tn->x, tn->y, tn->z)));
  result.setMinCorner(cent - std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale()));
  result.setMaxCorner(cent + std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale()));
}

void Z3DSwcFilter::notTransformedTreeBound(ZSwcPack* swcPack, ZBBox<glm::dvec3>& res) const
{
  res.reset();
  ZBBox<glm::dvec3> nodeBound;
  for (auto tn = swcPack->swc().begin(); tn != swcPack->swc().end(); ++tn) {
    notTransformedTreeNodeBound(tn, nodeBound);
    res.expand(nodeBound);
  }
}

// void Z3DSwcFilter::updateAxisAlignedBoundBoxImpl()
//{
//   getTreeBound(m_swcTree, m_axisAlignedBoundBox);
// }

void Z3DSwcFilter::updateNotTransformedBoundBoxImpl()
{
  notTransformedTreeBound(m_swcPack, m_notTransformedBoundBox);
}

void Z3DSwcFilter::addSelectionLines()
{
  for (auto& nodePair : m_swcPack->decompsedNodePairs()) {
    addSelectionBox(nodePair, m_selectionLines);
  }
  for (auto& tn : m_swcPack->decomposedNodes()) {
    if (ZSwc::isRoot(tn) && ZSwc::isLeaf(tn)) {
      addSelectionBox(tn, m_selectionLines);
    }
  }
}

void Z3DSwcFilter::addEditingSelectionLines()
{
  if (m_swcPack->isLocked()) {
    return;
  }
  ZBBox<glm::dvec3> boundBox;
  for (const auto& tn : m_swcPack->selectedNodes()) {
    addSelectionBox(tn, m_editingSelectionLines);
  }
}

void Z3DSwcFilter::notTransformedTreeNodeBound(const ZSwc::ConstSwcTreeNode& tn, ZBBox<glm::dvec3>& result) const
{
  glm::dvec3 cent(tn->x, tn->y, tn->z);
  result.setMinCorner(cent - std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale()));
  result.setMaxCorner(cent + std::max(.5, tn->radius) * (m_renderingPrimitive.isSelected("Line") ? 1 : sizeScale()));
}

void Z3DSwcFilter::prepareColor()
{
  m_swcColors1.clear();
  m_swcColors2.clear();
  m_lineColors.clear();
  m_pointColors.clear();

#if 1
  if (m_swcColorParameters.colorMode.isSelected("Branch Type") ||
      m_swcColorParameters.colorMode.isSelected("Colormap Branch Type") ||
      m_swcColorParameters.colorMode.isSelected("Subclass")) {
    for (auto& [n1, n2] : m_swcPack->decompsedNodePairs()) {
      glm::vec4 color1 = m_swcColorParameters.colorByType(n1);
      glm::vec4 color2 = m_swcColorParameters.colorByType(n2);
      if (n1->radius > n2->radius) {
        std::swap(color1, color2);
      }

      m_swcColors1.push_back(color1);
      m_swcColors2.push_back(color2);
      m_lineColors.push_back(color1);
      m_lineColors.push_back(color2);
    }
    for (auto& decomposedNode : m_swcPack->decomposedNodes()) {
      m_pointColors.push_back(m_swcColorParameters.colorByType(decomposedNode));
    }
  } else if (m_swcColorParameters.colorMode.isSelected("Single Color")) {
    glm::vec4 color = m_swcColorParameters.swcTreeColor.get();
    for (size_t j = 0; j < m_swcPack->decompsedNodePairs().size(); ++j) {
      m_swcColors1.push_back(color);
      m_swcColors2.push_back(color);
      m_lineColors.push_back(color);
      m_lineColors.push_back(color);
    }
    for (size_t j = 0; j < m_swcPack->decomposedNodes().size(); ++j) {
      m_pointColors.push_back(color);
    }
  } else if (m_swcColorParameters.colorMode.isSelected("Topology")) {
    for (const auto& [n1, n2] : m_swcPack->decompsedNodePairs()) {
      glm::vec4 color1, color2;
      if (ZSwc::isRoot(n1)) {
        color1 = m_swcColorParameters.colorsForDifferentTopology[0]->get();
      } else if (ZSwc::isBranchNode(n1)) {
        color1 = m_swcColorParameters.colorsForDifferentTopology[1]->get();
      } else if (ZSwc::isLeaf(n1)) {
        color1 = m_swcColorParameters.colorsForDifferentTopology[2]->get();
      } else {
        color1 = m_swcColorParameters.colorsForDifferentTopology[3]->get();
      }
      if (ZSwc::isRoot(n2)) {
        color2 = m_swcColorParameters.colorsForDifferentTopology[0]->get();
      } else if (ZSwc::isBranchNode(n2)) {
        color2 = m_swcColorParameters.colorsForDifferentTopology[1]->get();
      } else if (ZSwc::isLeaf(n2)) {
        color2 = m_swcColorParameters.colorsForDifferentTopology[2]->get();
      } else {
        color2 = m_swcColorParameters.colorsForDifferentTopology[3]->get();
      }
      if (n1->radius > n2->radius) {
        std::swap(color1, color2);
      }

      m_swcColors1.push_back(color1);
      m_swcColors2.push_back(color2);
      m_lineColors.push_back(color1);
      m_lineColors.push_back(color2);
    }
    for (auto& n1 : m_swcPack->decomposedNodes()) {
      glm::vec4 color1;
      if (ZSwc::isRoot(n1)) {
        color1 = m_swcColorParameters.colorsForDifferentTopology[0]->get();
      } else if (ZSwc::isBranchNode(n1)) {
        color1 = m_swcColorParameters.colorsForDifferentTopology[1]->get();
      } else if (ZSwc::isLeaf(n1)) {
        color1 = m_swcColorParameters.colorsForDifferentTopology[2]->get();
      } else {
        color1 = m_swcColorParameters.colorsForDifferentTopology[3]->get();
      }
      m_pointColors.push_back(color1);
    }
  }
#else
  for (auto& [n1, n2] : m_swcPack->decompsedNodePairs()) {
    glm::vec4 color1 = m_swcColorParameters.colorOfNode(n1);
    glm::vec4 color2 = m_swcColorParameters.colorOfNode(n2);
    if (n1->radius > n2->radius) {
      std::swap(color1, color2);
    }

    m_swcColors1.push_back(color1);
    m_swcColors2.push_back(color2);
    m_lineColors.push_back(color1);
    m_lineColors.push_back(color2);
  }
  for (auto& decomposedNode : m_swcPack->decomposedNodes()) {
    m_pointColors.push_back(m_swcColorParameters.colorOfNode(decomposedNode));
  }
#endif

  m_coneRenderer.setDataColors(&m_swcColors1, &m_swcColors2);
  m_lineRenderer.setDataColors(&m_lineColors);
  m_sphereRenderer.setDataColors(&m_pointColors);
  m_sphereRendererForCone.setDataColors(&m_pointColors);
}

void Z3DSwcFilter::selectSwc(QMouseEvent* e, int /*w*/, int /*h*/)
{
  if (!m_swcPack) {
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
    bool hit = obj == m_swcPack;
    if (isNodeRendering()) {
      hit = m_swcPack->allNodesSet().find(static_cast<const ZSwc::SwcTreeNode*>(obj)) != m_swcPack->allNodesSet().end();
    }
    if (hit) {
      Q_EMIT objSelected(appending);
      e->accept();
    }
    return;
  }

  if (m_swcPack->isLocked()) {
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
    auto it = m_swcPack->allNodesSet().find(static_cast<const ZSwc::SwcTreeNode*>(obj));
    m_pressedSwcTreeNode = it != m_swcPack->allNodesSet().end() ? *it : nullptr;
    return;
  }

  if (e->type() == QEvent::MouseButtonRelease) {
    if (std::abs(e->position().x() - m_startCoord.x) < 2 && std::abs(m_startCoord.y - e->position().y()) < 2) {
      Q_EMIT treeNodeSelected(m_pressedSwcTreeNode,
                              e->modifiers() == Qt::ControlModifier,
                              e->modifiers() == Qt::ShiftModifier);
      if (m_pressedSwcTreeNode) {
        e->accept();
      }
    }
    m_pressedSwcTreeNode = nullptr;
  }
}

void Z3DSwcFilter::contextMenuEvent(QContextMenuEvent* e, int, int)
{
  if (m_swcPack->isLocked()) {
    return;
  }

  if (isVisible() && !isSelected() && m_swcPack && !m_swcPack->selectedNodes().empty()) {
    const void* obj = pickingManager().objectAtWidgetPos(glm::ivec2(e->x(), e->y()));
    if (!obj) {
      return;
    }
    auto nodeObj = *static_cast<const ZSwc::SwcTreeNode*>(obj);

    bool hasSelectedNodesMouse = false;
    for (auto p : m_swcPack->selectedNodes()) {
      if (p == nodeObj) {
        hasSelectedNodesMouse = true;
        break;
      }
    }
    if (!hasSelectedNodesMouse) {
      return;
    }

    m_swcPack->contextMenu().popup(e->globalPos());
  }
}

glm::dvec3 Z3DSwcFilter::projectPointOnRay(const glm::dvec3& pt, const glm::dvec3& v1, const glm::dvec3& v2)
{
  return v1 + glm::dot(pt - v1, v2 - v1) * (v2 - v1);
}

void Z3DSwcFilter::deleteSelectedNodes()
{
  if (m_swcPack->isLocked()) {
    return;
  }
  m_swcPack->deleteSelectedNodes();
}

} // namespace nim
