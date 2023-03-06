#include "z3dregionannotationview.h"

#include <QApplication>

namespace nim {

Z3DRegionAnnotationView::Z3DRegionAnnotationView(ZRegionAnnotationDoc& doc, Z3DRenderingEngine& engine)
  : Z3DFilterView<ZRegionAnnotationDoc, Z3DRegionAnnotationFilter>(doc, engine)
{
  docRegionAnnotationsAdded(m_doc.objs());
  connect(&m_doc, &ZRegionAnnotationDoc::objAdded, this, &Z3DRegionAnnotationView::docRegionAnnotationAdded);
}

void Z3DRegionAnnotationView::docRegionAnnotationsAdded(const std::vector<size_t>& objs)
{
  try {
    for (auto id : objs) {
      auto viewControl = new Z3DRegionAnnotationFilter(m_engine.globalParas(), this);
      viewControl->setData(m_doc.regionAnnotationPack(id));
      viewControl->setSelected(m_doc.isObjSelected(id));
      expandBoundBox(viewControl->axisAlignedBoundBox());
      m_idToFilter[id].reset(viewControl);

      viewControl->outputPort("GeometryFilter")->connect(m_engine.compositor().inputPort("GeometryFilters"));
      connect(viewControl, &Z3DRegionAnnotationFilter::boundBoxChanged, this, &Z3DRegionAnnotationView::updateBoundBox);
      connect(viewControl,
              &Z3DRegionAnnotationFilter::objDeselected,
              this,
              &Z3DRegionAnnotationView::onObjDeselectedFromView);
      connect(viewControl,
              &Z3DRegionAnnotationFilter::objSelected,
              this,
              &Z3DRegionAnnotationView::onObjSelectedFromView);
      connect(viewControl,
              &Z3DRegionAnnotationFilter::objVisibleChanged,
              this,
              &Z3DRegionAnnotationView::onObjVisibleChangedFromView);
      m_engine.addEventListenerToBack(*viewControl);
    }
    if (!objs.empty()) {
      m_engine.networkEvaluator().updateNetwork();
      m_engine.updateBoundBox();

      for (auto id : objs) {
        Q_EMIT objViewReady(id);
      }
    }
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to render regionAnnotation: " << e.what();
    if (m_engine.canvas()) {
      QMessageBox::critical(m_engine.canvas(),
                            QApplication::applicationName(),
                            QString("Failed to render regionAnnotation:\n%1").arg(e.what()));
    }
  }
}

void Z3DRegionAnnotationView::docRegionAnnotationAdded(size_t id)
{
  try {
    auto viewControl = new Z3DRegionAnnotationFilter(m_engine.globalParas(), this);
    viewControl->setData(m_doc.regionAnnotationPack(id));
    viewControl->setSelected(m_doc.isObjSelected(id));
    expandBoundBox(viewControl->axisAlignedBoundBox());
    m_idToFilter[id].reset(viewControl);

    viewControl->outputPort("GeometryFilter")->connect(m_engine.compositor().inputPort("GeometryFilters"));
    connect(viewControl, &Z3DRegionAnnotationFilter::boundBoxChanged, this, &Z3DRegionAnnotationView::updateBoundBox);
    connect(viewControl,
            &Z3DRegionAnnotationFilter::objDeselected,
            this,
            &Z3DRegionAnnotationView::onObjDeselectedFromView);
    connect(viewControl,
            &Z3DRegionAnnotationFilter::objSelected,
            this,
            &Z3DRegionAnnotationView::onObjSelectedFromView);
    connect(viewControl,
            &Z3DRegionAnnotationFilter::objVisibleChanged,
            this,
            &Z3DRegionAnnotationView::onObjVisibleChangedFromView);
    m_engine.addEventListenerToBack(*viewControl);

    m_engine.networkEvaluator().updateNetwork();
    m_engine.updateBoundBox();

    Q_EMIT objViewReady(id);
  }
  catch (const ZException& e) {
    LOG(ERROR) << "Failed to render regionAnnotation: " << e.what();
    if (m_engine.canvas()) {
      QMessageBox::critical(m_engine.canvas(),
                            QApplication::applicationName(),
                            QString("Failed to render regionAnnotation:\n%1").arg(e.what()));
    }
  }
}

} // namespace nim
