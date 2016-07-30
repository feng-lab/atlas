#include "z3dregionannotationview.h"

#include <cassert>

namespace nim {

Z3DRegionAnnotationView::Z3DRegionAnnotationView(ZRegionAnnotationDoc& doc, Z3DView& view)
  : Z3DFilterView<ZRegionAnnotationDoc, Z3DRegionAnnotationFilter>(doc, view)
{
  docRegionAnnotationsAdded(m_doc.objs());
  connect(&m_doc, &ZRegionAnnotationDoc::objAdded, this, &Z3DRegionAnnotationView::docRegionAnnotationAdded);
}

void Z3DRegionAnnotationView::docRegionAnnotationsAdded(const QList<size_t>& objs)
{
  try {
    for (int i = 0; i < objs.size(); ++i) {
      size_t id = objs[i];
      Z3DRegionAnnotationFilter* viewControl = new Z3DRegionAnnotationFilter(globalParas(), this);
      viewControl->setData(m_doc.regionAnnotation(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id].reset(viewControl);

      viewControl->outputPort("GeometryFilter")->connect(compositor().inputPort("GeometryFilters"));
      connect(viewControl, &Z3DRegionAnnotationFilter::boundBoxChanged, this, &Z3DRegionAnnotationView::updateBoundBox);
      connect(viewControl, &Z3DRegionAnnotationFilter::objDeselected, this,
              &Z3DRegionAnnotationView::onObjDeselectedFromView);
      connect(viewControl, &Z3DRegionAnnotationFilter::objSelected, this,
              &Z3DRegionAnnotationView::onObjSelectedFromView);
      connect(viewControl, &Z3DRegionAnnotationFilter::objVisibleChanged, this,
              &Z3DRegionAnnotationView::onObjVisibleChangedFromView);
      canvas().addEventListenerToBack(viewControl);
    }
    if (!objs.empty()) {
      networkEvaluator().updateNetwork();
      m_view.updateBoundBox();

      for (int i = 0; i < objs.size(); ++i) {
        emit objViewReady(objs[i]);
      }
    }
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to render regionAnnotation: " << e.what();
    QMessageBox::critical(&m_view.canvas(), tr("Failed to render regionAnnotation"), e.what());
  }
}

void Z3DRegionAnnotationView::docRegionAnnotationAdded(size_t id)
{
  try {
    Z3DRegionAnnotationFilter* viewControl = new Z3DRegionAnnotationFilter(globalParas(), this);
    viewControl->setData(m_doc.regionAnnotation(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id].reset(viewControl);

    viewControl->outputPort("GeometryFilter")->connect(compositor().inputPort("GeometryFilters"));
    connect(viewControl, &Z3DRegionAnnotationFilter::boundBoxChanged, this, &Z3DRegionAnnotationView::updateBoundBox);
    connect(viewControl, &Z3DRegionAnnotationFilter::objDeselected, this,
            &Z3DRegionAnnotationView::onObjDeselectedFromView);
    connect(viewControl, &Z3DRegionAnnotationFilter::objSelected, this,
            &Z3DRegionAnnotationView::onObjSelectedFromView);
    connect(viewControl, &Z3DRegionAnnotationFilter::objVisibleChanged, this,
            &Z3DRegionAnnotationView::onObjVisibleChangedFromView);
    canvas().addEventListenerToBack(viewControl);

    networkEvaluator().updateNetwork();
    m_view.updateBoundBox();

    emit objViewReady(id);
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to render regionAnnotation: " << e.what();
    QMessageBox::critical(&m_view.canvas(), tr("Failed to render regionAnnotation"), e.what());
  }
}

} // namespace nim

