#include "z3dmeshfilter.h"

#include "z3dmeshrenderer.h"
#include "zregionannotation.h"
#include "zmesh.h"
#include "zrandom.h"
#include "z3dtextureglowrenderer.h"
#include "zlog.h"
#include <QFileInfo>

namespace nim {

Z3DMeshFilter::Z3DMeshFilter(Z3DGlobalParameters& globalParas, const RegionNode* regionNode, QObject* parent)
  : Z3DGeometryFilter(globalParas, parent)
  , m_triangleListRenderer(m_rendererBase)
  , m_wireframeMode("Wireframe Option")
  , m_wireframeColor("Wireframe Color", glm::vec4(1), glm::vec4(0), glm::vec4(1))
  , m_colorMode("Color Mode")
  , m_singleColorForAllMesh("Mesh Color",
                            glm::vec4(ZRandom::instance().randReal<float>(),
                                      ZRandom::instance().randReal<float>(),
                                      ZRandom::instance().randReal<float>(),
                                      1.f))
  , m_glow("Glow", false)
  , m_glowMode("Glow Mode")
  , m_glowBlurRadius("Glow Blur Radius", 10, 2, 1000)
  , m_glowBlurScale("Glow Blur Scale", 1.f, 1.f, 5.f)
  , m_glowBlurStrength("Glow Blur Strength", .5f, 0.f, 1.f)
  , m_selectMeshEvent("Select Mesh", false)
  , m_pressedMesh(nullptr)
  , m_selectedMeshes(nullptr)
  , m_dataIsInvalid(false)
  , m_regionNode(regionNode)
{
  m_singleColorForAllMesh.setStyle("COLOR");
  connect(&m_singleColorForAllMesh, &ZVec4Parameter::valueChanged, this, &Z3DMeshFilter::prepareColor);

  // Color Mode
  m_colorMode.addOptions("Mesh Color", "Single Color");
  m_colorMode.select("Single Color");
  m_colorMode.setDescription(QStringLiteral(
    "Controls how mesh surface colors are chosen:\n"
    "- 'Single Color' (recommended when you want to control color) applies the 'Mesh Color' parameter below as a solid color for the whole mesh.\n"
    "- 'Mesh Color' reads color attributes embedded in the mesh file (per‑vertex/face). In this mode you cannot override the color with parameters;\n"
    "  if the mesh lacks embedded colors, the fill color falls back to the contained/default color (often black). Choose 'Single Color' to force a specific color."));

  connect(&m_colorMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DMeshFilter::prepareColor);
  connect(&m_colorMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DMeshFilter::adjustWidgets);

  m_wireframeMode.addOptionsWithData(
    std::make_pair("No Wireframe", static_cast<int>(Z3DMeshRenderer::WireframeMode::NoWireframe)),
    std::make_pair("With Wireframe", static_cast<int>(Z3DMeshRenderer::WireframeMode::WithWireframe)),
    std::make_pair("Only Wireframe", static_cast<int>(Z3DMeshRenderer::WireframeMode::OnlyWireframe)));
  m_wireframeMode.select("No Wireframe");
  m_wireframeColor.setStyle("COLOR");
  updateWireframeMode();
  updateWireframeColor();
  connect(&m_wireframeMode, &ZStringIntOptionParameter::valueChanged, this, &Z3DMeshFilter::updateWireframeMode);
  connect(&m_wireframeColor, &ZVec4Parameter::valueChanged, this, &Z3DMeshFilter::updateWireframeColor);

  addParameter(m_colorMode);

  addParameter(m_singleColorForAllMesh);
  m_singleColorForAllMesh.setDescription(
    QStringLiteral("Solid RGBA color used when 'Color Mode' is set to 'Single Color'."));

  addParameter(m_wireframeMode);
  addParameter(m_wireframeColor);
  m_wireframeMode.setDescription(QStringLiteral("Render meshes with or without a wireframe overlay."));
  m_wireframeColor.setDescription(QStringLiteral("Wireframe line color (applies when wireframe is visible)."));

  connect(&m_glow, &ZBoolParameter::valueChanged, this, &Z3DMeshFilter::adjustWidgets);
  addParameter(m_glow);
  // Initialize glow parameter defaults to match previous behavior
  m_glowMode.clearOptions();
  m_glowMode.addOptionsWithData(
    std::make_pair(enumToQString(GlowMode::Additive), static_cast<int>(GlowMode::Additive)),
    std::make_pair(enumToQString(GlowMode::Screen), static_cast<int>(GlowMode::Screen)),
    std::make_pair(enumToQString(GlowMode::Softlight), static_cast<int>(GlowMode::Softlight)),
    std::make_pair(enumToQString(GlowMode::Glowmap), static_cast<int>(GlowMode::Glowmap)));
  m_glowMode.select(enumToQString(GlowMode::Screen));
  m_glowBlurScale.setSingleStep(0.5f);
  addParameter(m_glowMode);
  addParameter(m_glowBlurRadius);
  addParameter(m_glowBlurScale);
  addParameter(m_glowBlurStrength);
  m_glow.setDescription(QStringLiteral("Enable a post-processing glow around bright surfaces."));
  m_glowMode.setDescription(QStringLiteral("Blend mode for the glow pass."));
  m_glowBlurRadius.setDescription(QStringLiteral("Radius of the glow blur kernel (pixels)."));
  m_glowBlurScale.setDescription(QStringLiteral("Scale factor applied to the glow blur radius."));
  m_glowBlurStrength.setDescription(QStringLiteral("Opacity applied when compositing the glow."));

  m_selectMeshEvent.listenTo("select mesh", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonPress);
  m_selectMeshEvent.listenTo("select mesh", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonRelease);
  m_selectMeshEvent.listenTo("select mesh", Qt::LeftButton, Qt::NoModifier, QEvent::MouseButtonDblClick);
  m_selectMeshEvent.listenTo("select mesh", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonDblClick);
  m_selectMeshEvent.listenTo("append select mesh", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonPress);
  m_selectMeshEvent.listenTo("append select mesh", Qt::LeftButton, Qt::ControlModifier, QEvent::MouseButtonRelease);
  connect(&m_selectMeshEvent, &ZEventListenerParameter::mouseEventTriggered, this, &Z3DMeshFilter::selectMesh);
  addEventListener(m_selectMeshEvent);

  adjustWidgets();
}

QString Z3DMeshFilter::regionName() const
{
  return m_regionNode ? m_regionNode->name : QString();
}

double Z3DMeshFilter::process(Z3DEye)
{
  syncRendererState();

  if (m_dataIsInvalid) {
    prepareData();
  }

  return 1.;
}

void Z3DMeshFilter::setData(std::vector<ZMesh*>* meshList)
{
  m_origMeshList.clear();
  if (meshList) {
    m_origMeshList = *meshList;
    LOG(INFO) << className() << " read " << m_origMeshList.size() << " meshes.";
  }
  getVisibleData();
  m_dataIsInvalid = true;
  invalidateResult();

  updateBoundBox();
  initializeRotationCenterIfDefault();
}

bool Z3DMeshFilter::isReady(Z3DEye eye) const
{
  return Z3DGeometryFilter::isReady(eye) && m_visible.get() && !m_origMeshList.empty();
}

// namespace {

// bool compareParameterName(const ZParameter *p1, const ZParameter *p2)
//{
//   QString n1 = p1->getName().mid(5); // "Mesh "
//   QString n2 = p2->getName().mid(5);
//   n1.remove(n1.size()-6, 6); //" Color"
//   n2.remove(n2.size()-6, 6);
//   return n1.toInt() < n2.toInt();
// }

//}

std::shared_ptr<ZWidgetsGroup> Z3DMeshFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Mesh", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_stayOnTop, 1);
    m_widgetsGroup->addChild(m_colorMode, 1);
    m_widgetsGroup->addChild(m_singleColorForAllMesh, 1);
    m_widgetsGroup->addChild(m_wireframeMode, 1);
    m_widgetsGroup->addChild(m_wireframeColor, 1);

