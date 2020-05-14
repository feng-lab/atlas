#include "zswcview.h"

namespace nim {

ZSwcView::ZSwcView(ZSwcDoc& doc, ZView& view)
  : ZFilterView<ZSwcDoc, ZSwcFilter>(doc, view)
{
  docSwcsAdded(m_doc.objs());
  connect(&m_doc, &ZSwcDoc::objAdded, this, &ZSwcView::docSwcAdded);
}

void ZSwcView::docSwcsAdded(const QList<size_t>& objs)
{
  for (auto obj : objs) {
    auto viewControl = new ZSwcFilter(m_view);
    viewControl->setData(m_doc.swcPack(obj));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[obj].reset(viewControl);
    connect(viewControl, &ZSwcFilter::boundBoxChanged, this, &ZSwcView::updateBoundBox);
    connect(viewControl, &ZSwcFilter::objDeselected, this, &ZSwcView::onObjDeselectedFromView);
    connect(viewControl, &ZSwcFilter::objSelected, this, &ZSwcView::onObjSelectedFromView);
    connect(viewControl, &ZSwcFilter::objVisibleChanged, this, &ZSwcView::onObjVisibleChangedFromView);
    emit objViewReady(obj);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZSwcView::docSwcAdded(size_t id)
{
  auto viewControl = new ZSwcFilter(m_view);
  viewControl->setData(m_doc.swcPack(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, &ZSwcFilter::boundBoxChanged, this, &ZSwcView::updateBoundBox);
  connect(viewControl, &ZSwcFilter::objDeselected, this, &ZSwcView::onObjDeselectedFromView);
  connect(viewControl, &ZSwcFilter::objSelected, this, &ZSwcView::onObjSelectedFromView);
  connect(viewControl, &ZSwcFilter::objVisibleChanged, this, &ZSwcView::onObjVisibleChangedFromView);
  emit objViewReady(id);
}

} // namespace nim
