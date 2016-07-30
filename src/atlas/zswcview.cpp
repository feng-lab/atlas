#include "zswcview.h"

#include <cassert>

namespace nim {

ZSwcView::ZSwcView(ZSwcDoc& doc, ZView& view)
  : ZFilterView<ZSwcDoc, ZSwcFilter>(doc, view)
{
  docSwcsAdded(m_doc.objs());
  connect(&m_doc, &ZSwcDoc::objAdded, this, &ZSwcView::docSwcAdded);
}

void ZSwcView::docSwcsAdded(const QList<size_t>& objs)
{
  for (int i = 0; i < objs.size(); ++i) {
    ZSwcFilter* viewControl = new ZSwcFilter(m_view);
    viewControl->setData(m_doc.swc(objs[i]));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[objs[i]].reset(viewControl);
    connect(viewControl, &ZSwcFilter::boundBoxChanged, this, &ZSwcView::updateBoundBox);
    connect(viewControl, &ZSwcFilter::objDeselected, this, &ZSwcView::onObjDeselectedFromView);
    connect(viewControl, &ZSwcFilter::objSelected, this, &ZSwcView::onObjSelectedFromView);
    emit objViewReady(objs[i]);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZSwcView::docSwcAdded(size_t id)
{
  ZSwcFilter* viewControl = new ZSwcFilter(m_view);
  viewControl->setData(m_doc.swc(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, &ZSwcFilter::boundBoxChanged, this, &ZSwcView::updateBoundBox);
  connect(viewControl, &ZSwcFilter::objDeselected, this, &ZSwcView::onObjDeselectedFromView);
  connect(viewControl, &ZSwcFilter::objSelected, this, &ZSwcView::onObjSelectedFromView);
  emit objViewReady(id);
}

} // namespace nim