    m_widgetsGroup->addChild(m_rendererParameters.coordTransform, 2);
    m_widgetsGroup->addChild(m_rendererParameters.opacity, 5);
    m_widgetsGroup->addChild(m_rendererParameters.materialAmbient, 7);
    m_widgetsGroup->addChild(m_rendererParameters.materialSpecular, 7);
    m_widgetsGroup->addChild(m_rendererParameters.materialShininess, 7);

    m_widgetsGroup->addChild(m_glow, 5);
    m_widgetsGroup->addChild(m_glowMode, 5);
    m_widgetsGroup->addChild(m_glowBlurRadius, 5);
    m_widgetsGroup->addChild(m_glowBlurScale, 5);
    m_widgetsGroup->addChild(m_glowBlurStrength, 5);

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

std::shared_ptr<ZWidgetsGroup> Z3DMeshFilter::widgetsGroupForAnnotationFilter()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("Mesh", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_singleColorForAllMesh, 1);
    m_widgetsGroup->addChild(m_wireframeMode, 1);
    m_widgetsGroup->addChild(m_wireframeColor, 1);

    m_widgetsGroup->addChild(m_rendererParameters.opacity, 5);
    m_widgetsGroup->addChild(m_rendererParameters.materialAmbient, 5);
    m_widgetsGroup->addChild(m_rendererParameters.materialSpecular, 5);
    m_widgetsGroup->addChild(m_rendererParameters.materialShininess, 5);
    m_widgetsGroup->addChild(m_xCut, 5);
    m_widgetsGroup->addChild(m_yCut, 5);
    m_widgetsGroup->addChild(m_zCut, 5);
    m_widgetsGroup->addChild(m_boundBoxMode, 5);
    m_widgetsGroup->addChild(m_boundBoxLineWidth, 5);
    m_widgetsGroup->addChild(m_boundBoxLineColor, 5);
    m_widgetsGroup->setBasicAdvancedCutoff(5);
  }
  return m_widgetsGroup;
}

