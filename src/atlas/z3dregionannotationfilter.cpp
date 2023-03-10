#include "z3dregionannotationfilter.h"

#include "zregionannotationviewsettingtreeview.h"
#include "zmesh.h"
#include "zrandom.h"
#include <QFileInfo>
#include <QPushButton>

namespace nim {

Z3DRegionAnnotationFilter::Z3DRegionAnnotationFilter(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DGeometryFilter(globalParas, parent)
{
  // addParameter(m_visible);

  connect(&m_visible, &ZBoolParameter::boolChanged, this, &Z3DRegionAnnotationFilter::visibleChanged);

  m_numParas = m_parameters.size();
}

double Z3DRegionAnnotationFilter::process(Z3DEye eye)
{
  initializeCutRange();
  initializeRotationCenter();
  for (const auto& idFilter : m_idToMeshFilters) {
    idFilter.second->process(eye);
  }
  return 1.;
}

void Z3DRegionAnnotationFilter::setData(ZRegionAnnotationPack& rap)
{
  m_regionAnnotationPack = &rap;
  allMeshChanged();
  connect(&m_regionAnnotationPack->regionAnnotation(),
          &ZRegionAnnotation::allMeshChanged,
          this,
          &Z3DRegionAnnotationFilter::allMeshChanged);
}

bool Z3DRegionAnnotationFilter::isReady(Z3DEye eye) const
{
  return Z3DGeometryFilter::isReady(eye) && m_regionAnnotationPack;
}

std::shared_ptr<ZWidgetsGroup> Z3DRegionAnnotationFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("RegionAnnotation", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_stayOnTop, 1);

    std::vector<ZParameter*> paras = m_rendererBase.parameters();
    for (auto para : paras) {
      if (para->name() == "Coord Transform") {
        m_widgetsGroup->addChild(*para, 2);
      }
    }
    m_widgetsGroup->addChild(m_boundBoxMode, 5);
    m_widgetsGroup->addChild(m_boundBoxLineWidth, 5);
    m_widgetsGroup->addChild(m_boundBoxLineColor, 5);
    m_widgetsGroup->addChild(m_selectionLineWidth, 7);
    m_widgetsGroup->addChild(m_selectionLineColor, 7);
    m_widgetsGroup->addChild(m_manipulatorSize, 7);

    auto pb = new QPushButton("Show All Regions");
    connect(pb, &QPushButton::clicked, this, &Z3DRegionAnnotationFilter::showAllRegions);
    m_widgetsGroup->addChild(*pb, 8);

    pb = new QPushButton("Hide All Regions");
    connect(pb, &QPushButton::clicked, this, &Z3DRegionAnnotationFilter::hideAllRegions);
    m_widgetsGroup->addChild(*pb, 8);

    m_viewSettingTreeModel =
      std::make_unique<ZRegionAnnotationViewSettingTreeModel>(m_regionAnnotationPack->regionAnnotation(),
                                                              m_idToMeshFilters,
                                                              this);
    m_viewSettingTreeWidget = new ZRegionAnnotationViewSettingTreeView(*m_viewSettingTreeModel,
                                                                       m_regionAnnotationPack->regionAnnotation(),
                                                                       m_idToMeshFilters);
    m_widgetsGroup->addChild(*m_viewSettingTreeWidget, 9);
  }
  return m_widgetsGroup;
}

void Z3DRegionAnnotationFilter::renderOpaque(Z3DEye eye)
{
  for (const auto& idFilter : m_idToMeshFilters) {
    if (idFilter.second->isVisible() && idFilter.second->opacity() == 1.f) {
      idFilter.second->renderOpaque(eye);
    }
  }
  renderBoundBox(eye);
}

void Z3DRegionAnnotationFilter::renderTransparent(Z3DEye eye)
{
  for (const auto& idFilter : m_idToMeshFilters) {
    if (idFilter.second->isVisible() && idFilter.second->opacity() < 1.f) {
      idFilter.second->renderTransparent(eye);
    }
  }
  renderBoundBox(eye);
}

void Z3DRegionAnnotationFilter::setViewport(glm::uvec2 viewport)
{
  Z3DGeometryFilter::setViewport(viewport);
  for (const auto& idFilter : m_idToMeshFilters) {
    idFilter.second->setViewport(viewport);
  }
}

void Z3DRegionAnnotationFilter::setViewport(glm::uvec4 viewport)
{
  Z3DGeometryFilter::setViewport(viewport);
  for (const auto& idFilter : m_idToMeshFilters) {
    idFilter.second->setViewport(viewport);
  }
}

void Z3DRegionAnnotationFilter::setShaderHookType(Z3DRendererBase::ShaderHookType t)
{
  for (const auto& idFilter : m_idToMeshFilters) {
    idFilter.second->setShaderHookType(t);
  }
}

