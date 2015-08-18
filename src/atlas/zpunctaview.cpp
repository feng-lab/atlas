#include "zpunctaview.h"

#include <cassert>

namespace nim {

ZPunctaView::ZPunctaView(ZPunctaDoc &doc, ZView &view)
  : ZFilterView<ZPunctaDoc, ZPunctaFilter>(doc, view)
{
  docPunctaAdded(m_doc.objs());
  connect(&m_doc, SIGNAL(objAdded(size_t,ZObjDoc*)), this, SLOT(docPunctaAdded(size_t)));
}

void ZPunctaView::docPunctaAdded(const QList<size_t> &objs)
{
  for (int i=0; i<objs.size(); ++i) {
    ZPunctaFilter *viewControl = new ZPunctaFilter(m_view);
    viewControl->setData(m_doc.puncta(objs[i]));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[objs[i]] = viewControl;
    connect(viewControl, SIGNAL(boundBoxChanged()), this, SLOT(updateBoundBox()));
    connect(viewControl, SIGNAL(objDeselected()), this, SLOT(onObjDeselectedFromView()));
    connect(viewControl, SIGNAL(objSelected(bool)), this, SLOT(onObjSelectedFromView(bool)));
    emit objViewReady(objs[i]);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZPunctaView::docPunctaAdded(size_t id)
{
  ZPunctaFilter *viewControl = new ZPunctaFilter(m_view);
  viewControl->setData(m_doc.puncta(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id] = viewControl;
  m_view.updateBoundBox();
  connect(viewControl, SIGNAL(boundBoxChanged()), this, SLOT(updateBoundBox()));
  connect(viewControl, SIGNAL(objDeselected()), this, SLOT(onObjDeselectedFromView()));
  connect(viewControl, SIGNAL(objSelected(bool)), this, SLOT(onObjSelectedFromView(bool)));
  emit objViewReady(id);
}

} // namespace nim
