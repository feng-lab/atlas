#include "zpunctaview.h"

namespace nim {

ZPunctaView::ZPunctaView(ZPunctaDoc& doc, ZView& view)
  : ZFilterView<ZPunctaDoc, ZPunctaFilter>(doc, view)
{
  docPunctasAdded(m_doc.objs());
  connect(&m_doc, &ZPunctaDoc::objAdded, this, &ZPunctaView::docPunctaAdded);
}

void ZPunctaView::docPunctasAdded(const std::vector<size_t>& objs)
{
  for (auto id : objs) {
    auto viewControl = new ZPunctaFilter(m_view);
    viewControl->setData(m_doc.punctaPack(id));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[id].reset(viewControl);
    connect(viewControl, &ZPunctaFilter::boundBoxChanged, this, &ZPunctaView::updateBoundBox);
    connect(viewControl, &ZPunctaFilter::objDeselected, this, &ZPunctaView::onObjDeselectedFromView);
    connect(viewControl, &ZPunctaFilter::objSelected, this, &ZPunctaView::onObjSelectedFromView);
    connect(viewControl, &ZPunctaFilter::objVisibleChanged, this, &ZPunctaView::onObjVisibleChangedFromView);
    emit objViewReady(id);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZPunctaView::docPunctaAdded(size_t id)
{
  auto viewControl = new ZPunctaFilter(m_view);
  viewControl->setData(m_doc.punctaPack(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, &ZPunctaFilter::boundBoxChanged, this, &ZPunctaView::updateBoundBox);
  connect(viewControl, &ZPunctaFilter::objDeselected, this, &ZPunctaView::onObjDeselectedFromView);
  connect(viewControl, &ZPunctaFilter::objSelected, this, &ZPunctaView::onObjSelectedFromView);
  connect(viewControl, &ZPunctaFilter::objVisibleChanged, this, &ZPunctaView::onObjVisibleChangedFromView);
  emit objViewReady(id);
}

} // namespace nim
