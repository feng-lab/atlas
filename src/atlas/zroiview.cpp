#include "zroiview.h"

#include <cassert>

namespace nim {

ZROIView::ZROIView(ZROIDoc &doc, ZView &view)
  : ZFilterView<ZROIDoc, ZROIFilter>(doc, view)
{
  docROIAdded(m_doc.objs());
  connect(&m_doc, SIGNAL(objAdded(size_t,ZObjDoc*)), this, SLOT(docROIAdded(size_t)));
}

void ZROIView::docROIAdded(const QList<size_t> &objs)
{
  for (int i=0; i<objs.size(); ++i) {
    ZROIFilter *viewControl = new ZROIFilter(m_view);
    viewControl->setData(m_doc.roi(objs[i]));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[objs[i]].reset(viewControl);
    connect(viewControl, SIGNAL(boundBoxChanged()), this, SLOT(updateBoundBox()));
    connect(viewControl, SIGNAL(objDeselected()), this, SLOT(onObjDeselectedFromView()));
    connect(viewControl, SIGNAL(objSelected(bool)), this, SLOT(onObjSelectedFromView(bool)));
    emit objViewReady(objs[i]);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZROIView::docROIAdded(size_t id)
{
  ZROIFilter *viewControl = new ZROIFilter(m_view);
  viewControl->setData(m_doc.roi(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, SIGNAL(boundBoxChanged()), this, SLOT(updateBoundBox()));
  connect(viewControl, SIGNAL(objDeselected()), this, SLOT(onObjDeselectedFromView()));
  connect(viewControl, SIGNAL(objSelected(bool)), this, SLOT(onObjSelectedFromView(bool)));
  emit objViewReady(id);
}

} // namespace nim
