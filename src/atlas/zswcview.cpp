#include "zswcview.h"

#include <cassert>

namespace nim {

ZSwcView::ZSwcView(ZSwcDoc &doc, ZView &view)
  : ZFilterView<ZSwcDoc, ZSwcFilter>(doc, view)
{
  docSwcAdded(m_doc.objs());
  connect(&m_doc, SIGNAL(objAdded(size_t,ZObjDoc*)), this, SLOT(docSwcAdded(size_t)));
}

void ZSwcView::docSwcAdded(const QList<size_t> &objs)
{
  for (int i=0; i<objs.size(); ++i) {
    ZSwcFilter *viewControl = new ZSwcFilter(m_view);
    viewControl->setData(m_doc.swc(objs[i]));
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

void ZSwcView::docSwcAdded(size_t id)
{
  ZSwcFilter *viewControl = new ZSwcFilter(m_view);
  viewControl->setData(m_doc.swc(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, SIGNAL(boundBoxChanged()), this, SLOT(updateBoundBox()));
  connect(viewControl, SIGNAL(objDeselected()), this, SLOT(onObjDeselectedFromView()));
  connect(viewControl, SIGNAL(objSelected(bool)), this, SLOT(onObjSelectedFromView(bool)));
  emit objViewReady(id);
}

} // namespace nim
