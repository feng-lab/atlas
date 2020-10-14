#include "zregionannotationfilter.h"

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
  , m_view(view)
  , m_showControlPoints("Show Control Points", true)
  , m_fixedControlPointsSize("Fixed Control Points Size", true)
  , m_highlightRegionOnMouseHover("Highlight Region On Mouse Hover", true)
{
  m_viewPrecedencePara.blockSignals(true);
  m_viewPrecedencePara.set(getViewPrecedence());
  m_viewPrecedencePara.blockSignals(false);
  addParameter(&m_showControlPoints);
  addParameter(&m_fixedControlPointsSize);
  addParameter(&m_highlightRegionOnMouseHover);
  m_numParametersWithoutRegionSepcificParas = m_parameters.size();
}

void ZRegionAnnotationFilter::setData(ZRegionAnnotationPack& regionAnnotationPack)
{
  m_regionAnnotationPack = &regionAnnotationPack;

  allROIChanged();

  connect(&m_regionAnnotationPack->regionAnnotation(), &ZRegionAnnotation::boundBoxChanged,
          this, &ZRegionAnnotationFilter::boundBoxChanged);
  connect(&m_regionAnnotationPack->regionAnnotation(), &ZRegionAnnotation::allROIChanged,
          this, &ZRegionAnnotationFilter::allROIChanged);
  connect(&m_regionAnnotationPack->regionAnnotation(), &ZRegionAnnotation::regionROIAdded,
          this, &ZRegionAnnotationFilter::regionROIAdded);

  connect(m_regionAnnotationPack, &ZRegionAnnotationPack::lockedStateChanged,
          this, &ZRegionAnnotationFilter::onLockedStateChanged);
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
  auto res = m_regionAnnotationPack->boundBox();
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
    m_widgetsGroup->addChild(m_showControlPoints, 1);
    m_widgetsGroup->addChild(m_fixedControlPointsSize, 1);
    m_widgetsGroup->addChild(m_transform, 2);
    m_widgetsGroup->addChild(m_offsetPara, 2);
    m_widgetsGroup->addChild(m_highlightRegionOnMouseHover, 2);

    m_viewSettingTreeModel =
      std::make_unique<ZRegionAnnotationViewSettingTreeModel>(m_regionAnnotationPack->regionAnnotation(),
                                                              m_idToROIFilters, this);
    m_viewSettingTreeWidget =
      new ZRegionAnnotationViewSettingTreeView(*m_viewSettingTreeModel,
                                               m_regionAnnotationPack->regionAnnotation(),
                                               m_idToROIFilters);
    m_widgetsGroup->addChild(*m_viewSettingTreeWidget, 4);
  }
  return m_widgetsGroup;
}

void ZRegionAnnotationFilter::deleteKeyPressed()
{
  if (m_regionAnnotationPack && m_regionAnnotationPack->isLocked()) {
    return;
  }
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->deleteKeyPressed();
  }
}

void ZRegionAnnotationFilter::copyKeyPressed()
{
  if (m_regionAnnotationPack && m_regionAnnotationPack->isLocked()) {
    return;
  }
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->copyKeyPressed();
  }
}

void ZRegionAnnotationFilter::pasteKeyPressed(int slice, QPointF point, bool hFlip, bool vFlip)
{
  if (m_regionAnnotationPack && m_regionAnnotationPack->isLocked()) {
    return;
  }
  auto srcBoundBox = m_regionAnnotationPack->regionAnnotation().copiedItemBoundBox();
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->pasteKeyPressed(slice, point, srcBoundBox, hFlip, vFlip);
  }
}

void ZRegionAnnotationFilter::mousePressed(const QPointF& scenePos)
{
  if (m_regionAnnotationPack && m_regionAnnotationPack->isLocked()) {
    return;
  }
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->mousePressed(scenePos);
  }
}

void ZRegionAnnotationFilter::mouseMoved(const QPointF& scenePos)
{
  if (m_regionAnnotationPack && m_regionAnnotationPack->isLocked()) {
    return;
  }
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->mouseMoved(scenePos);
  }
}

void ZRegionAnnotationFilter::mouseReleased(const QPointF& scenePos)
{
  if (m_regionAnnotationPack && m_regionAnnotationPack->isLocked()) {
    return;
  }
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->mouseReleased(scenePos);
  }
}

