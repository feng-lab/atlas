#include "zregionannotationfilter.h"

#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include "zgraphicsview.h"
#include "zsaturateoperation.h"
#include "zgraphicsscene.h"

namespace nim {

ZRegionAnnotationFilter::ZRegionAnnotationFilter(ZView &view)
  : ZObjFilter(view)
  , m_regionAnnotation(nullptr)
  , m_visible("Visible", true)
  , m_widgetsGroup(nullptr)
  , m_view(view)
{
  connect(&m_visible, SIGNAL(valueChanged()), this, SLOT(visibleChanged()));
  //addParameter(&m_visible);
}

ZRegionAnnotationFilter::~ZRegionAnnotationFilter()
{
}

void ZRegionAnnotationFilter::setData(ZRegionAnnotation &regionAnnotation)
{
  m_regionAnnotation = &regionAnnotation;

  allROIChanged();

  connect(m_regionAnnotation, SIGNAL(boundBoxChanged()), this, SIGNAL(boundBoxChanged()));
  connect(m_regionAnnotation, SIGNAL(allROIChanged()), this, SLOT(allROIChanged()));
  connect(m_regionAnnotation, SIGNAL(regionROIAdded(int64_t,ZROI*)),
          this, SLOT(regionROIAdded(int64_t,ZROI*)));
}

void ZRegionAnnotationFilter::releaseItemsOwnership()
{
  for (auto it = m_idToROIFilters.begin(); it != m_idToROIFilters.end(); ++it) {
    it->second->releaseItemsOwnership();
  }
}

void ZRegionAnnotationFilter::setNormalView(int z, int t)
{
  for (auto it = m_idToROIFilters.begin(); it != m_idToROIFilters.end(); ++it) {
    it->second->setNormalView(z, t);
  }
}

void ZRegionAnnotationFilter::setMaxZProjView(int t)
{
  for (auto it = m_idToROIFilters.begin(); it != m_idToROIFilters.end(); ++it) {
    it->second->setMaxZProjView(t);
  }
}

const std::vector<int> &ZRegionAnnotationFilter::boundBox() const
{
  return m_regionAnnotation->boundBox();
}

ZWidgetsGroup *ZRegionAnnotationFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = new ZWidgetsGroup("", nullptr, 1);
    new ZWidgetsGroup(&m_visible, m_widgetsGroup, 1);
    for (auto it = m_nameToID.begin(); it != m_nameToID.end(); ++it) {
      ZWidgetsGroup *wg = m_idToROIFilters[it->second]->viewSettingWidgetsGroup();
      wg->setGroupName(it->first);
      m_widgetsGroup->addChildGroup(wg);
    }
  }
  m_widgetsGroup->setUseToolBoxStyle(true);
  return m_widgetsGroup;
}

void ZRegionAnnotationFilter::deleteKeyPressed()
{
  for (auto it = m_idToROIFilters.begin(); it != m_idToROIFilters.end(); ++it) {
    it->second->deleteKeyPressed();
  }
}

void ZRegionAnnotationFilter::mousePressed(const QPointF &scenePos)
{
  for (auto it = m_idToROIFilters.begin(); it != m_idToROIFilters.end(); ++it) {
    it->second->mousePressed(scenePos);
  }
}

void ZRegionAnnotationFilter::mouseReleased(const QPointF &scenePos)
{
  for (auto it = m_idToROIFilters.begin(); it != m_idToROIFilters.end(); ++it) {
    it->second->mouseReleased(scenePos);
  }
}

void ZRegionAnnotationFilter::visibleChanged()
{
  for (auto it = m_idToROIFilters.begin(); it != m_idToROIFilters.end(); ++it) {
    it->second->setVisible(m_visible.get());
  }
}

void ZRegionAnnotationFilter::regionROIAdded(int64_t id, ZROI *roi)
{
  assert(roi);
  m_idToROIFilters.at(id)->setData(*roi);
}

void ZRegionAnnotationFilter::allROIChanged()
{
  if (m_widgetsGroup) {
    for (auto it = m_nameToID.begin(); it != m_nameToID.end(); ++it) {
      delete m_idToROIFilters[it->second]->viewSettingWidgetsGroup();
    }
  }

  m_idToROIFilters.clear();
  m_idToRegionNames.clear();
  m_nameToID.clear();

  m_parameters.clear();
  //addParameter(&m_visible);

  ZTree<RegionNode>& annoTree = m_regionAnnotation->annotationTree();
  for (auto it = annoTree.begin(); it != annoTree.end(); ++it) {
    int id = it->id;
    ZROIFilter *flt = new ZROIFilter(m_view);
    if (it->roi) {
      flt->setData(*it->roi.get());
    }
    flt->setVisible(false);
    flt->setOutlineColor(glm::vec3(it->red/255.f, it->green/255.f, it->blue/255.f));
    flt->setRegionColor(glm::vec3(it->red/255.f, it->green/255.f, it->blue/255.f));
    m_idToRegionNames[id] = QString("%1_%2").arg(it->abbreviation).arg(it->id);
    m_nameToID[m_idToRegionNames[id]] = id;
    QList<ZParameter*> paras = flt->parameters();
    for (int i=0; i<paras.size(); ++i) {
      paras[i]->setName(QString("%1 %2").arg(m_idToRegionNames[id]).arg(paras[i]->name()));
      addParameter(paras[i]);
    }
    m_idToROIFilters[id] = std::unique_ptr<ZROIFilter>(flt);
  }

  if (m_widgetsGroup) {
    for (auto it = m_nameToID.begin(); it != m_nameToID.end(); ++it) {
      ZWidgetsGroup *wg = m_idToROIFilters[it->second]->viewSettingWidgetsGroup();
      wg->setGroupName(it->first);
      m_widgetsGroup->addChildGroup(wg);
    }
    m_widgetsGroup->emitWidgetsGroupChangedSignal();
  }
}

} // namespace nim

