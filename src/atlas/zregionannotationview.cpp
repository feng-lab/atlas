#include "zregionannotationview.h"

#include <cassert>

namespace nim {

ZRegionAnnotationView::ZRegionAnnotationView(ZRegionAnnotationDoc &doc, ZView &view)
  : ZFilterView<ZRegionAnnotationDoc, ZRegionAnnotationFilter>(doc, view)
{
  docRegionAnnotationAdded(m_doc.objs());
  connect(&m_doc, SIGNAL(objAdded(size_t,ZObjDoc*)), this, SLOT(docRegionAnnotationAdded(size_t)));
}

void ZRegionAnnotationView::docRegionAnnotationAdded(const QList<size_t> &objs)
{
  for (int i=0; i<objs.size(); ++i) {
    ZRegionAnnotationFilter *viewControl = new ZRegionAnnotationFilter(m_view);
    viewControl->setData(m_doc.regionAnnotation(objs[i]));
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

void ZRegionAnnotationView::docRegionAnnotationAdded(size_t id)
{
  ZRegionAnnotationFilter *viewControl = new ZRegionAnnotationFilter(m_view);
  viewControl->setData(m_doc.regionAnnotation(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, SIGNAL(boundBoxChanged()), this, SLOT(updateBoundBox()));
  connect(viewControl, SIGNAL(objDeselected()), this, SLOT(onObjDeselectedFromView()));
  connect(viewControl, SIGNAL(objSelected(bool)), this, SLOT(onObjSelectedFromView(bool)));
  emit objViewReady(id);
}

} // namespace nim