void Z3DRegionAnnotationFilter::setShaderHookParaDDPDepthBlenderTexture(const Z3DTexture* t)
{
  for (const auto& idFilter : m_idToMeshFilters) {
    idFilter.second->setShaderHookParaDDPDepthBlenderTexture(t);
  }
}

void Z3DRegionAnnotationFilter::setShaderHookParaDDPFrontBlenderTexture(const Z3DTexture* t)
{
  for (const auto& idFilter : m_idToMeshFilters) {
    idFilter.second->setShaderHookParaDDPFrontBlenderTexture(t);
  }
}

void Z3DRegionAnnotationFilter::visibleChanged(bool v)
{
  for (const auto& idFilter : m_idToMeshFilters) {
    idFilter.second->setVisible(v);
  }
}

void Z3DRegionAnnotationFilter::allMeshChanged()
{
  if (m_widgetsGroup) {
    if (m_viewSettingTreeWidget) {
      m_widgetsGroup->removeChild(*m_viewSettingTreeWidget);
      m_viewSettingTreeWidget->setParent(nullptr);
      m_viewSettingTreeWidget = nullptr;
      m_viewSettingTreeModel.reset();
    }
  }

  while (m_numParas < m_parameters.size()) {
    removeParameter(*m_parameters[m_numParas]);
  }
  m_idToMeshFilters.clear();
  m_idToRegionNames.clear();
  m_nameToID.clear();

  for (const auto& node : m_regionAnnotationPack->regionAnnotation().annotationTree()) {
    auto id = node.id;
    auto flt = new Z3DMeshFilter(m_rendererBase.globalParas(), &node);
    if (node.mesh) {
      std::vector<ZMesh*> meshList;
      meshList.push_back(node.mesh.get());
      flt->setData(&meshList);
    }
    flt->setVisible(m_visible.get());
    flt->setMeshColor(glm::vec4(node.red / 255.f, node.green / 255.f, node.blue / 255.f, 1.f));
    flt->setOpacity(0.5);
    m_idToRegionNames[id] = QString("%1_%2").arg(node.abbreviation).arg(node.id);
    m_nameToID[m_idToRegionNames[id]] = id;
    std::vector<ZParameter*> paras = flt->parameters();
    for (auto& para : paras) {
      if (para->name().contains("Coord Transform")) {
        connect(&m_rendererBase.coordTransformPara(),
                &Z3DTransformParameter::valueChanged,
                para,
                &ZParameter::updateFromSender);
      } else if (para->name() == "Visible" || para->name() == "Mesh Color" || para->name().contains("Wireframe") ||
                 para->name() == "Opacity" || para->name().contains("Material") || para->name() == "X Cut" ||
                 para->name() == "Y Cut" || para->name() == "Z Cut" || para->name() == "Bound Box") {
        para->setName(QString("%1 %2").arg(m_idToRegionNames[id]).arg(para->name()));
        addParameter(*para);
      }
    }
    m_idToMeshFilters[id] = std::unique_ptr<Z3DMeshFilter>(flt);
  }

  m_dataIsInvalid = true;
  invalidateResult();

  updateBoundBox();

  if (m_widgetsGroup) {
    m_viewSettingTreeModel =
      std::make_unique<ZRegionAnnotationViewSettingTreeModel>(m_regionAnnotationPack->regionAnnotation(),
                                                              m_idToMeshFilters,
                                                              this);
    m_viewSettingTreeWidget = new ZRegionAnnotationViewSettingTreeView(*m_viewSettingTreeModel,
                                                                       m_regionAnnotationPack->regionAnnotation(),
                                                                       m_idToMeshFilters);
    m_widgetsGroup->addChild(*m_viewSettingTreeWidget, 9);

    m_widgetsGroup->emitWidgetsGroupChangedSignal();
  }
}

void Z3DRegionAnnotationFilter::renderPicking(Z3DEye /*unused*/) {}

void Z3DRegionAnnotationFilter::registerPickingObjects() {}

void Z3DRegionAnnotationFilter::deregisterPickingObjects() {}

void Z3DRegionAnnotationFilter::updateNotTransformedBoundBoxImpl()
{
  m_notTransformedBoundBox.setMinCorner(glm::dvec3(m_regionAnnotationPack->boundBox().minCorner));
  m_notTransformedBoundBox.setMaxCorner(glm::dvec3(m_regionAnnotationPack->boundBox().maxCorner));
}

void Z3DRegionAnnotationFilter::showAllRegions()
{
  if (!isVisible()) {
    return;
  }
  for (auto& [id, flt] : m_idToMeshFilters) {
    flt->setVisible(true);
  }
}

void Z3DRegionAnnotationFilter::hideAllRegions()
{
  for (auto& [id, flt] : m_idToMeshFilters) {
    flt->setVisible(false);
  }
}

} // namespace nim
