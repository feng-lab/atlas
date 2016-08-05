#include "zroiview.h"

namespace nim {

ZROIView::ZROIView(ZROIDoc& doc, ZView& view)
  : ZFilterView<ZROIDoc, ZROIFilter>(doc, view)
{
  docROIsAdded(m_doc.objs());
  connect(&m_doc, &ZROIDoc::objAdded, this, &ZROIView::docROIAdded);
}

void ZROIView::docROIsAdded(const QList<size_t>& objs)
{
  for (int i = 0; i < objs.size(); ++i) {
    ZROIFilter* viewControl = new ZROIFilter(m_view);
    viewControl->setData(m_doc.roi(objs[i]));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[objs[i]].reset(viewControl);
    connect(viewControl, &ZROIFilter::boundBoxChanged, this, &ZROIView::updateBoundBox);
    connect(viewControl, &ZROIFilter::objDeselected, this, &ZROIView::onObjDeselectedFromView);
    connect(viewControl, &ZROIFilter::objSelected, this, &ZROIView::onObjSelectedFromView);
    emit objViewReady(objs[i]);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZROIView::docROIAdded(size_t id)
{
  ZROIFilter* viewControl = new ZROIFilter(m_view);
  viewControl->setData(m_doc.roi(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, &ZROIFilter::boundBoxChanged, this, &ZROIView::updateBoundBox);
  connect(viewControl, &ZROIFilter::objDeselected, this, &ZROIView::onObjDeselectedFromView);
  connect(viewControl, &ZROIFilter::objSelected, this, &ZROIView::onObjSelectedFromView);
  emit objViewReady(id);
}

} // namespace nim
