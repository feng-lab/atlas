#include "z3dregionannotationfilter.h"

#include "zmesh.h"
#include "zrandom.h"
#include <QFileInfo>

namespace nim {

Z3DRegionAnnotationFilter::Z3DRegionAnnotationFilter(Z3DGlobalParameters& globalParas, QObject* parent)
  : Z3DGeometryFilter(globalParas, parent)
  , m_visible("Visible", true)
{
  //addParameter(m_visible);

  connect(&m_visible, &ZBoolParameter::boolChanged, this, &Z3DRegionAnnotationFilter::visibleChanged);
  connect(&m_visible, &ZBoolParameter::boolChanged, this, &Z3DRegionAnnotationFilter::objVisibleChanged);

  m_numParas = m_parameters.size();
}

Z3DRegionAnnotationFilter::~Z3DRegionAnnotationFilter()
{
}

void Z3DRegionAnnotationFilter::process(Z3DEye eye)
{
  initializeCutRange();
  initializeRotationCenter();
  for (auto it = m_idToMeshFilters.begin(); it != m_idToMeshFilters.end(); ++it) {
    it->second->process(eye);
  }
}

void Z3DRegionAnnotationFilter::setData(ZRegionAnnotation& regAnno)
{
  m_regionAnnotation = &regAnno;
  allMeshChanged();
  connect(m_regionAnnotation, &ZRegionAnnotation::allMeshChanged, this, &Z3DRegionAnnotationFilter::allMeshChanged);
}

bool Z3DRegionAnnotationFilter::isReady(Z3DEye eye) const
{
  return Z3DGeometryFilter::isReady(eye) && m_regionAnnotation;
}

std::shared_ptr<ZWidgetsGroup> Z3DRegionAnnotationFilter::widgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("RegionAnnotation", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    m_widgetsGroup->addChild(m_stayOnTop, 1);

    std::vector<ZParameter*> paras = m_rendererBase.parameters();
    for (auto para : paras) {
      if (para->name() == "Coord Transform")
        m_widgetsGroup->addChild(*para, 2);
    }
    m_widgetsGroup->addChild(m_boundBoxMode, 5);
    m_widgetsGroup->addChild(m_boundBoxLineWidth, 5);
    m_widgetsGroup->addChild(m_boundBoxLineColor, 5);
    m_widgetsGroup->addChild(m_selectionLineWidth, 7);
    m_widgetsGroup->addChild(m_selectionLineColor, 7);
    m_widgetsGroup->addChild(m_manipulatorSize, 7);


    for (auto it = m_nameToID.begin(); it != m_nameToID.end(); ++it) {
      std::shared_ptr<ZWidgetsGroup> wg = m_idToMeshFilters[it->second]->widgetsGroupForAnnotationFilter();
      wg->setGroupName(it->first);
      m_widgetsGroup->addChild(wg);
    }
  }
  m_widgetsGroup->setUseToolBoxStyle(true);
  return m_widgetsGroup;
}

void Z3DRegionAnnotationFilter::renderOpaque(Z3DEye eye)
{
  for (auto it = m_idToMeshFilters.begin(); it != m_idToMeshFilters.end(); ++it) {
    if (it->second->isVisible() && it->second->opacity() == 1.f)
      it->second->renderTransparent(eye);
  }
  renderBoundBox(eye);
}

void Z3DRegionAnnotationFilter::renderTransparent(Z3DEye eye)
{
  for (auto it = m_idToMeshFilters.begin(); it != m_idToMeshFilters.end(); ++it) {
    if (it->second->isVisible() && it->second->opacity() < 1.f)
      it->second->renderTransparent(eye);
  }
  renderBoundBox(eye);
}

void Z3DRegionAnnotationFilter::setViewport(glm::uvec2 viewport)
{
  Z3DGeometryFilter::setViewport(viewport);
  for (auto it = m_idToMeshFilters.begin(); it != m_idToMeshFilters.end(); ++it) {
    it->second->setViewport(viewport);
  }
}

void Z3DRegionAnnotationFilter::setViewport(glm::uvec4 viewport)
{
  Z3DGeometryFilter::setViewport(viewport);
  for (auto it = m_idToMeshFilters.begin(); it != m_idToMeshFilters.end(); ++it) {
    it->second->setViewport(viewport);
  }
}

void Z3DRegionAnnotationFilter::setShaderHookType(Z3DRendererBase::ShaderHookType t)
{
  for (auto it = m_idToMeshFilters.begin(); it != m_idToMeshFilters.end(); ++it) {
    it->second->setShaderHookType(t);
  }
}

