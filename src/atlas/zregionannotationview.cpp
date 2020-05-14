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
  for (unsigned long obj : objs) {
    auto viewControl = new ZRegionAnnotationFilter(m_view);
    viewControl->setData(m_doc.regionAnnotationPack(obj));
    expandBoundBox(viewControl->boundBox());
    m_idToFilter[obj].reset(viewControl);
    connect(viewControl, &ZRegionAnnotationFilter::boundBoxChanged, this, &ZRegionAnnotationView::updateBoundBox);
    connect(viewControl, &ZRegionAnnotationFilter::objDeselected, this,
            &ZRegionAnnotationView::onObjDeselectedFromView);
    connect(viewControl, &ZRegionAnnotationFilter::objSelected, this, &ZRegionAnnotationView::onObjSelectedFromView);
    connect(viewControl, &ZRegionAnnotationFilter::objVisibleChanged,
            this, &ZRegionAnnotationView::onObjVisibleChangedFromView);
    emit objViewReady(obj);
  }
  if (!objs.empty()) {
    m_view.updateBoundBox();
  }
}

void ZRegionAnnotationView::docRegionAnnotationAdded(size_t id)
{
  auto viewControl = new ZRegionAnnotationFilter(m_view);
  viewControl->setData(m_doc.regionAnnotationPack(id));
  expandBoundBox(viewControl->boundBox());
  m_idToFilter[id].reset(viewControl);
  m_view.updateBoundBox();
  connect(viewControl, &ZRegionAnnotationFilter::boundBoxChanged, this, &ZRegionAnnotationView::updateBoundBox);
  connect(viewControl, &ZRegionAnnotationFilter::objDeselected, this, &ZRegionAnnotationView::onObjDeselectedFromView);
  connect(viewControl, &ZRegionAnnotationFilter::objSelected, this, &ZRegionAnnotationView::onObjSelectedFromView);
  connect(viewControl, &ZRegionAnnotationFilter::objVisibleChanged, this,
          &ZRegionAnnotationView::onObjVisibleChangedFromView);
  emit objViewReady(id);
}

} // namespace nim