void ZRegionAnnotationFilter::rotateClockwise(double x, double y)
{
  if (m_regionAnnotationPack && m_regionAnnotationPack->isLocked()) {
    return;
  }
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->rotateClockwise(x, y);
  }
}

void ZRegionAnnotationFilter::rotateCounterclockwise(double x, double y)
{
  if (m_regionAnnotationPack && m_regionAnnotationPack->isLocked()) {
    return;
  }
  for (const auto& idFilter : m_idToROIFilters) {
    idFilter.second->rotateCounterclockwise(x, y);
  }
}

void ZRegionAnnotationFilter::regionROIAdded(int64_t id, ZROI* roi)
{
  CHECK(roi);
  m_idToROIFilters.at(id)->setData(*roi, *m_regionAnnotationPack);
}

void ZRegionAnnotationFilter::allROIChanged()
{
  if (m_widgetsGroup) {
    if (m_viewSettingTreeWidget) {
      m_widgetsGroup->removeChild(*m_viewSettingTreeWidget);
      m_viewSettingTreeWidget->setParent(nullptr);
      m_viewSettingTreeWidget = nullptr;
      m_viewSettingTreeModel.reset();
    }
  }

  m_idToROIFilters.clear();
  m_idToRegionNames.clear();
  m_nameToID.clear();

  while (m_parameters.size() > static_cast<int>(m_numParametersWithoutRegionSepcificParas)) {
    m_parameters.pop_back();
  }

  for (const auto& node : m_regionAnnotationPack->regionAnnotation().annotationTree()) {
    int id = node.id;
    auto flt = new ZROIFilter(m_view, &node);
    if (node.roi) {
      flt->setData(*(node.roi), *m_regionAnnotationPack);
    }
    flt->setVisible(m_visible.get());
    flt->setOutlineColor(glm::vec3(node.red / 255.f, node.green / 255.f, node.blue / 255.f));
    flt->setRegionColor(glm::vec3(node.red / 255.f, node.green / 255.f, node.blue / 255.f));
    flt->viewPrecedencePara().setValue(m_viewPrecedencePara.get());
    flt->transformPara().set(m_transform.get());
    flt->offsetPara().set(m_offsetPara.get());
    flt->showControlPointsPara().set(m_showControlPoints.get());
    flt->fixedControlPointsSizePara().set(m_fixedControlPointsSize.get());
    flt->highlightRegionOnMouseHoverPara().set(m_highlightRegionOnMouseHover.get());
    connect(&m_visible, &ZBoolParameter::valueChanged,
            &flt->visiblePara(), &ZBoolParameter::updateFromSender);
    connect(&m_viewPrecedencePara, &ZIntParameter::valueChanged,
            &flt->viewPrecedencePara(), &ZIntParameter::updateFromSender);
    connect(&m_transform, &Z2DTransformParameter::valueChanged,
            &flt->transformPara(), &Z2DTransformParameter::updateFromSender);
    connect(&m_offsetPara, &ZDVec2Parameter::valueChanged,
            &flt->offsetPara(), &ZDVec2Parameter::updateFromSender);
    connect(&m_showControlPoints, &ZBoolParameter::valueChanged,
            &flt->showControlPointsPara(), &ZBoolParameter::updateFromSender);
    connect(&m_fixedControlPointsSize, &ZBoolParameter::valueChanged,
            &flt->fixedControlPointsSizePara(), &ZBoolParameter::updateFromSender);
    connect(&m_highlightRegionOnMouseHover, &ZBoolParameter::valueChanged,
            &flt->highlightRegionOnMouseHoverPara(), &ZBoolParameter::updateFromSender);

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
    m_viewSettingTreeModel =
      std::make_unique<ZRegionAnnotationViewSettingTreeModel>(m_regionAnnotationPack->regionAnnotation(),
                                                              m_idToROIFilters, this);
    m_viewSettingTreeWidget =
      new ZRegionAnnotationViewSettingTreeView(*m_viewSettingTreeModel,
                                               m_regionAnnotationPack->regionAnnotation(),
                                               m_idToROIFilters);
    m_widgetsGroup->addChild(*m_viewSettingTreeWidget, 4);

    m_widgetsGroup->emitWidgetsGroupChangedSignal();
  }
}

void ZRegionAnnotationFilter::onLockedStateChanged(bool)
{}

} // namespace nim

