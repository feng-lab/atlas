#include "zregionannotationfilter.h"

#include "zregionannotationviewsettingtreemodel.h"
#include "zregionannotationviewsettingtreeview.h"
#include "zgraphicsview.h"
#include "znumericparameter.h"
#include "zwidgetsgroup.h"
#include "zgraphicsscene.h"
#include "zsaturateoperation.h"
#include "zlog.h"
#include <QPushButton>

namespace nim {

ZRegionAnnotationFilter::ZRegionAnnotationFilter(ZView& view)
  : ZObjFilter(view)
  , m_visible("Visible", true)
  , m_view(view)
{
  connect(&m_visible, &ZBoolParameter::valueChanged, this, &ZRegionAnnotationFilter::visibleChanged);
  //addParameter(&m_visible);
  m_viewPrecedencePara.blockSignals(true);
  m_viewPrecedencePara.set(getViewPrecedence());
  m_viewPrecedencePara.blockSignals(false);
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
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->releaseItemsOwnership();
  }
}

void ZRegionAnnotationFilter::setSelected(bool v)
{
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->setSelected(v);
  }
}

void ZRegionAnnotationFilter::setNormalView(int z, int t)
{
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->setNormalView(z, t);
  }
}

void ZRegionAnnotationFilter::setMaxZProjView(int t)
{
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->setMaxZProjView(t);
  }
}

ZBBox<glm::ivec4> ZRegionAnnotationFilter::boundBox() const
{
  auto res = m_regionAnnotation->boundBox();
  updateBoundBoxWithOffsetPara(res);
  return res;
}

std::shared_ptr<ZWidgetsGroup> ZRegionAnnotationFilter::viewSettingWidgetsGroup()
{
  if (!m_widgetsGroup) {
    m_widgetsGroup = std::make_shared<ZWidgetsGroup>("", 1);
    m_widgetsGroup->addChild(m_visible, 1);

    auto pb = new QPushButton("Bring to Front");
    connect(pb, &QPushButton::clicked, this, &ZRegionAnnotationFilter::bringToFront);
    m_widgetsGroup->addChild(*pb, 1);

    pb = new QPushButton("Send to Back");
    connect(pb, &QPushButton::clicked, this, &ZRegionAnnotationFilter::sendToBack);
    m_widgetsGroup->addChild(*pb, 1);

    m_widgetsGroup->addChild(m_viewPrecedencePara, 1);
    m_widgetsGroup->addChild(m_transform, 2);
    m_widgetsGroup->addChild(m_offsetPara, 2);

    auto model =
      new ZRegionAnnotationViewSettingTreeModel(*m_regionAnnotation, m_idToROIFilters, this);
    m_viewSettingTreeWidgetGroup = std::make_shared<ZWidgetsGroup>(
      *new ZRegionAnnotationViewSettingTreeView(*model, *m_regionAnnotation, m_idToROIFilters, &m_view), 4);
    m_widgetsGroup->addChild(m_viewSettingTreeWidgetGroup);
  }
  return m_widgetsGroup;
}

void ZRegionAnnotationFilter::deleteKeyPressed()
{
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->deleteKeyPressed();
  }
}

void ZRegionAnnotationFilter::copyKeyPressed()
{
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->copyKeyPressed();
  }
}

void ZRegionAnnotationFilter::pasteKeyPressed(int slice, QPointF point, bool hFlip, bool vFlip)
{
  auto srcBoundBox = m_regionAnnotation->copiedItemBoundBox();
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->pasteKeyPressed(slice, point, srcBoundBox, hFlip, vFlip);
  }
}

void ZRegionAnnotationFilter::mousePressed(const QPointF& scenePos)
{
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->mousePressed(scenePos);
  }
}

void ZRegionAnnotationFilter::mouseMoved(const QPointF& scenePos)
{
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->mouseMoved(scenePos);
  }
}

void ZRegionAnnotationFilter::mouseReleased(const QPointF& scenePos)
{
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->mouseReleased(scenePos);
  }
}

void ZRegionAnnotationFilter::rotateClockwise()
{
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->rotateClockwise();
  }
}

void ZRegionAnnotationFilter::rotateCounterclockwise()
{
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->rotateCounterclockwise();
  }
}

void ZRegionAnnotationFilter::visibleChanged()
{
  for (const auto& idFilter : m_idToROIFilters) {
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
    if (m_viewSettingTreeWidgetGroup) {
      m_widgetsGroup->removeChild(m_viewSettingTreeWidgetGroup);
      m_viewSettingTreeWidgetGroup.reset();
    }
  }

  m_idToROIFilters.clear();
  m_idToRegionNames.clear();
  m_nameToID.clear();

  m_parameters.clear();
  //addParameter(&m_visible);
  addParameter(&m_viewPrecedencePara);
  addParameter(&m_transform);
  addParameter(&m_offsetPara);

  for (const auto& node : m_regionAnnotation->annotationTree()) {
    int id = node.id;
    auto flt = new ZROIFilter(m_view, &node);
    if (node.roi) {
      flt->setData(*(node.roi));
    }
    flt->setVisible(true);
    flt->setOutlineColor(glm::vec3(node.red / 255.f, node.green / 255.f, node.blue / 255.f));
    flt->setRegionColor(glm::vec3(node.red / 255.f, node.green / 255.f, node.blue / 255.f));
    flt->viewPrecedencePara().setValue(m_viewPrecedencePara.get());
    flt->transformPara().set(m_transform.get());
    connect(&m_viewPrecedencePara, &ZIntParameter::valueChanged,
            &flt->viewPrecedencePara(), &ZIntParameter::updateFromSender);
    connect(&m_transform, &Z2DTransformParameter::valueChanged,
            &flt->transformPara(), &Z2DTransformParameter::updateFromSender);
    connect(&m_offsetPara, &ZDVec2Parameter::valueChanged,
            &flt->offsetPara(), &ZDVec2Parameter::updateFromSender);

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
    auto model =
      new ZRegionAnnotationViewSettingTreeModel(*m_regionAnnotation, m_idToROIFilters, this);
    m_viewSettingTreeWidgetGroup = std::make_shared<ZWidgetsGroup>(
      *new ZRegionAnnotationViewSettingTreeView(*model, *m_regionAnnotation, m_idToROIFilters, &m_view), 4);
    m_widgetsGroup->addChild(m_viewSettingTreeWidgetGroup);

    m_widgetsGroup->emitWidgetsGroupChangedSignal();
  }
}

} // namespace nim

