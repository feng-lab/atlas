#include "zskeletonview.h"

namespace nim {

ZSkeletonView::ZSkeletonView(ZSkeletonDoc& doc, ZView& view)
  : ZFilterView<ZSkeletonDoc, ZSkeletonFilter>(doc, view)
{
  docSkeletonsAdded(m_doc.objs());
  connect(&m_doc, &ZSkeletonDoc::objAdded, this, &ZSkeletonView::docSkeletonAdded);
  connect(&m_doc, &ZSkeletonDoc::skeletonChanged, this, &ZSkeletonView::docSkeletonChanged);
}

void ZSkeletonView::docSkeletonsAdded(const std::vector<size_t>& objs)
{
  for (auto obj : objs) {
    auto viewControl = new ZSkeletonFilter(m_view);
    viewControl->setData(m_doc.skeleton(obj));
    viewControl->setSelected(m_doc.isObjSelected(obj));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[obj].reset(viewControl);
    connect(viewControl, &ZSkeletonFilter::boundBoxChanged, this, &ZSkeletonView::updateBoundBox);
    connect(viewControl, &ZSkeletonFilter::objDeselected, this, &ZSkeletonView::onObjDeselectedFromView);
    connect(viewControl, &ZSkeletonFilter::objSelected, this, &ZSkeletonView::onObjSelectedFromView);
    connect(viewControl, &ZSkeletonFilter::objVisibleChanged, this, &ZSkeletonView::onObjVisibleChangedFromView);
    Q_EMIT objViewReady(obj);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZSkeletonView::docSkeletonAdded(size_t id)
{
  auto viewControl = new ZSkeletonFilter(m_view);
  viewControl->setData(m_doc.skeleton(id));
  viewControl->setSelected(m_doc.isObjSelected(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, &ZSkeletonFilter::boundBoxChanged, this, &ZSkeletonView::updateBoundBox);
  connect(viewControl, &ZSkeletonFilter::objDeselected, this, &ZSkeletonView::onObjDeselectedFromView);
  connect(viewControl, &ZSkeletonFilter::objSelected, this, &ZSkeletonView::onObjSelectedFromView);
  connect(viewControl, &ZSkeletonFilter::objVisibleChanged, this, &ZSkeletonView::onObjVisibleChangedFromView);
  Q_EMIT objViewReady(id);
}

void ZSkeletonView::docSkeletonChanged(size_t id)
{
  auto it = m_idToFilter.find(id);
  if (it == m_idToFilter.end()) {
    return;
  }

  it->second->reloadSkeletonGeometry();
}

} // namespace nim