void Z3DRegionAnnotationFilter::setShaderHookParaDDPDepthBlenderTexture(const Z3DTexture* t)
{
  for (auto it = m_idToMeshFilters.begin(); it != m_idToMeshFilters.end(); ++it) {
    it->second->setShaderHookParaDDPDepthBlenderTexture(t);
  }
}

void Z3DRegionAnnotationFilter::setShaderHookParaDDPFrontBlenderTexture(const Z3DTexture* t)
{
  for (auto it = m_idToMeshFilters.begin(); it != m_idToMeshFilters.end(); ++it) {
    it->second->setShaderHookParaDDPFrontBlenderTexture(t);
  }
}

void Z3DRegionAnnotationFilter::visibleChanged(bool v)
{
  for (auto it = m_idToMeshFilters.begin(); it != m_idToMeshFilters.end(); ++it) {
    it->second->setVisible(v);
  }
}

void Z3DRegionAnnotationFilter::allMeshChanged()
{
  if (m_widgetsGroup) {
    for (auto it = m_nameToID.begin(); it != m_nameToID.end(); ++it) {
      m_widgetsGroup->removeChild(m_idToMeshFilters[it->second]->widgetsGroupForAnnotationFilter());
    }
  }

  while (m_numParas < m_parameters.size()) {
    removeParameter(*m_parameters[m_numParas]);
  }
  m_idToMeshFilters.clear();
  m_idToRegionNames.clear();
  m_nameToID.clear();

  const ZTree<RegionNode>& annoTree = m_regionAnnotation->annotationTree();
  for (auto it = annoTree.begin(); it != annoTree.end(); ++it) {
    int id = it->id;
    Z3DMeshFilter* flt = new Z3DMeshFilter(m_rendererBase.globalParas());
    if (it->mesh) {
      QList<ZMesh*> meshList;
      meshList.push_back(it->mesh.get());
      flt->setData(&meshList);
    }
    flt->setVisible(false);
    flt->setMeshColor(glm::vec4(it->red / 255.f, it->green / 255.f, it->blue / 255.f, 1.f));
    flt->setOpacity(0.5);
    m_idToRegionNames[id] = QString("%1_%2").arg(it->abbreviation).arg(it->id);
    m_nameToID[m_idToRegionNames[id]] = id;
    std::vector<ZParameter*> paras = flt->parameters();
    for (size_t i = 0; i < paras.size(); ++i) {
      if (paras[i]->name().contains("Coord Transform")) {
        connect(&m_rendererBase.coordTransformPara(), &Z3DTransformParameter::valueChanged,
                paras[i], &ZParameter::updateFromSender);
      } else if (paras[i]->name() == "Visible" ||
                 paras[i]->name() == "Mesh Color" ||
                 paras[i]->name().contains("Wireframe") ||
                 paras[i]->name() == "Opacity" ||
                 paras[i]->name().contains("Material") ||
                 paras[i]->name() == "X Cut" ||
                 paras[i]->name() == "Y Cut" ||
                 paras[i]->name() == "Z Cut" ||
                 paras[i]->name() == "Bound Box") {
        paras[i]->setName(QString("%1 %2").arg(m_idToRegionNames[id]).arg(paras[i]->name()));
        addParameter(*paras[i]);
      }
    }
    m_idToMeshFilters[id] = std::unique_ptr<Z3DMeshFilter>(flt);
  }

  m_dataIsInvalid = true;
  invalidateResult();

  updateBoundBox();

  if (m_widgetsGroup) {
    for (auto it = m_nameToID.begin(); it != m_nameToID.end(); ++it) {
      std::shared_ptr<ZWidgetsGroup> wg = m_idToMeshFilters[it->second]->widgetsGroupForAnnotationFilter();
      wg->setGroupName(it->first);
      m_widgetsGroup->addChild(wg);
    }
    m_widgetsGroup->emitWidgetsGroupChangedSignal();
  }
}

void Z3DRegionAnnotationFilter::renderPicking(Z3DEye)
{
}

void Z3DRegionAnnotationFilter::registerPickingObjects()
{
}

void Z3DRegionAnnotationFilter::deregisterPickingObjects()
{
}

void Z3DRegionAnnotationFilter::updateNotTransformedBoundBoxImpl()
{
  m_notTransformedBoundBox[0] = m_regionAnnotation->boundBox()[0];
  m_notTransformedBoundBox[1] = m_regionAnnotation->boundBox()[1];
  m_notTransformedBoundBox[2] = m_regionAnnotation->boundBox()[2];
  m_notTransformedBoundBox[3] = m_regionAnnotation->boundBox()[3];
  m_notTransformedBoundBox[4] = m_regionAnnotation->boundBox()[4];
  m_notTransformedBoundBox[5] = m_regionAnnotation->boundBox()[5];
}

} // namespace nim

