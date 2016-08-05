#include "zpunctaview.h"

namespace nim {

ZPunctaView::ZPunctaView(ZPunctaDoc& doc, ZView& view)
  : ZFilterView<ZPunctaDoc, ZPunctaFilter>(doc, view)
{
  docPunctasAdded(m_doc.objs());
  connect(&m_doc, &ZPunctaDoc::objAdded, this, &ZPunctaView::docPunctaAdded);
}

void ZPunctaView::docPunctasAdded(const QList<size_t>& objs)
{
  for (int i = 0; i < objs.size(); ++i) {
    ZPunctaFilter* viewControl = new ZPunctaFilter(m_view);
    viewControl->setData(m_doc.puncta(objs[i]));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[objs[i]].reset(viewControl);
    connect(viewControl, &ZPunctaFilter::boundBoxChanged, this, &ZPunctaView::updateBoundBox);
    connect(viewControl, &ZPunctaFilter::objDeselected, this, &ZPunctaView::onObjDeselectedFromView);
    connect(viewControl, &ZPunctaFilter::objSelected, this, &ZPunctaView::onObjSelectedFromView);
    emit objViewReady(objs[i]);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZPunctaView::docPunctaAdded(size_t id)
{
  ZPunctaFilter* viewControl = new ZPunctaFilter(m_view);
  viewControl->setData(m_doc.puncta(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, &ZPunctaFilter::boundBoxChanged, this, &ZPunctaView::updateBoundBox);
  connect(viewControl, &ZPunctaFilter::objDeselected, this, &ZPunctaView::onObjDeselectedFromView);
  connect(viewControl, &ZPunctaFilter::objSelected, this, &ZPunctaView::onObjSelectedFromView);
  emit objViewReady(id);
}

} // namespace nim