void Z3DMeshFilter::renderOpaque(Z3DEye eye)
{
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    m_rendererBase.renderVulkan(eye, m_triangleListRenderer);
  } else {
    m_rendererBase.render(eye, m_triangleListRenderer);
  }
  renderBoundBox(eye);
}

void Z3DMeshFilter::renderTransparent(Z3DEye eye)
{
  if (m_glow.get()) {
    // Compositor owns glow composition; only render bound box here
    renderBoundBox(eye);
  } else {
    if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
      m_rendererBase.renderVulkan(eye, m_triangleListRenderer);
    } else {
      m_rendererBase.render(eye, m_triangleListRenderer);
    }
    renderBoundBox(eye);
  }
}

void Z3DMeshFilter::renderPicking(Z3DEye eye)
{
  if (!m_pickingObjectsRegistered) {
    registerPickingObjects();
  }
  if (m_rendererBase.activeBackend() == RenderBackend::Vulkan) {
    m_rendererBase.renderPickingVulkan(eye, m_triangleListRenderer);
  } else {
    m_rendererBase.renderPicking(eye, m_triangleListRenderer);
  }
}

void Z3DMeshFilter::prepareData()
{
  if (!m_dataIsInvalid) {
    return;
  }

  deregisterPickingObjects();

  m_triangleListRenderer.setData(&m_meshList);
  prepareColor();
  adjustWidgets();
  m_dataIsInvalid = false;
}

void Z3DMeshFilter::registerPickingObjects()
{
  if (!m_pickingObjectsRegistered) {
    for (auto mesh : m_meshList) {
      pickingManager().registerObject(mesh);
    }
    m_registeredMeshList = m_meshList;
    m_meshPickingColors.clear();
    for (auto mesh : m_meshList) {
      glm::col4 pickingColor = pickingManager().colorOfObject(mesh);
      glm::vec4 fPickingColor(pickingColor[0] / 255.f,
                              pickingColor[1] / 255.f,
                              pickingColor[2] / 255.f,
                              pickingColor[3] / 255.f);
      m_meshPickingColors.push_back(fPickingColor);
    }
    m_triangleListRenderer.setDataPickingColors(&m_meshPickingColors);
  }

  m_pickingObjectsRegistered = true;
}

void Z3DMeshFilter::deregisterPickingObjects()
{
  if (m_pickingObjectsRegistered) {
    for (auto mesh : m_registeredMeshList) {
      pickingManager().deregisterObject(mesh);
    }
    m_registeredMeshList.clear();
  }

  m_pickingObjectsRegistered = false;
}

