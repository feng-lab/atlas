#include "zregionannotationview.h"

namespace nim {

ZRegionAnnotationView::ZRegionAnnotationView(ZRegionAnnotationDoc& doc, ZView& view)
  : ZFilterView<ZRegionAnnotationDoc, ZRegionAnnotationFilter>(doc, view)
{
  docRegionAnnotationsAdded(m_doc.objs());
  connect(&m_doc, &ZRegionAnnotationDoc::objAdded, this, &ZRegionAnnotationView::docRegionAnnotationAdded);
}

void ZRegionAnnotationView::docRegionAnnotationsAdded(const QList<size_t>& objs)
{
  for (int i = 0; i < objs.size(); ++i) {
    ZRegionAnnotationFilter* viewControl = new ZRegionAnnotationFilter(m_view);
    viewControl->setData(m_doc.regionAnnotation(objs[i]));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[objs[i]].reset(viewControl);
    connect(viewControl, &ZRegionAnnotationFilter::boundBoxChanged, this, &ZRegionAnnotationView::updateBoundBox);
    connect(viewControl, &ZRegionAnnotationFilter::objDeselected, this,
            &ZRegionAnnotationView::onObjDeselectedFromView);
    connect(viewControl, &ZRegionAnnotationFilter::objSelected, this, &ZRegionAnnotationView::onObjSelectedFromView);
    emit objViewReady(objs[i]);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZRegionAnnotationView::docRegionAnnotationAdded(size_t id)
{
  ZRegionAnnotationFilter* viewControl = new ZRegionAnnotationFilter(m_view);
  viewControl->setData(m_doc.regionAnnotation(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, &ZRegionAnnotationFilter::boundBoxChanged, this, &ZRegionAnnotationView::updateBoundBox);
  connect(viewControl, &ZRegionAnnotationFilter::objDeselected, this, &ZRegionAnnotationView::onObjDeselectedFromView);
  connect(viewControl, &ZRegionAnnotationFilter::objSelected, this, &ZRegionAnnotationView::onObjSelectedFromView);
  emit objViewReady(id);
}

} // namespace nim

