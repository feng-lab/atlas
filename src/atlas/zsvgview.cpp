//
// Created by Linqing Feng on 8/24/16.
//

#include "zsvgview.h"

namespace nim {

ZSvgView::ZSvgView(ZSvgDoc& doc, ZView& view)
  : ZFilterView<ZSvgDoc, ZSvgFilter>(doc, view)
{
  docSvgsAdded(m_doc.objs());
  connect(&m_doc, &ZSvgDoc::objAdded, this, &ZSvgView::docSvgAdded);
}

void ZSvgView::docSvgsAdded(const QList<size_t>& objs)
{
  for (int i = 0; i < objs.size(); ++i) {
    ZSvgFilter* viewControl = new ZSvgFilter(m_view);
    viewControl->setData(m_doc.svg(objs[i]));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[objs[i]].reset(viewControl);
    connect(viewControl, &ZSvgFilter::boundBoxChanged, this, &ZSvgView::updateBoundBox);
    connect(viewControl, &ZSvgFilter::objDeselected, this, &ZSvgView::onObjDeselectedFromView);
    connect(viewControl, &ZSvgFilter::objSelected, this, &ZSvgView::onObjSelectedFromView);
    emit objViewReady(objs[i]);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZSvgView::docSvgAdded(size_t id)
{
  ZSvgFilter* viewControl = new ZSvgFilter(m_view);
  viewControl->setData(m_doc.svg(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, &ZSvgFilter::boundBoxChanged, this, &ZSvgView::updateBoundBox);
  connect(viewControl, &ZSvgFilter::objDeselected, this, &ZSvgView::onObjDeselectedFromView);
  connect(viewControl, &ZSvgFilter::objSelected, this, &ZSvgView::onObjSelectedFromView);
  emit objViewReady(id);
}

} // namespace nim
