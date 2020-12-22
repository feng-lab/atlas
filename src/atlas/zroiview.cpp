#include "zroiview.h"

namespace nim {

ZROIView::ZROIView(ZROIDoc& doc, ZView& view)
  : ZFilterView<ZROIDoc, ZROIFilter>(doc, view)
{
  docROIsAdded(m_doc.objs());
  connect(&m_doc, &ZROIDoc::objAdded, this, &ZROIView::docROIAdded);
}

void ZROIView::docROIsAdded(const std::vector<size_t>& objs)
{
  for (auto obj : objs) {
    auto viewControl = new ZROIFilter(m_view);
    viewControl->setData(m_doc.roiPack(obj).roi(), m_doc.roiPack(obj));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[obj].reset(viewControl);
    connect(viewControl, &ZROIFilter::boundBoxChanged, this, &ZROIView::updateBoundBox);
    connect(viewControl, &ZROIFilter::objDeselected, this, &ZROIView::onObjDeselectedFromView);
    connect(viewControl, &ZROIFilter::objSelected, this, &ZROIView::onObjSelectedFromView);
    connect(viewControl, &ZROIFilter::objVisibleChanged, this, &ZROIView::onObjVisibleChangedFromView);
    emit objViewReady(obj);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZROIView::docROIAdded(size_t id)
{
  auto viewControl = new ZROIFilter(m_view);
  viewControl->setData(m_doc.roiPack(id).roi(), m_doc.roiPack(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, &ZROIFilter::boundBoxChanged, this, &ZROIView::updateBoundBox);
  connect(viewControl, &ZROIFilter::objDeselected, this, &ZROIView::onObjDeselectedFromView);
  connect(viewControl, &ZROIFilter::objSelected, this, &ZROIView::onObjSelectedFromView);
  connect(viewControl, &ZROIFilter::objVisibleChanged, this, &ZROIView::onObjVisibleChangedFromView);
  emit objViewReady(id);
}

} // namespace nim
