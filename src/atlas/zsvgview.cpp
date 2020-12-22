#include "zsvgview.h"

namespace nim {

ZSvgView::ZSvgView(ZSvgDoc& doc, ZView& view)
  : ZFilterView<ZSvgDoc, ZSvgFilter>(doc, view)
{
  docSvgsAdded(m_doc.objs());
  connect(&m_doc, &ZSvgDoc::objAdded, this, &ZSvgView::docSvgAdded);
}

void ZSvgView::docSvgsAdded(const std::vector<size_t>& objs)
{
  for (auto id : objs) {
    auto viewControl = new ZSvgFilter(m_view);
    viewControl->setData(m_doc.svg(id));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[id].reset(viewControl);
    connect(viewControl, &ZSvgFilter::boundBoxChanged, this, &ZSvgView::updateBoundBox);
    connect(viewControl, &ZSvgFilter::objDeselected, this, &ZSvgView::onObjDeselectedFromView);
    connect(viewControl, &ZSvgFilter::objSelected, this, &ZSvgView::onObjSelectedFromView);
    connect(viewControl, &ZSvgFilter::objVisibleChanged, this, &ZSvgView::onObjVisibleChangedFromView);
    emit objViewReady(id);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZSvgView::docSvgAdded(size_t id)
{
  auto viewControl = new ZSvgFilter(m_view);
  viewControl->setData(m_doc.svg(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, &ZSvgFilter::boundBoxChanged, this, &ZSvgView::updateBoundBox);
  connect(viewControl, &ZSvgFilter::objDeselected, this, &ZSvgView::onObjDeselectedFromView);
  connect(viewControl, &ZSvgFilter::objSelected, this, &ZSvgView::onObjSelectedFromView);
  connect(viewControl, &ZSvgFilter::objVisibleChanged, this, &ZSvgView::onObjVisibleChangedFromView);
  emit objViewReady(id);
}

} // namespace nim