ZBBox<glm::dvec3> Z3DMeshFilter::meshBound(ZMesh* p)
{
  auto it = m_meshBoundboxMapper.find(p);
  if (it != m_meshBoundboxMapper.end()) {
    ZBBox<glm::dvec3> result = it->second;
    //    result[0] *= getCoordTransform().x;
    //    result[1] *= getCoordTransform().x;
    //    result[2] *= getCoordTransform().y;
    //    result[3] *= getCoordTransform().y;
    //    result[4] *= getCoordTransform().z;
    //    result[5] *= getCoordTransform().z;
    return result;
  } else {
    ZBBox<glm::dvec3> result = p->boundBox(coordTransform());
    m_meshBoundboxMapper[p] = result;
    //    result[0] *= getCoordTransform().x;
    //    result[1] *= getCoordTransform().x;
    //    result[2] *= getCoordTransform().y;
    //    result[3] *= getCoordTransform().y;
    //    result[4] *= getCoordTransform().z;
    //    result[5] *= getCoordTransform().z;
    return result;
  }
}

void Z3DMeshFilter::updateNotTransformedBoundBoxImpl()
{
  m_notTransformedBoundBox.reset();
  for (auto& mesh : m_origMeshList) {
    m_notTransformedBoundBox.expand(mesh->boundBox());
  }
}

void Z3DMeshFilter::prepareColor()
{
  m_meshColors.clear();

  if (m_colorMode.isSelected("Single Color")) {
    for (size_t i = 0; i < m_meshList.size(); ++i) {
      m_meshColors.push_back(m_singleColorForAllMesh.get());
    }
    m_triangleListRenderer.setDataColors(&m_meshColors);
    m_triangleListRenderer.setColorSource(MeshColorSource::CustomColor);
  } else if (m_colorMode.isSelected("Mesh Color")) {
    m_triangleListRenderer.setColorSource(MeshColorSource::MeshColor);
  }
}

void Z3DMeshFilter::updateWireframeMode()
{
  m_triangleListRenderer.setWireframeMode(
    static_cast<Z3DMeshRenderer::WireframeMode>(m_wireframeMode.associatedData()));
  adjustWidgets();
}

void Z3DMeshFilter::updateWireframeColor()
{
  m_triangleListRenderer.setWireframeColor(m_wireframeColor.get());
}

void Z3DMeshFilter::adjustWidgets()
{
  m_singleColorForAllMesh.setVisible(m_colorMode.isSelected("Single Color"));

  m_glowMode.setVisible(m_glow.get());
  m_glowBlurRadius.setVisible(m_glow.get());
  m_glowBlurScale.setVisible(m_glow.get());
  m_glowBlurStrength.setVisible(m_glow.get());
  m_wireframeColor.setVisible(!m_wireframeMode.isSelected("No Wireframe"));
}

void Z3DMeshFilter::selectMesh(QMouseEvent* e, int /*w*/, int /*h*/)
{
  if (m_meshList.empty()) {
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
    bool hit = contains(m_meshList, static_cast<const ZMesh*>(obj));
    if (hit) {
      Q_EMIT objSelected(appending);
      e->accept();
    }
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
    for (auto m : m_meshList) {
      if (m == obj) {
        m_pressedMesh = m;
        break;
      }
    }
    return;
  }

  if (e->type() == QEvent::MouseButtonRelease) {
    if (std::abs(e->position().x() - m_startCoord.x) < 2 && std::abs(m_startCoord.y - e->position().y()) < 2) {
      if (e->modifiers() == Qt::ControlModifier) {
        Q_EMIT meshSelected(m_pressedMesh, true);
      } else {
        Q_EMIT meshSelected(m_pressedMesh, false);
      }
      if (m_pressedMesh) {
        e->accept();
      }
    }
    m_pressedMesh = nullptr;
  }
}

void Z3DMeshFilter::onApplyTransform()
{
  VLOG(1) << m_rendererParameters.coordTransform.get();
}

void Z3DMeshFilter::updateMeshVisibleState()
{
  getVisibleData();
  m_dataIsInvalid = true;
  invalidateResult();
}

void Z3DMeshFilter::getVisibleData()
{
  m_meshList = m_origMeshList;
}

} // namespace nim
