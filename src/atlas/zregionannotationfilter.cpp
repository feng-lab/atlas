#include "zregionannotationfilter.h"

#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include "zgraphicsview.h"
#include "zsaturateoperation.h"
#include "zgraphicsscene.h"
#include "zlog.h"

namespace nim {

ZRegionAnnotationFilter::ZRegionAnnotationFilter(ZView& view)
  : ZObjFilter(view)
  , m_regionAnnotation(nullptr)
  , m_visible("Visible", true)
  , m_view(view)
{
  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZRegionAnnotationFilter::visibleChanged);
  //addParameter(&m_visible);
}

void ZRegionAnnotationFilter::setData(ZRegionAnnotation& regionAnnotation)
{
  m_regionAnnotation = &regionAnnotation;

  allROIChanged();

  connect(m_regionAnnotation, &ZRegionAnnotation::boundBoxChanged, this, &ZRegionAnnotationFilter::boundBoxChanged);
  connect(m_regionAnnotation, &ZRegionAnnotation::allROIChanged, this, &ZRegionAnnotationFilter::allROIChanged);
  connect(m_regionAnnotation, &ZRegionAnnotation::regionROIAdded, this, &ZRegionAnnotationFilter::regionROIAdded);
}

void ZRegionAnnotationFilter::releaseItemsOwnership()
{
  for (auto& idFilter : m_idToROIFilters) {
    idFilter.second->releaseItemsOwnership();
  }
}

void ZRegionAnnotationFilter::setNormalView(int z, int t)
{
  for (auto& idFilter : m_idToROIFilters) {
    idFilter.second->setNormalView(z, t);
  }
}

void ZRegionAnnotationFilter::setMaxZProjView(int t)
{
  for (auto& idFilter : m_idToROIFilters) {
    idFilter.second->setMaxZProjView(t);
  }
}

const std::vector<int>& ZRegionAnnotationFilter::boundBox() const
{
  return m_regionAnnotation->boundBox();
}

std::shared_ptr<ZWidgetsGroup> ZRegionAnnotationFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("", 1);
    m_widgetsGroup->addChild(m_visible, 1);
    for (const auto& nameID : m_nameToID) {
      std::shared_ptr<ZWidgetsGroup> wg = m_idToROIFilters[nameID.second]->viewSettingWidgetsGroupForAnnotationFilter();
      wg->setGroupName(nameID.first);
      m_widgetsGroup->addChild(wg);
    }
    m_widgetsGroup->addChild(m_offsetPara, 2);
  }
  m_widgetsGroup->setUseToolBoxStyle(true);
  return m_widgetsGroup;
}

void ZRegionAnnotationFilter::deleteKeyPressed()
{
  for (auto& idFilter : m_idToROIFilters) {
    idFilter.second->deleteKeyPressed();
  }
}

void ZRegionAnnotationFilter::mousePressed(const QPointF& scenePos)
{
  for (auto& idFilter : m_idToROIFilters) {
    idFilter.second->mousePressed(scenePos);
  }
}

void ZRegionAnnotationFilter::mouseReleased(const QPointF& scenePos)
{
  for (auto& idFilter : m_idToROIFilters) {
    idFilter.second->mouseReleased(scenePos);
  }
}

void ZRegionAnnotationFilter::rotateClockwise()
{
  for (auto& idFilter : m_idToROIFilters) {
    idFilter.second->rotateClockwise();
  }
}

void ZRegionAnnotationFilter::rotateCounterclockwise()
{
  for (auto& idFilter : m_idToROIFilters) {
    idFilter.second->rotateCounterclockwise();
  }
}

void ZRegionAnnotationFilter::visibleChanged()
{
  for (auto& idFilter : m_idToROIFilters) {
    idFilter.second->setVisible(m_visible.get());
  }
}

void ZRegionAnnotationFilter::regionROIAdded(int64_t id, ZROI* roi)
{
  CHECK(roi);
  m_idToROIFilters.at(id)->setData(*roi);
}

void ZRegionAnnotationFilter::allROIChanged()
{
  if (m_widgetsGroup) {
    for (const auto& nameID : m_nameToID) {
      m_widgetsGroup->removeChild(m_idToROIFilters[nameID.second]->viewSettingWidgetsGroupForAnnotationFilter());
    }
  }

  m_idToROIFilters.clear();
  m_idToRegionNames.clear();
  m_nameToID.clear();

  m_parameters.clear();
  //addParameter(&m_visible);
  addParameter(&m_offsetPara);

  for (auto& node : m_regionAnnotation->annotationTree()) {
    int id = node.id;
    ZROIFilter* flt = new ZROIFilter(m_view);
    if (node.roi) {
      flt->setData(*(node.roi));
    }
    flt->setVisible(false);
    flt->setOutlineColor(glm::vec3(node.red / 255.f, node.green / 255.f, node.blue / 255.f));
    flt->setRegionColor(glm::vec3(node.red / 255.f, node.green / 255.f, node.blue / 255.f));
    connect(&m_offsetPara, &ZDVec4Parameter::valueChanged, &flt->offsetPara(), &ZDVec4Parameter::updateFromSender);
    m_idToRegionNames[id] = QString("%1_%2").arg(node.abbreviation).arg(node.id);
    m_nameToID[m_idToRegionNames[id]] = id;
    for (auto para : flt->parameters()) {
      if (para->name() == "Offset") {
        continue;
      }
      para->setName(QString("%1 %2").arg(m_idToRegionNames[id]).arg(para->name()));
      addParameter(para);
    }
    m_idToROIFilters[id] = std::unique_ptr<ZROIFilter>(flt);
  }

  if (m_widgetsGroup) {
    for (const auto& nameID : m_nameToID) {
      std::shared_ptr<ZWidgetsGroup> wg = m_idToROIFilters[nameID.second]->viewSettingWidgetsGroupForAnnotationFilter();
      wg->setGroupName(nameID.first);
      m_widgetsGroup->addChild(wg);
    }
    m_widgetsGroup->emitWidgetsGroupChangedSignal();
  }
}

} // namespace nim

